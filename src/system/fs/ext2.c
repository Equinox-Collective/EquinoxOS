#include "ext2.h"
#include "../drivers/hardware/disk/ata.h"
#include "../mem/memory.h"
#include "../drivers/hardware/serial/serial.h"
#include "vfs.h"
#include "../drivers/hardware/serial/serial.h"
#include "../../syslibc/string.h"

extern void term_print(const char* str);
static uint64_t ext2_partition_lba = 0;

// Forward declarations of internal functions
void ext2_read_block(uint32_t block, uint8_t* buffer);
void ext2_write_block(uint32_t block, uint8_t* buffer);
void ext2_read_inode(uint32_t inode, ext2_inode_t* out_inode);
void ext2_write_inode(uint32_t inode, ext2_inode_t* in_inode);
uint32_t ext2_get_inode_block(ext2_inode_t* inode, uint32_t block);
uint32_t ext2_read(uint32_t inode_num, uint32_t offset, uint32_t size, uint8_t* buffer);
uint32_t ext2_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
vfs_dirent_t* ext2_vfs_readdir(vfs_node_t* node, uint32_t index);
vfs_node_t* ext2_vfs_finddir(vfs_node_t* node, char* name);
uint32_t ext2_write(uint32_t inode_num, uint32_t offset, uint32_t size, uint8_t* buffer);
uint32_t ext2_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
uint32_t ext2_allocate_block(void);
uint32_t ext2_allocate_inode(void);
void ext2_add_entry(uint32_t dir_inode_num, uint32_t file_inode, const char* name, uint8_t type);
void ext2_save_bgd(void);

static ext2_superblock_t* sb = NULL;
static ext2_group_desc_t* bgd_table = NULL;
static vfs_inode_ops_t ext2_inode_ops;
static vfs_file_ops_t ext2_file_ops;
static uint32_t block_size = 1024;
static uint32_t groups_count = 0;

static vfs_dirent_t shared_dirent;

uint32_t ext2_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    return ext2_read(node->inode, offset, size, buffer);
}

// === КЭШ ЛИСТИНГА КАТАЛОГА ===
// Раньше ext2_vfs_readdir(index) при КАЖДОМ вызове заново читал inode каталога
// и сканировал его блоки С НАЧАЛА до нужного индекса. getFiles() в enGUI (и
// обработчик SYS_READ_DIR) дёргают readdir(0..N) последовательно -> O(N^2)
// чтений ATA PIO. На whpx каждый опрос порта ATA = дорогой VM-exit, поэтому
// листинг корня (refresh_explorer при старте GUI) занимал ~15 c — это и был
// «провал» между загрузкой ресурсов и первым кадром рабочего стола.
// Теперь каталог читается ОДИН раз в кэш; повторные readdir(index) — O(1).
// Кэш инвалидируется при создании файла (ext2_add_entry) и перезаписи
// (ext2_overwrite), а также при смене каталога (другой inode).
#define EXT2_DIRCACHE_MAX 256
static struct {
    char     name[128];
    uint32_t inode;
    uint32_t size;
} ext2_dir_cache[EXT2_DIRCACHE_MAX];
static uint32_t ext2_dir_cache_count = 0;
static uint32_t ext2_dir_cache_inode = 0; // inode каталога в кэше (0 == пусто)

void ext2_dir_cache_invalidate(void) {
    ext2_dir_cache_inode = 0;
    ext2_dir_cache_count = 0;
}

void ext2_read_block(uint32_t block, uint8_t *buffer) {
    read_sectors_ata_pio((uintptr_t)buffer, 
                         ext2_partition_lba + (uint64_t)block * (block_size / 512), 
                         block_size / 512);
}

static void ext2_build_dir_cache(vfs_node_t* node) {
    ext2_dir_cache_count = 0;
    ext2_dir_cache_inode = node->inode;

    ext2_inode_t dir_inode;
    ext2_read_inode(node->inode, &dir_inode);
    if (!(dir_inode.mode & EXT2_S_IFDIR)) return;

    uint8_t* buffer = kmalloc(block_size);
    for (uint32_t i = 0; i < dir_inode.blocks; i++) {
        uint32_t b = ext2_get_inode_block(&dir_inode, i);
        if (b == 0) break;
        ext2_read_block(b, buffer);

        ext2_dir_entry_t* entry = (ext2_dir_entry_t*)buffer;
        uint32_t offset = 0;
        while (offset < block_size) {
            if (entry->inode != 0 && entry->name_len > 0 && entry->name_len < 128) {
                if (ext2_dir_cache_count < EXT2_DIRCACHE_MAX) {
                    uint32_t k = ext2_dir_cache_count;
                    memcpy(ext2_dir_cache[k].name,
                           (uint8_t*)entry + sizeof(ext2_dir_entry_t), entry->name_len);
                    ext2_dir_cache[k].name[entry->name_len] = '\0';
                    ext2_dir_cache[k].inode = entry->inode;

                    ext2_inode_t file_inode;
                    ext2_read_inode(entry->inode, &file_inode);
                    ext2_dir_cache[k].size = file_inode.size;
                    ext2_dir_cache_count++;
                }
            }
            offset += entry->rec_len;
            if (entry->rec_len == 0) break;
            entry = (ext2_dir_entry_t*)(buffer + offset);
        }
    }
    kfree(buffer);
}

vfs_dirent_t* ext2_vfs_readdir(vfs_node_t* node, uint32_t index) {
    // Перестраиваем кэш только если он от другого каталога или ещё пуст.
    if (ext2_dir_cache_inode != node->inode || ext2_dir_cache_inode == 0) {
        ext2_build_dir_cache(node);
    }
    if (index >= ext2_dir_cache_count) return NULL;

    strcpy(shared_dirent.name, ext2_dir_cache[index].name);
    shared_dirent.inode = ext2_dir_cache[index].inode;
    shared_dirent.size  = ext2_dir_cache[index].size;
    return &shared_dirent;
}

// O(n) поиск файла в каталоге за ОДИН проход по блокам каталога.
/* Этап 8: кольцо узлов вместо одного static — при двух одновременных execve
 * (пайплайн `ls | grep`) планировщик перемежал finddir двух процессов, и
 * второй вызов затирал узел первого ещё до чтения файла. */
static vfs_node_t shared_find_nodes[8];
static uint32_t   shared_find_idx = 0;

/* Внутренняя функция: ищет единственный компонент name в каталоге node. */
static vfs_node_t *ext2_finddir_one(vfs_node_t *node, const char *name) {
    ext2_inode_t dir_inode;
    ext2_read_inode(node->inode, &dir_inode);
    if (!(dir_inode.mode & EXT2_S_IFDIR)) return NULL;

    uint8_t *buffer = kmalloc(block_size);
    uint32_t max_blocks = dir_inode.blocks / (block_size / 512);
    if (max_blocks == 0) max_blocks = 1;

    for (uint32_t i = 0; i < max_blocks; i++) {
        uint32_t b = ext2_get_inode_block(&dir_inode, i);
        if (b == 0) break;
        ext2_read_block(b, buffer);

        ext2_dir_entry_t *entry = (ext2_dir_entry_t *)buffer;
        uint32_t offset = 0;

        while (offset < block_size) {
            if (entry->rec_len == 0) break;
            if (entry->inode != 0 && entry->name_len > 0 && entry->name_len < 128) {
                char ename[128];
                memcpy(ename, (uint8_t *)entry + sizeof(ext2_dir_entry_t), entry->name_len);
                ename[entry->name_len] = '\0';

                if (strcmp(ename, name) == 0) {
                    ext2_inode_t file_inode;
                    ext2_read_inode(entry->inode, &file_inode);

                    vfs_node_t *fn = &shared_find_nodes[shared_find_idx++ & 7u];
                    memset(fn, 0, sizeof(*fn));
                    strncpy(fn->name, ename, sizeof(fn->name) - 1);
                    fn->inode = entry->inode;
                    fn->size  = file_inode.size;
                    fn->flags = (file_inode.mode & EXT2_S_IFDIR) ? VFS_FLAG_DIR : VFS_FLAG_FILE;
                    fn->read  = ext2_vfs_read;
                    fn->write = ext2_vfs_write;

                    fn->inode_ops = &ext2_inode_ops;
                    fn->file_ops  = &ext2_file_ops;

                    kfree(buffer);
                    return fn;
                }
            }
            offset += entry->rec_len;
            entry = (ext2_dir_entry_t *)(buffer + offset);
        }
    }

    kfree(buffer);
    return NULL;
}

/*
 * ext2_vfs_finddir — публичный VFS-хук (v1 поле node->finddir).
 * name может содержать единственный компонент ("bin") или
 * полный подпуть ("bin/snake.elf"). В последнем случае разрезаем и спускаемся.
 */
struct vfs_node *ext2_vfs_finddir(vfs_node_t *node, char *name) {
    if (!name || name[0] == '\0') return NULL;

    /* Нет '/' в имени → одиночный компонент, быстрый путь */
    const char *slash = name;
    while (*slash && *slash != '/') slash++;
    if (*slash == '\0') {
        return ext2_finddir_one(node, name);
    }

    /* Есть '/' → разрезаем и спускаемся */
    char buf[256];
    size_t nl = strlen(name);
    if (nl >= sizeof(buf)) nl = sizeof(buf) - 1;
    memcpy(buf, name, nl);
    buf[nl] = '\0';

    vfs_node_t *cur = node;
    char *token = buf;
    while (token && *token) {
        char *next_slash = token;
        while (*next_slash && *next_slash != '/') next_slash++;
        bool has_more = (*next_slash == '/');
        *next_slash = '\0';
        if (*token == '\0') { token = has_more ? next_slash + 1 : NULL; continue; }
        cur = ext2_finddir_one(cur, token);
        if (!cur) return NULL;
        token = has_more ? next_slash + 1 : NULL;
    }
    return cur;
}

/* v2 inode_ops для ext2 (lookup через finddir) */
static int ext2_v2_lookup(vfs_node_t *dir, const char *name, vfs_node_t **out) {
    vfs_node_t *found = ext2_finddir_one(dir, name);
    if (!found) return -1;
    /* Выделяем отдельную копию, чтобы refcount работал */
    vfs_node_t *copy = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!copy) return -1;
    memcpy(copy, found, sizeof(vfs_node_t));
    copy->refcount = 1;
    *out = copy;
    return 0;
}

static int ext2_v2_create(vfs_node_t *dir, const char *name, uint32_t mode) {
    (void)mode;
    uint32_t ino = ext2_allocate_inode();
    if (!ino) return -1;
    ext2_inode_t new_inode;
    memset(&new_inode, 0, sizeof(ext2_inode_t));
    new_inode.mode = EXT2_S_IFREG | 0644;
    new_inode.links_count = 1;
    ext2_write_inode(ino, &new_inode);
    ext2_add_entry(dir->inode, ino, name, 1);
    ext2_dir_cache_invalidate();
    return 0;
}

static int ext2_v2_mkdir(vfs_node_t *dir, const char *name, uint32_t mode) {
    (void)mode;
    uint32_t ino = ext2_allocate_inode();
    if (!ino) return -1;
    ext2_inode_t new_inode;
    memset(&new_inode, 0, sizeof(ext2_inode_t));
    new_inode.mode = EXT2_S_IFDIR | 0755;
    new_inode.links_count = 2;
    ext2_write_inode(ino, &new_inode);
    ext2_add_entry(dir->inode, ino, name, 2);
    ext2_dir_cache_invalidate();
    return 0;
}

static int ext2_v2_unlink(vfs_node_t *dir, const char *name) {
    (void)dir; (void)name;
    return -1; /* TODO */
}

static int ext2_v2_stat(vfs_node_t *node, vfs_stat_t *st) {
    ext2_inode_t inode;
    ext2_read_inode(node->inode, &inode);
    st->size  = inode.size;
    st->inode = node->inode;
    st->type  = (inode.mode & EXT2_S_IFDIR) ? VFS_FLAG_DIR : VFS_FLAG_FILE;
    return 0;
}

static vfs_inode_ops_t ext2_inode_ops = {
    .lookup = ext2_v2_lookup,
    .create = ext2_v2_create,
    .mkdir  = ext2_v2_mkdir,
    .unlink = ext2_v2_unlink,
    .stat   = ext2_v2_stat,
};

/* v2 file_ops для ext2 (обёртка вокруг ext2_read/ext2_write) */
static uint32_t ext2_file_ops_read(vfs_file_t *f, uint32_t off, uint32_t sz, uint8_t *buf) {
    return ext2_read(f->node->inode, off, sz, buf);
}
static uint32_t ext2_file_ops_write(vfs_file_t *f, uint32_t off, uint32_t sz, uint8_t *buf) {
    return ext2_write(f->node->inode, off, sz, buf);
}
static vfs_dirent_t *ext2_file_ops_readdir(vfs_file_t *f, uint32_t idx) {
    return ext2_vfs_readdir(f->node, idx);
}

static vfs_file_ops_t ext2_file_ops = {
    .read    = ext2_file_ops_read,
    .write   = ext2_file_ops_write,
    .readdir = ext2_file_ops_readdir,
};

vfs_node_t* ext2_get_root_node() {
    if (!sb) return NULL;
    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    memset(node, 0, sizeof(vfs_node_t));
    strcpy(node->name, "ext2");
    node->inode      = 2;              /* EXT2 root inode */
    node->flags      = VFS_FLAG_DIR;
    node->refcount   = 1;
    /* v2 ops */
    node->inode_ops  = &ext2_inode_ops;
    node->file_ops   = &ext2_file_ops;
    /* v1 legacy ops (используются fd.c, vfs_read_file fallback) */
    node->read    = ext2_vfs_read;
    node->readdir = ext2_vfs_readdir;
    node->finddir = ext2_vfs_finddir;
    node->write   = ext2_vfs_write;
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
    
    /* Этап 8: локальный токенайзер вместо strtok — strtok держит общее
     * статическое состояние, и при двух одновременных execve (пайплайн
     * `ls | grep`) процессы затирали его друг другу: оба resolve падали. */
    char path_copy[256];
    size_t plen = strlen(path);
    if (plen >= sizeof(path_copy)) return 0;
    memcpy(path_copy, path, plen + 1);

    char *p = path_copy;
    while (*p) {
        while (*p == '/') p++;                 /* пропустить разделители */
        if (!*p) break;
        char *token = p;
        while (*p && *p != '/') p++;
        if (*p) { *p = '\0'; p++; }

        ext2_read_inode(current_inode_num, &current_inode);
        if (!(current_inode.mode & EXT2_S_IFDIR)) return 0;

        current_inode_num = ext2_find_entry(&current_inode, token);
        if (current_inode_num == 0) return 0;
    }

    return current_inode_num;
}

void ext2_write_block(uint32_t block, uint8_t* buffer) {
    write_sectors_ata_pio((uintptr_t)buffer, 
                          ext2_partition_lba + (uint64_t)block * (block_size / 512), 
                          block_size / 512);
}

void ext2_set_partition(uint64_t lba) {
    ext2_partition_lba = lba;
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

// Преобразует диапазон ЛОГИЧЕСКИХ блоков файла в ФИЗИЧЕСКИЕ номера блоков,
// КЭШируя indirect/doubly-indirect блоки. Раньше ext2_get_inode_block для
// КАЖДОГО блока данных заново читал indirect-блок (kmalloc+read+kfree) — при
// block_size=4096 файл 4.5МБ лежит в single-indirect, и тот же indirect-блок
// перечитывался ~1024 раза. Тут читаем каждый indirect/doubly максимум 1 раз
// подряд (последовательный доступ). count*4 байт под результат.
static void ext2_map_blocks(ext2_inode_t* inode, uint32_t start,
                            uint32_t count, uint32_t* out) {
    uint32_t ppb = block_size / 4; // указателей в блоке
    uint32_t* ind = kmalloc(block_size); // кэш single-indirect
    uint32_t* dbl = kmalloc(block_size); // кэш doubly-indirect
    uint32_t ind_cached = 0; // какой физ. блок сейчас в ind[] (0 == пусто)
    uint32_t dbl_cached = 0;

    for (uint32_t k = 0; k < count; k++) {
        uint32_t block = start + k;

        if (block < 12) { out[k] = inode->block[block]; continue; }
        uint32_t b = block - 12;

        if (b < ppb) { // single indirect
            uint32_t iblk = inode->block[12];
            if (iblk == 0) { out[k] = 0; continue; }
            if (ind_cached != iblk) { ext2_read_block(iblk, (uint8_t*)ind); ind_cached = iblk; }
            out[k] = ind[b];
            continue;
        }
        b -= ppb;

        if (b < ppb * ppb) { // doubly indirect
            uint32_t dblk = inode->block[13];
            if (dblk == 0) { out[k] = 0; continue; }
            if (dbl_cached != dblk) { ext2_read_block(dblk, (uint8_t*)dbl); dbl_cached = dblk; }
            uint32_t idx = b / ppb;
            uint32_t iblk = dbl[idx];
            if (iblk == 0) { out[k] = 0; continue; }
            if (ind_cached != iblk) { ext2_read_block(iblk, (uint8_t*)ind); ind_cached = iblk; }
            out[k] = ind[b % ppb];
            continue;
        }

        // triply indirect не поддерживается
        out[k] = 0;
    }

    kfree(ind);
    kfree(dbl);
}

uint32_t ext2_read(uint32_t inode_num, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!sb) return 0;
    ext2_inode_t inode;
    ext2_read_inode(inode_num, &inode);

    if (offset >= inode.size) return 0;
    if (offset + size > inode.size) size = inode.size - offset;
    if (size == 0) return 0;

    uint32_t bs  = block_size;     // реальный размер блока (статик, как в ext2_read_block)
    uint32_t spb = bs / 512;       // секторов в блоке

    uint32_t first_block = offset / bs;
    uint32_t last_block  = (offset + size - 1) / bs;
    uint32_t nblocks     = last_block - first_block + 1;

    // Карта физических блоков (с кэшем indirect).
    uint32_t* phys = kmalloc(nblocks * sizeof(uint32_t));
    ext2_map_blocks(&inode, first_block, nblocks, phys);

    uint8_t* tmp = NULL; // ленивый буфер под частичные блоки/дыры
    uint32_t bytes_read = 0;

    for (uint32_t i = 0; i < nblocks; ) {
        uint32_t logical    = first_block + i;
        uint64_t blk_start  = (uint64_t)logical * bs;          // смещение начала блока в файле
        uint32_t in_off     = (uint32_t)((offset + bytes_read) - blk_start); // !=0 только для первого
        uint32_t avail      = bs - in_off;
        uint32_t remaining  = size - bytes_read;
        uint32_t to_copy    = (avail < remaining) ? avail : remaining;
        uint32_t b          = phys[i];

        // Быстрый путь: целый блок с начала, не дыра — батчим непрерывный прогон
        // физически соседних блоков в ОДНО чтение ATA прямо в buffer.
        if (in_off == 0 && to_copy == bs && b != 0) {
            uint32_t run = 1;
            while (i + run < nblocks
                   && phys[i + run] == b + run
                   && (size - (bytes_read + (uint64_t)run * bs)) >= bs) {
                run++;
            }
            read_sectors_ata_pio((uintptr_t)(buffer + bytes_read),
                                 (uint64_t)b * spb, run * spb);
            bytes_read += run * bs;
            i += run;
            continue;
        }

        // Медленный путь: частичный блок (голова/хвост) или дыра.
        if (b == 0) {
            memset(buffer + bytes_read, 0, to_copy);
        } else {
            if (!tmp) tmp = kmalloc(bs);
            ext2_read_block(b, tmp);
            memcpy(buffer + bytes_read, tmp + in_off, to_copy);
        }
        bytes_read += to_copy;
        i++;
    }

    if (tmp) kfree(tmp);
    kfree(phys);
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
    /* Этап 8: образ у нас ПЛОСКИЙ (WINDOWS_ext2.py кладёт все файлы в корень
     * с полным путём в имени: «bin/foo.elf», «tmp/x»). resolve_path такие
     * имена не находит, поэтому дополнительно ищем плоскую запись в корне —
     * иначе каждый rewrite плодил дубликат, а чтение видело старый inode. */
    if (existing_ino == 0) {
        ext2_inode_t root_inode;
        ext2_read_inode(2, &root_inode);
        existing_ino = ext2_find_entry(&root_inode, name + 1);
    }
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

        /* Плоская запись в корне с полным путём в имени — как делает packer. */
        ext2_add_entry(2, ino, name + 1, 1);
        ext2_save_bgd();
    } else {
        ext2_inode_t inode;
        ext2_read_inode(ino, &inode);
        inode.size = 0;
        ext2_write_inode(ino, &inode);
    }
    
    ext2_write(ino, 0, size, (uint8_t*)data);

    // Содержимое/размер каталога могли измениться — сбрасываем кэш листинга,
    // чтобы refresh_explorer увидел новый/изменённый файл.
    extern void ext2_dir_cache_invalidate(void);
    ext2_dir_cache_invalidate();
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
