#include "ext2.h"
#include "../drivers/hardware/disk/ata.h"
#include "../mem/memory.h"
#include "../drivers/hardware/serial/serial.h"
#include "vfs.h"
#include "../drivers/hardware/serial/serial.h"
#include "../../syslibc/string.h"

extern void term_print(const char* str);

// Forward declarations of internal functions
void ext2_read_block(uint32_t block, uint8_t* buffer);
void ext2_write_block(uint32_t block, uint8_t* buffer);
void ext2_read_inode(uint32_t inode, ext2_inode_t* out_inode);
void ext2_write_inode(uint32_t inode, ext2_inode_t* in_inode);
uint32_t ext2_get_inode_block(ext2_inode_t* inode, uint32_t block);
uint32_t ext2_read(uint32_t inode_num, uint32_t offset, uint32_t size, uint8_t* buffer);
uint32_t ext2_write(uint32_t inode_num, uint32_t offset, uint32_t size, uint8_t* buffer);
uint32_t ext2_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
uint32_t ext2_allocate_block(void);
uint32_t ext2_allocate_inode(void);
void ext2_add_entry(uint32_t dir_inode_num, uint32_t file_inode, const char* name, uint8_t type);
void ext2_save_bgd(void);

static ext2_superblock_t* sb = NULL;
static ext2_group_desc_t* bgd_table = NULL;
static uint32_t block_size = 1024;
static uint32_t groups_count = 0;

static vfs_dirent_t shared_dirent;

uint32_t ext2_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    return ext2_read(node->inode, offset, size, buffer);
}

struct vfs_dirent* ext2_vfs_readdir(vfs_node_t* node, uint32_t index) {
    ext2_inode_t dir_inode;
    ext2_read_inode(node->inode, &dir_inode);
    
    if (!(dir_inode.mode & EXT2_S_IFDIR)) return NULL;
    
    uint8_t* buffer = kmalloc(block_size);
    uint32_t current_idx = 0;
    
    for (uint32_t i = 0; i < dir_inode.blocks; i++) {
        uint32_t b = ext2_get_inode_block(&dir_inode, i);
        if (b == 0) break;
        ext2_read_block(b, buffer);
        
        ext2_dir_entry_t* entry = (ext2_dir_entry_t*)buffer;
        uint32_t offset = 0;
        
        while (offset < block_size) {
            if (entry->inode != 0) {
                if (current_idx == index) {
                    memcpy(shared_dirent.name, (uint8_t*)entry + sizeof(ext2_dir_entry_t), entry->name_len);
                    shared_dirent.name[entry->name_len] = '\0';
                    shared_dirent.inode = entry->inode;
                    
                    ext2_inode_t file_inode;
                    ext2_read_inode(entry->inode, &file_inode);
                    shared_dirent.size = file_inode.size;
                    
                    kfree(buffer);
                    return &shared_dirent;
                }
                current_idx++;
            }
            offset += entry->rec_len;
            if (entry->rec_len == 0) break;
            entry = (ext2_dir_entry_t*)(buffer + offset);
        }
    }
    kfree(buffer);
    return NULL;
}

vfs_node_t* ext2_get_root_node() {
    if (!sb) return NULL;
    vfs_node_t* node = kmalloc(sizeof(vfs_node_t));
    memset(node, 0, sizeof(vfs_node_t));
    strcpy(node->name, "EXT2_DISK");
    node->inode = 2; // Root inode
    node->flags = 0x01; // Directory
    node->read = ext2_vfs_read;
    node->readdir = ext2_vfs_readdir;
    node->write = ext2_vfs_write;
    return node;
}

void ext2_read_block(uint32_t block, uint8_t *buffer) {
  // ВАЖНО: Сначала буфер, потом LBA, потом количество секторов
  read_sectors_ata_pio((uintptr_t)buffer, block * (block_size / 512),
                       block_size / 512);
}

void ext2_read_inode(uint32_t inode, ext2_inode_t* out_inode) {
    if (!sb || inode == 0) {
        memset(out_inode, 0, sizeof(ext2_inode_t));
        return;
    }

    uint32_t group = (inode - 1) / sb->inodes_per_group;
    if (group >= groups_count) {
        memset(out_inode, 0, sizeof(ext2_inode_t));
        return;
    }

    uint32_t index = (inode - 1) % sb->inodes_per_group;
    
    uint32_t inode_table_block = bgd_table[group].inode_table;
    uint32_t offset = index * sb->inode_size;
    
    uint8_t* buffer = kmalloc(block_size);
    ext2_read_block(inode_table_block + (offset / block_size), buffer);
    
    // Safety check: Don't overflow our struct if disk inode is larger
    uint32_t copy_size = (sb->inode_size < sizeof(ext2_inode_t)) ? sb->inode_size : sizeof(ext2_inode_t);
    // Если копируем меньше, чем размер структуры (старая ФС со 128-байтным
    // inode при 256-байтной in-memory структуре), хвост out_inode будет
    // содержать мусор со стека вызывающего → ext2_get_inode_block уйдёт
    // читать «индирект» по случайному адресу диска. Обнуляем заранее.
    memset(out_inode, 0, sizeof(ext2_inode_t));
    memcpy(out_inode, buffer + (offset % block_size), copy_size);
    
    kfree(buffer);
}

uint32_t ext2_get_inode_block(ext2_inode_t* inode, uint32_t block) {
    uint32_t p_per_block = block_size / 4; // Pointers per block

    // Direct blocks
    if (block < 12) {
        return inode->block[block];
    }
    block -= 12;

    // Indirect block
    if (block < p_per_block) {
        if (inode->block[12] == 0) return 0;
        uint32_t* indirect = kmalloc(block_size);
        ext2_read_block(inode->block[12], (uint8_t*)indirect);
        uint32_t res = indirect[block];
        kfree(indirect);
        return res;
    }
    block -= p_per_block;

    // Doubly indirect block
    if (block < p_per_block * p_per_block) {
        if (inode->block[13] == 0) return 0;
        uint32_t* doubly = kmalloc(block_size);
        ext2_read_block(inode->block[13], (uint8_t*)doubly);
        
        uint32_t indirect_idx = block / p_per_block;
        if (doubly[indirect_idx] == 0) {
            kfree(doubly);
            return 0;
        }
        uint32_t* indirect = kmalloc(block_size);
        ext2_read_block(doubly[indirect_idx], (uint8_t*)indirect);
        
        uint32_t res = indirect[block % p_per_block];
        kfree(indirect);
        kfree(doubly);
        return res;
    }
    block -= p_per_block * p_per_block;

    // Triply indirect block
    // TODO: Triple indirect is rarely needed but can be added similarly
    return 0;
}

uint32_t ext2_find_entry(ext2_inode_t* dir_inode, const char* name) {
    uint8_t* buffer = kmalloc(block_size);
    
    for (uint32_t i = 0; i < dir_inode->blocks; i++) {
        uint32_t b = ext2_get_inode_block(dir_inode, i);
        if (b == 0) break;
        
        ext2_read_block(b, buffer);
        
        ext2_dir_entry_t* entry = (ext2_dir_entry_t*)buffer;
        uint32_t offset = 0;
        
        while (offset < block_size) {
            if (entry->rec_len == 0) break;

            if (entry->inode != 0 && entry->name_len < 255) {
                char entry_name[256];
                memcpy(entry_name, (uint8_t*)entry + sizeof(ext2_dir_entry_t), entry->name_len);
                entry_name[entry->name_len] = '\0';

                if (strcmp(entry_name, name) == 0) {
                    uint32_t ino = entry->inode;
                    kfree(buffer);
                    return ino;
                }
            }
            
            offset += entry->rec_len;
            entry = (ext2_dir_entry_t*)(buffer + offset);
        }
    }
    
    kfree(buffer);
    return 0;
}

uint32_t ext2_resolve_path(const char* path) {
    if (!sb || !path) return 0;
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) return 2;

    uint32_t current_inode_num = 2; // Start at root
    ext2_inode_t current_inode;
    
    char* path_copy = kmalloc(strlen(path) + 1);
    strcpy(path_copy, path);
    
    char* token = strtok(path_copy, "/");
    while (token != NULL) {
        ext2_read_inode(current_inode_num, &current_inode);
        if (!(current_inode.mode & EXT2_S_IFDIR)) {
            kfree(path_copy);
            return 0;
        }
        
        current_inode_num = ext2_find_entry(&current_inode, token);
        if (current_inode_num == 0) {
            kfree(path_copy);
            return 0;
        }
        
        token = strtok(NULL, "/");
    }
    
    kfree(path_copy);
    return current_inode_num;
}

void ext2_write_block(uint32_t block, uint8_t* buffer) {
    // ВАЖНО: Сначала адрес буфера, потом LBA, потом количество
    write_sectors_ata_pio((uintptr_t)buffer, block * (block_size / 512), block_size / 512);
}

void ext2_write_inode(uint32_t inode, ext2_inode_t* in_inode) {
    if (!sb) return;

    uint32_t group = (inode - 1) / sb->inodes_per_group;
    uint32_t index = (inode - 1) % sb->inodes_per_group;
    
    uint32_t inode_table_block = bgd_table[group].inode_table;
    uint32_t offset = index * sb->inode_size;
    
    uint8_t* buffer = kmalloc(block_size);
    ext2_read_block(inode_table_block + (offset / block_size), buffer);
    
    uint32_t copy_size = (sb->inode_size < sizeof(ext2_inode_t)) ? sb->inode_size : sizeof(ext2_inode_t);
    memcpy(buffer + (offset % block_size), in_inode, copy_size);
    
    ext2_write_block(inode_table_block + (offset / block_size), buffer);
    kfree(buffer);
}

void ext2_save_bgd() {
  uint32_t bgd_block = (block_size == 1024) ? 2 : 1;
  uint32_t bgd_table_size = groups_count * sizeof(ext2_group_desc_t);
  uint32_t sectors_to_write = (bgd_table_size + 511) / 512;

  // БЫЛО: write_sectors_ata_pio(LBA, Count, Buffer)
  // СТАЛО: write_sectors_ata_pio(Buffer, LBA, Count)
  write_sectors_ata_pio((uintptr_t)bgd_table,
                        (uint64_t)bgd_block * (block_size / 512),
                        sectors_to_write);
}

void ext2_add_entry(uint32_t dir_inode_num, uint32_t file_inode, const char* name, uint8_t type) {
    ext2_inode_t dir_inode;
    ext2_read_inode(dir_inode_num, &dir_inode);
    
    uint8_t* buffer = kmalloc(block_size);
    uint32_t name_len = strlen(name);
    uint32_t required_len = sizeof(ext2_dir_entry_t) + name_len;
    // Round to 4 byte boundary
    required_len = (required_len + 3) & ~3;

    for (uint32_t i = 0; i < dir_inode.blocks; i++) {
        uint32_t b = ext2_get_inode_block(&dir_inode, i);
        if (b == 0) break;
        ext2_read_block(b, buffer);
        
        ext2_dir_entry_t* entry = (ext2_dir_entry_t*)buffer;
        uint32_t offset = 0;
        
        while (offset < block_size) {
            uint32_t actual_entry_len = (sizeof(ext2_dir_entry_t) + entry->name_len + 3) & ~3;
            uint32_t space_available = entry->rec_len - actual_entry_len;
            
            if (space_available >= required_len) {
                // Resize current entry
                uint16_t old_rec_len = entry->rec_len;
                entry->rec_len = actual_entry_len;
                
                // Add new entry
                offset += actual_entry_len;
                ext2_dir_entry_t* new_entry = (ext2_dir_entry_t*)(buffer + offset);
                new_entry->inode = file_inode;
                new_entry->rec_len = old_rec_len - actual_entry_len;
                new_entry->name_len = name_len;
                new_entry->file_type = type;
                memcpy((uint8_t*)new_entry + sizeof(ext2_dir_entry_t), name, name_len);
                
                ext2_write_block(b, buffer);
                kfree(buffer);
                return;
            }
            
            offset += entry->rec_len;
            if (entry->rec_len == 0) break;
            entry = (ext2_dir_entry_t*)(buffer + offset);
        }
    }
    
    // TODO: If no space found, allocate new block for directory
    kfree(buffer);
}

uint32_t ext2_read(uint32_t inode_num, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!sb) return 0;
    ext2_inode_t inode;
    ext2_read_inode(inode_num, &inode);
    
    if (offset >= inode.size) return 0;
    if (offset + size > inode.size) size = inode.size - offset;
    
    uint32_t block_size = 1024 << sb->log_block_size;
    uint8_t* block_buf = kmalloc(block_size);
    uint32_t bytes_read = 0;
    
    while (bytes_read < size) {
        uint32_t block_index = (offset + bytes_read) / block_size;
        uint32_t block_offset = (offset + bytes_read) % block_size;
        uint32_t b = ext2_get_inode_block(&inode, block_index);
        
        if (b == 0) {
            memset(block_buf, 0, block_size);
        } else {
            ext2_read_block(b, block_buf);
        }
        uint32_t to_copy = block_size - block_offset;
        if (to_copy > size - bytes_read) to_copy = size - bytes_read;
        
        memcpy(buffer + bytes_read, block_buf + block_offset, to_copy);
        bytes_read += to_copy;
    }
    
    kfree(block_buf);
    return bytes_read;
}

uint32_t ext2_write(uint32_t inode_num, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!sb) return 0;
    ext2_inode_t inode;
    ext2_read_inode(inode_num, &inode);
    
    uint32_t b_size = 1024 << sb->log_block_size;
    uint8_t* block_buf = kmalloc(b_size);
    uint32_t bytes_written = 0;
    
    while (bytes_written < size) {
        uint32_t block_index = (offset + bytes_written) / b_size;
        uint32_t block_offset = (offset + bytes_written) % b_size;
        
        uint32_t b = ext2_get_inode_block(&inode, block_index);
        if (b == 0) {
            b = ext2_allocate_block();
            if (b == 0) break;
            
            if (block_index < 12) {
                inode.block[block_index] = b;
                inode.blocks += (b_size / 512); 
            } else {
                break;
            }
        }
        
        ext2_read_block(b, block_buf);
        uint32_t to_copy = b_size - block_offset;
        if (to_copy > size - bytes_written) to_copy = size - bytes_written;
        
        memcpy(block_buf + block_offset, buffer + bytes_written, to_copy);
        ext2_write_block(b, block_buf);
        
        bytes_written += to_copy;
        if (offset + bytes_written > inode.size) {
            inode.size = offset + bytes_written;
        }
    }
    
    ext2_write_inode(inode_num, &inode);
    kfree(block_buf);
    return bytes_written;
}

void ext2_overwrite(const char* name, const char* data, uint32_t size) {
    uint32_t existing_ino = ext2_resolve_path(name);
    uint32_t ino = existing_ino;
    
    if (ino == 0) {
        ino = ext2_allocate_inode();
        if (ino == 0) return;

        ext2_inode_t new_inode;
        memset(&new_inode, 0, sizeof(ext2_inode_t));
        new_inode.mode = EXT2_S_IFREG | 0644;
        new_inode.size = 0;
        new_inode.links_count = 1;
        ext2_write_inode(ino, &new_inode);
        
        ext2_add_entry(2, ino, name + 1, 1);
        ext2_save_bgd();
    } else {
        ext2_inode_t inode;
        ext2_read_inode(ino, &inode);
        inode.size = 0;
        ext2_write_inode(ino, &inode);
    }
    
    ext2_write(ino, 0, size, (uint8_t*)data);
}

uint32_t ext2_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!sb) return 0;
    (void)offset;

    char path[130];
    path[0] = '/';
    strcpy(path + 1, node->name);
    ext2_overwrite(path, (char*)buffer, size);
    return size;
}

uint32_t ext2_allocate_block() {
    for (uint32_t i = 0; i < groups_count; i++) {
        if (bgd_table[i].free_blocks_count > 0) {
            uint8_t* bitmap = kmalloc(block_size);
            ext2_read_block(bgd_table[i].block_bitmap, bitmap);
            
            for (uint32_t j = 0; j < block_size; j++) {
                if (bitmap[j] != 0xFF) {
                    for (int bit = 0; bit < 8; bit++) {
                        if (!(bitmap[j] & (1 << bit))) {
                            bitmap[j] |= (1 << bit);
                            ext2_write_block(bgd_table[i].block_bitmap, bitmap);
                            bgd_table[i].free_blocks_count--;
                            if (sb->free_blocks_count > 0) sb->free_blocks_count--;
                            ext2_save_bgd();
                            
                            kfree(bitmap);
                            return i * sb->blocks_per_group + (j * 8 + bit) + sb->first_data_block;
                        }
                    }
                }
            }
            kfree(bitmap);
        }
    }
    return 0;
}

uint32_t ext2_allocate_inode() {
    for (uint32_t i = 0; i < groups_count; i++) {
        if (bgd_table[i].free_inodes_count > 0) {
            uint8_t* bitmap = kmalloc(block_size);
            ext2_read_block(bgd_table[i].inode_bitmap, bitmap);
            
            for (uint32_t j = 0; j < block_size; j++) {
                if (bitmap[j] != 0xFF) {
                    for (int bit = 0; bit < 8; bit++) {
                        if (!(bitmap[j] & (1 << bit))) {
                            bitmap[j] |= (1 << bit);
                            ext2_write_block(bgd_table[i].inode_bitmap, bitmap);
                            bgd_table[i].free_inodes_count--;
                            if (sb->free_inodes_count > 0) sb->free_inodes_count--;
                            ext2_save_bgd();

                            kfree(bitmap);
                            return i * sb->inodes_per_group + (j * 8 + bit) + 1;
                        }
                    }
                }
            }
            kfree(bitmap);
        }
    }
    return 0;
}

void ext2_init() {
    term_print("EXT2: Initializing...\n");

    // Superblock is always at 1024 bytes offset (LBA 2 if sector size is 512)
    uint8_t* buffer = kmalloc(1024);
    if (!buffer) {
        term_print("EXT2: Failed to allocate memory for superblock buffer\n");
        return;
    }

    // Read sectors 2 and 3 (1024 bytes)
    read_sectors_ata_pio((uintptr_t)buffer, 2, 2);

    sb = (ext2_superblock_t*)kmalloc(sizeof(ext2_superblock_t));
    memcpy(sb, buffer, sizeof(ext2_superblock_t));
    kfree(buffer);

    // Validate Magic
    if (sb->magic != EXT2_MAGIC) {
        term_print("EXT2: Invalid magic number! Not an EXT2 filesystem.\n");
        kfree(sb);
        sb = NULL;
        return;
    }

    block_size = 1024 << sb->log_block_size;
    groups_count = (sb->blocks_count + sb->blocks_per_group - 1) / sb->blocks_per_group;

    term_print("EXT2: Mounted successfully!\n");
    // Log info to serial for debugging
    serial_puts(COM1, "EXT2: Block size: ");
    serial_puts(COM1, (block_size == 1024) ? "1024\n" : "Other\n");
    serial_puts(COM1, "EXT2: Inode size: ");
    if (sb->inode_size == 128) serial_puts(COM1, "128\n");
    else if (sb->inode_size == 256) serial_puts(COM1, "256\n");
    else serial_puts(COM1, "Other\n");
    
    // Read Block Group Descriptor Table
    uint32_t bgd_block = (block_size == 1024) ? 2 : 1;
    uint32_t bgd_table_size = groups_count * sizeof(ext2_group_desc_t);
    uint32_t sectors_to_read = (bgd_table_size + 511) / 512;

    bgd_table = (ext2_group_desc_t*)kmalloc(sectors_to_read * 512);
    read_sectors_ata_pio((uintptr_t)bgd_table, bgd_block * (block_size / 512), sectors_to_read);

    term_print("EXT2: Read Group Descriptor Table.\n");
}
