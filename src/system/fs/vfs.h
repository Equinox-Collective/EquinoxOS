#ifndef VFS_H
#define VFS_H

/*
 * EquinoxOS — Virtual File System v2.0
 *
 * Архитектура:
 *   - Дерево маунтов: каждый маунт привязан к пути (например "/", "/fat", "/dev")
 *   - Dentry Cache: кэширование путей в памяти для O(1) доступа
 *   - Inode Cache / Refcounting: уникальные ноды в памяти
 *   - Strict Operations: vfs_inode_ops_t и vfs_file_ops_t
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Типы нод                                                            */
/* ------------------------------------------------------------------ */

#define VFS_FLAG_FILE      0x00
#define VFS_FLAG_DIR       0x01
#define VFS_FLAG_CHARDEV   0x02
#define VFS_FLAG_BLOCKDEV  0x03
#define VFS_FLAG_SYMLINK   0x04
#define VFS_FLAG_MOUNTPT   0x08

/* Флаги открытия */
#define VFS_O_RDONLY   0x0000
#define VFS_O_WRONLY   0x0001
#define VFS_O_RDWR     0x0002
#define VFS_O_CREAT    0x0040
#define VFS_O_TRUNC    0x0200
#define VFS_O_APPEND   0x0400

/* ------------------------------------------------------------------ */
/* Структуры                                                           */
/* ------------------------------------------------------------------ */

struct vfs_node;
struct vfs_dentry;
struct vfs_file;

typedef struct vfs_node vfs_node_t;
typedef struct vfs_dentry vfs_dentry_t;
typedef struct vfs_file vfs_file_t;

/* Запись в директории (возвращается readdir) */
typedef struct {
    char     name[128];
    uint32_t inode;
    uint32_t size;
    uint8_t  type;       /* VFS_FLAG_* */
} vfs_dirent_t;

/* Статическая информация о файле */
typedef struct {
    uint32_t size;
    uint8_t  type;       /* VFS_FLAG_* */
    uint32_t inode;
} vfs_stat_t;

/* Операции с файлом (открытым) */
typedef struct {
    uint32_t (*read)   (vfs_file_t *file, uint32_t offset, uint32_t size, uint8_t *buf);
    uint32_t (*write)  (vfs_file_t *file, uint32_t offset, uint32_t size, uint8_t *buf);
    vfs_dirent_t* (*readdir)(vfs_file_t *file, uint32_t idx);
} vfs_file_ops_t;

/* Операции с нодой (директорией/метаданными) */
typedef struct {
    int (*lookup) (vfs_node_t *dir, const char *name, vfs_node_t **out_node);
    int (*create) (vfs_node_t *dir, const char *name, uint32_t mode);
    int (*mkdir)  (vfs_node_t *dir, const char *name, uint32_t mode);
    int (*unlink) (vfs_node_t *dir, const char *name);
    int (*stat)   (vfs_node_t *node, vfs_stat_t *st);
} vfs_inode_ops_t;

/* VFS-нода: единица файловой системы */
struct vfs_node {
    char              name[128];   /* имя файла/директории */
    uint32_t          flags;       /* VFS_FLAG_* */
    uint32_t          inode;       /* FS-specific inode */
    uint32_t          size;        /* размер в байтах */
    uint32_t          refcount;    /* счетчик ссылок */

    vfs_inode_ops_t  *inode_ops;
    vfs_file_ops_t   *file_ops;

    /* Данные бэкенда (опционально, например указатель на devops) */
    void             *impl;

    /* Legacy-список (для devfs) - TODO: удалить в будущем */
    struct vfs_node  *next;
};

/* Dentry: элемент кэша путей */
struct vfs_dentry {
    char              name[128];
    vfs_node_t       *node;        /* ассоциированная нода */
    vfs_dentry_t     *parent;      /* родительский dentry */
    vfs_dentry_t     *sibling;     /* следующий элемент в папке */
    vfs_dentry_t     *child;       /* первый ребенок */
    uint32_t          refcount;
};

/* Открытый файловый хендл */
struct vfs_file {
    bool           used;
    vfs_dentry_t  *dentry;      /* dentry этого хендла */
    vfs_node_t    *node;        /* прямой указатель на ноду для скорости */
    uint32_t       pos;         /* текущая позиция */
    int            flags;       /* VFS_O_* флаги открытия */
    char           path[256];   /* исходный путь */
};

/* ------------------------------------------------------------------ */
/* Mount table                                                         */
/* ------------------------------------------------------------------ */

#define VFS_MAX_MOUNTS  16

typedef struct {
    char          mountpoint[256]; /* "/", "/fat", "/dev" */
    vfs_dentry_t *root_dentry;     /* корневой dentry смонтированной ФС */
    bool          used;
} vfs_mount_t;

/* ------------------------------------------------------------------ */
/* Публичный API                                                       */
/* ------------------------------------------------------------------ */

void vfs_init(void);

/* Управление нодами (вызывается драйверами) */
vfs_node_t *vfs_alloc_node(void);
void vfs_ref_node(vfs_node_t *node);
void vfs_unref_node(vfs_node_t *node);

/* Монтирование */
int  vfs_mount(const char *mountpoint, vfs_node_t *root_node);
int  vfs_umount(const char *mountpoint);

/* Работа с файлами */
vfs_file_t *vfs_open(const char *path, int flags);
void vfs_close(vfs_file_t *f);
int  vfs_read(vfs_file_t *f, void *buf, uint32_t size);
int  vfs_write(vfs_file_t *f, const void *buf, uint32_t size);
int  vfs_seek(vfs_file_t *f, int32_t offset, int whence);
int  vfs_fstat(vfs_file_t *f, vfs_stat_t *out);

/* Работа с путями (без открытия) */
int  vfs_stat(const char *path, vfs_stat_t *out);
vfs_dirent_t *vfs_readdir(const char *path, uint32_t index);
int  vfs_mkdir(const char *path);
int  vfs_unlink(const char *path);

/* Отладка */
void vfs_dump_mounts(void);
void vfs_ls(void);

/* ------------------------------------------------------------------ */
/* Legacy / compatibility                                              */
/* ------------------------------------------------------------------ */

uint8_t *vfs_read_file(const char *path, uint32_t *out_size);
int vfs_write_file(const char *path, const uint8_t *data, uint32_t size);
void vfs_register_device(vfs_node_t *node);

extern vfs_node_t *vfs_root;

/* ------------------------------------------------------------------ */
/* devfs                                                               */
/* ------------------------------------------------------------------ */

vfs_node_t *devfs_create_root(void);

#endif /* VFS_H */

