#include "vfs.h"
#include "../mem/memory.h"
#include "../../syslibc/string.h"
#include "../../syslibc/stdio.h"

vfs_node_t* vfs_root = NULL;

void vfs_init() {
    vfs_root = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    memset(vfs_root, 0, sizeof(vfs_node_t));
    strcpy(vfs_root->name, "root");
    vfs_root->flags = 0x01; // Папка
}

// Регистрация устройства в корне (упрощенно)
void vfs_register_device(vfs_node_t* node) {
    if (!node) return;
    node->next = vfs_root->next;
    vfs_root->next = node;
}

// Поиск узла по имени
vfs_node_t* vfs_find(const char* name) {
    vfs_node_t* curr = vfs_root->next;
    while (curr) {
        if (strcmp(curr->name, name) == 0) return curr;
        curr = curr->next;
    }
    return NULL;
}

// Системные вызовы (внутриядерные)
uint32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (node && node->write) {
        return node->write(node, offset, size, buffer);
    }
    return 0;
}

extern void term_print(const char* str);

void vfs_ls(void) {
    vfs_node_t* dev = vfs_root->next;
    if (!dev) {
        term_print("VFS: No devices registered.\n");
        return;
    }
    while (dev) {
        term_print("--- Volume: "); term_print(dev->name); term_print(" ---\n");
        if (dev->readdir) {
            for (int i = 0; i < 256; i++) {
                vfs_dirent_t* de = dev->readdir(dev, i);
                if (!de) break;
                term_print("  ");
                term_print(de->name);
                
                char sizebuf[32];
                sprintf(sizebuf, " (%d bytes)\n", de->size);
                term_print(sizebuf);
            }
        } else {
            term_print("  (No readdir support)\n");
        }
        dev = dev->next;
    }
}

uint8_t* vfs_read_file(const char* name, uint32_t* out_size) {
    vfs_node_t* dev = vfs_root->next;
    while (dev) {
        if (dev->readdir && dev->read) {
            for (int i = 0; i < 256; i++) {
                vfs_dirent_t* de = dev->readdir(dev, i);
                if (!de) break;
                if (strcmp(de->name, name) == 0) {
                    *out_size = de->size;
                    uint8_t* buf = kmalloc(*out_size);
                    vfs_node_t file_node;
                    memset(&file_node, 0, sizeof(vfs_node_t));
                    file_node.inode = de->inode;
                    strcpy(file_node.name, de->name);
                    if (dev->read(&file_node, 0, de->size, buf) > 0) return buf;
                    kfree(buf);
                }
            }
        }
        dev = dev->next;
    }
    return NULL;
}

uint32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (node && node->read) {
        return node->read(node, offset, size, buffer);
    }
    return 0;
}