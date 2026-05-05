#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

struct vfs_node;
typedef struct vfs_node vfs_node_t;

typedef struct vfs_dirent {
    char name[128];
    uint32_t inode;
    uint32_t size;
} vfs_dirent_t;

typedef uint32_t (*vfs_read_t)(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer);
typedef uint32_t (*vfs_write_t)(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer);
typedef struct vfs_dirent* (*vfs_readdir_t)(struct vfs_node* node, uint32_t index);
typedef struct vfs_node* (*vfs_finddir_t)(struct vfs_node* node, char* name);

typedef struct vfs_node {
    char name[128];
    uint32_t flags;
    uint32_t inode; // FS-specific inode number
    uint32_t size;
    vfs_read_t read;
    vfs_write_t write;
    vfs_readdir_t readdir;
    vfs_finddir_t finddir;
    struct vfs_node* next;
} vfs_node_t;

// Глобальный корень системы
extern vfs_node_t* vfs_root;

void vfs_init();
void vfs_register_device(vfs_node_t* node);
void vfs_ls(void);
uint8_t* vfs_read_file(const char* name, uint32_t* out_size);
uint32_t vfs_write_file(const char* name, const uint8_t* data, uint32_t size,
                        const char** out_device_name);

#endif
