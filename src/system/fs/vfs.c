/*
 * EquinoxOS — Virtual File System v2.0 (vfs.c)
 *
 * Реализует:
 *   - Mount table: до VFS_MAX_MOUNTS точек монтирования. Каждая точка указывает на root dentry.
 *   - Dentry Cache (dcache): Быстрый поиск компонентов пути без диска.
 *   - Inode Cache / Refcounting: Уникальные vfs_node_t.
 *   - devfs: /dev/null, /dev/zero, /dev/tty
 */

#include "vfs.h"
#include "../mem/memory.h"
#include "../../syslibc/string.h"
#include "../../syslibc/stdio.h"

extern void term_print(const char *str);

/* ================================================================== */
/*  Глобальные                                                         */
/* ================================================================== */

static vfs_mount_t mount_table[VFS_MAX_MOUNTS];
static int         mount_count = 0;

#define VFS_MAX_OPEN 128
static vfs_file_t file_pool[VFS_MAX_OPEN];

/* Legacy */
vfs_node_t *vfs_root = NULL;

/* Dentry Root */
static vfs_dentry_t *vfs_root_dentry = NULL;

/* ================================================================== */
/*  Утилиты                                                            */
/* ================================================================== */

static void path_normalize(const char *src, char *dst, int dstsz) {
    int di = 0;
    int si = 0;
    while (src[si] && di < dstsz - 1) {
        if (src[si] == '/' && di > 0 && dst[di-1] == '/') {
            si++; continue;
        }
        dst[di++] = src[si++];
    }
    if (di > 1 && dst[di-1] == '/') di--;
    dst[di] = '\0';
    if (di == 0) { dst[0] = '/'; dst[1] = '\0'; }
}

/* ================================================================== */
/*  Dentry и Nodes                                                     */
/* ================================================================== */

vfs_node_t *vfs_alloc_node(void) {
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    memset(node, 0, sizeof(vfs_node_t));
    node->refcount = 1;
    return node;
}

void vfs_ref_node(vfs_node_t *node) {
    if (node) node->refcount++;
}

void vfs_unref_node(vfs_node_t *node) {
    if (node) {
        if (node->refcount > 0) node->refcount--;
        if (node->refcount == 0) {
            kfree(node);
        }
    }
}

static vfs_dentry_t *alloc_dentry(const char *name, vfs_node_t *node, vfs_dentry_t *parent) {
    vfs_dentry_t *d = (vfs_dentry_t *)kmalloc(sizeof(vfs_dentry_t));
    memset(d, 0, sizeof(vfs_dentry_t));
    strncpy(d->name, name, sizeof(d->name) - 1);
    d->node = node;
    d->parent = parent;
    d->refcount = 1;
    if (node) vfs_ref_node(node);
    
    if (parent) {
        d->sibling = parent->child;
        parent->child = d;
    }
    return d;
}

static vfs_dentry_t *find_dentry(vfs_dentry_t *parent, const char *name) {
    if (!parent) return NULL;
    for (vfs_dentry_t *child = parent->child; child; child = child->sibling) {
        if (strcmp(child->name, name) == 0) return child;
    }
    return NULL;
}

static vfs_dentry_t *vfs_resolve_path_parent(const char *raw_path, char *last_comp) {
    char path[256];
    path_normalize(raw_path, path, sizeof(path));
    
    const char *p = path;
    while (*p == '/') p++;
    
    /* FLAT FALLBACK CHECK:
     * Наш образ диска ПЛОСКИЙ (WINDOWS_ext2.py кладет файлы вида "bin/sysgui.elf" в корень).
     * Если путь p целиком существует как плоский файл в корневом dentry ФС, 
     * возвращаем корень в качестве родителя, а весь относительный путь p — как имя файла. */
    if (*p != '\0' && vfs_root_dentry && vfs_root_dentry->node) {
        if (vfs_root_dentry->node->inode_ops && vfs_root_dentry->node->inode_ops->lookup) {
            vfs_node_t *new_node = NULL;
            if (vfs_root_dentry->node->inode_ops->lookup(vfs_root_dentry->node, p, &new_node) == 0 && new_node) {
                vfs_unref_node(new_node); // Нам нужно было только проверить существование
                if (last_comp) strcpy(last_comp, p);
                return vfs_root_dentry;
            }
        }
    }
    
    vfs_dentry_t *current = vfs_root_dentry;
    if (!current) return NULL;
    
    char comp[128];
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < 127) comp[i++] = *p++;
        comp[i] = '\0';
        while (*p == '/') p++;
        
        if (*p == '\0') {
            if (last_comp) strcpy(last_comp, comp);
            return current;
        }
        
        vfs_dentry_t *next = find_dentry(current, comp);
        if (!next) {
            if (!current->node || !current->node->inode_ops || !current->node->inode_ops->lookup) return NULL;
            vfs_node_t *new_node = NULL;
            if (current->node->inode_ops->lookup(current->node, comp, &new_node) == 0 && new_node) {
                next = alloc_dentry(comp, new_node, current);
                vfs_unref_node(new_node); 
            } else {
                return NULL;
            }
        }
        current = next;
    }
    
    if (last_comp) last_comp[0] = '\0';
    return current;
}

static vfs_dentry_t *vfs_resolve_path(const char *raw_path) {
    char last_comp[128];
    vfs_dentry_t *parent = vfs_resolve_path_parent(raw_path, last_comp);
    if (!parent) return NULL;
    
    if (last_comp[0] == '\0') return parent; 
    
    vfs_dentry_t *next = find_dentry(parent, last_comp);
    if (!next) {
        if (!parent->node || !parent->node->inode_ops || !parent->node->inode_ops->lookup) return NULL;
        vfs_node_t *new_node = NULL;
        if (parent->node->inode_ops->lookup(parent->node, last_comp, &new_node) == 0 && new_node) {
            next = alloc_dentry(last_comp, new_node, parent);
            vfs_unref_node(new_node);
        }
    }
    return next;
}

/* ================================================================== */
/*  VFS init / mount                                                   */
/* ================================================================== */

void vfs_init(void) {
    memset(mount_table, 0, sizeof(mount_table));
    mount_count = 0;
    memset(file_pool, 0, sizeof(file_pool));

    vfs_root = vfs_alloc_node();
    strcpy(vfs_root->name, "root");
    vfs_root->flags = VFS_FLAG_DIR;

    if (!vfs_root_dentry) {
        vfs_root_dentry = alloc_dentry("/", NULL, NULL);
    }

    term_print("[VFS v2] Initialized\n");
}

int vfs_mount(const char *mountpoint, vfs_node_t *root_node) {
    if (!mountpoint || !root_node) return -1;
    
    if (!vfs_root_dentry) {
        vfs_root_dentry = alloc_dentry("/", NULL, NULL);
    }
    
    vfs_dentry_t *target_dentry = NULL;

    if (strcmp(mountpoint, "/") == 0) {
        if (vfs_root_dentry->node) vfs_unref_node(vfs_root_dentry->node);
        vfs_root_dentry->node = root_node;
        vfs_ref_node(root_node);
        target_dentry = vfs_root_dentry;
    } else {
        const char *name = mountpoint;
        if (name[0] == '/') name++;
        
        vfs_dentry_t *d = find_dentry(vfs_root_dentry, name);
        if (d) {
            if (d->node) vfs_unref_node(d->node);
            d->node = root_node;
            vfs_ref_node(root_node);
            target_dentry = d;
        } else {
            target_dentry = alloc_dentry(name, root_node, vfs_root_dentry);
        }
    }
    
    int idx = mount_count++;
    mount_table[idx].used = true;
    strncpy(mount_table[idx].mountpoint, mountpoint, sizeof(mount_table[idx].mountpoint) - 1);
    mount_table[idx].root_dentry = target_dentry;
    
    char buf[64];
    sprintf(buf, "[VFS] Mounted %s\n", mountpoint);
    term_print(buf);
    return 0;
}

int vfs_umount(const char *mountpoint) {
    if (!mountpoint) return -1;
    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].used && strcmp(mount_table[i].mountpoint, mountpoint) == 0) {
            mount_table[i].used = false;
            return 0;
        }
    }
    return -1;
}

/* ================================================================== */
/*  vfs_open / vfs_close / vfs_read / vfs_write / vfs_seek            */
/* ================================================================== */

static vfs_file_t *alloc_file(void) {
    for (int i = 0; i < VFS_MAX_OPEN; i++) {
        if (!file_pool[i].used) {
            memset(&file_pool[i], 0, sizeof(vfs_file_t));
            file_pool[i].used = true;
            return &file_pool[i];
        }
    }
    return NULL;
}

vfs_file_t *vfs_open(const char *path, int flags) {
    if (!path || path[0] == '\0') return NULL;
    
    char last_comp[128];
    vfs_dentry_t *parent = vfs_resolve_path_parent(path, last_comp);
    if (!parent) return NULL;
    
    vfs_dentry_t *dentry = NULL;
    if (last_comp[0] == '\0') {
        dentry = parent;
    } else {
        dentry = find_dentry(parent, last_comp);
        if (!dentry) {
            if (parent->node && parent->node->inode_ops) {
                vfs_node_t *new_node = NULL;
                int ret = -1;
                if (parent->node->inode_ops->lookup) {
                    ret = parent->node->inode_ops->lookup(parent->node, last_comp, &new_node);
                }
                
                if (ret == 0 && new_node) {
                    dentry = alloc_dentry(last_comp, new_node, parent);
                    vfs_unref_node(new_node);
                } else if ((flags & VFS_O_CREAT) || (flags & VFS_O_WRONLY) || (flags & VFS_O_RDWR)) {
                    if (parent->node->inode_ops->create) {
                        ret = parent->node->inode_ops->create(parent->node, last_comp, 0);
                        if (ret == 0 && parent->node->inode_ops->lookup) {
                            if (parent->node->inode_ops->lookup(parent->node, last_comp, &new_node) == 0 && new_node) {
                                dentry = alloc_dentry(last_comp, new_node, parent);
                                vfs_unref_node(new_node);
                            }
                        }
                    }
                }
            }
        }
    }
    
    if (!dentry || !dentry->node) return NULL;
    
    vfs_file_t *f = alloc_file();
    if (!f) return NULL;
    
    f->dentry = dentry;
    f->node = dentry->node;
    f->pos = 0;
    f->flags = flags;
    strncpy(f->path, path, sizeof(f->path) - 1);
    
    if (flags & VFS_O_APPEND) {
        f->pos = f->node->size;
    }
    
    return f;
}

void vfs_close(vfs_file_t *f) {
    if (!f || !f->used) return;
    f->used = false;
    f->node = NULL;
    f->dentry = NULL;
}

int vfs_read(vfs_file_t *f, void *buf, uint32_t size) {
    if (!f || !f->used || !buf || size == 0) return 0;
    if ((f->flags & 3) == VFS_O_WRONLY) return -1;
    if (!f->node->file_ops || !f->node->file_ops->read) return -1;
    
    int n = f->node->file_ops->read(f, f->pos, size, (uint8_t *)buf);
    if (n > 0) f->pos += n;
    return n;
}

int vfs_write(vfs_file_t *f, const void *buf, uint32_t size) {
    if (!f || !f->used || !buf || size == 0) return 0;
    if ((f->flags & 3) == VFS_O_RDONLY) return -1;
    if (!f->node->file_ops || !f->node->file_ops->write) return -1;
    
    int n = f->node->file_ops->write(f, f->pos, size, (uint8_t *)buf);
    if (n > 0) {
        f->pos += n;
        if (f->pos > f->node->size) f->node->size = f->pos;
    }
    return n;
}

int vfs_seek(vfs_file_t *f, int32_t offset, int whence) {
    if (!f || !f->used) return -1;
    int32_t new_pos;
    switch (whence) {
        case 0: new_pos = offset; break;              /* SEEK_SET */
        case 1: new_pos = (int32_t)f->pos + offset; break; /* SEEK_CUR */
        case 2: new_pos = (int32_t)f->node->size + offset; break; /* SEEK_END */
        default: return -1;
    }
    if (new_pos < 0) new_pos = 0;
    f->pos = (uint32_t)new_pos;
    return (int)f->pos;
}

int vfs_fstat(vfs_file_t *f, vfs_stat_t *out) {
    if (!f || !f->used || !out) return -1;
    if (f->node->inode_ops && f->node->inode_ops->stat) {
        return f->node->inode_ops->stat(f->node, out);
    }
    out->size  = f->node->size;
    out->type  = (uint8_t)f->node->flags;
    out->inode = f->node->inode;
    return 0;
}

int vfs_stat(const char *path, vfs_stat_t *out) {
    if (!path || !out) return -1;
    vfs_dentry_t *dentry = vfs_resolve_path(path);
    if (!dentry || !dentry->node) return -1;
    
    if (dentry->node->inode_ops && dentry->node->inode_ops->stat) {
        return dentry->node->inode_ops->stat(dentry->node, out);
    }
    out->size  = dentry->node->size;
    out->type  = (uint8_t)dentry->node->flags;
    out->inode = dentry->node->inode;
    return 0;
}

vfs_dirent_t *vfs_readdir(const char *path, uint32_t index) {
    if (!path) return NULL;
    vfs_file_t *f = vfs_open(path, VFS_O_RDONLY);
    if (!f) return NULL;
    
    vfs_dirent_t *res = NULL;
    if (f->node->file_ops && f->node->file_ops->readdir) {
        res = f->node->file_ops->readdir(f, index);
    }
    vfs_close(f);
    return res;
}

int vfs_mkdir(const char *path) {
    if (!path) return -1;
    char last_comp[128];
    vfs_dentry_t *parent = vfs_resolve_path_parent(path, last_comp);
    if (!parent || !parent->node || !parent->node->inode_ops || !parent->node->inode_ops->mkdir) return -1;
    return parent->node->inode_ops->mkdir(parent->node, last_comp, 0);
}

int vfs_unlink(const char *path) {
    if (!path) return -1;
    char last_comp[128];
    vfs_dentry_t *parent = vfs_resolve_path_parent(path, last_comp);
    if (!parent || !parent->node || !parent->node->inode_ops || !parent->node->inode_ops->unlink) return -1;
    
    int res = parent->node->inode_ops->unlink(parent->node, last_comp);
    if (res == 0) {
        // Remove from dcache
        vfs_dentry_t *d = find_dentry(parent, last_comp);
        if (d) {
            // Unlink sibling
            if (parent->child == d) {
                parent->child = d->sibling;
            } else {
                vfs_dentry_t *curr = parent->child;
                while (curr && curr->sibling != d) curr = curr->sibling;
                if (curr) curr->sibling = d->sibling;
            }
            vfs_unref_node(d->node);
            kfree(d);
        }
    }
    return res;
}

/* ================================================================== */
/*  devfs                                                              */
/* ================================================================== */

static uint32_t dev_null_read(vfs_file_t *f, uint32_t off, uint32_t sz, uint8_t *buf) {
    (void)f; (void)off; (void)sz; (void)buf; return 0;
}
static uint32_t dev_null_write(vfs_file_t *f, uint32_t off, uint32_t sz, uint8_t *buf) {
    (void)f; (void)off; (void)buf; return sz;
}
static uint32_t dev_zero_read(vfs_file_t *f, uint32_t off, uint32_t sz, uint8_t *buf) {
    (void)f; (void)off;
    if (buf && sz) memset(buf, 0, sz);
    return sz;
}
static uint32_t dev_tty_read(vfs_file_t *f, uint32_t off, uint32_t sz, uint8_t *buf) {
    (void)f; (void)off; (void)sz; (void)buf; return 0;
}
static uint32_t dev_tty_write(vfs_file_t *f, uint32_t off, uint32_t sz, uint8_t *buf) {
    (void)f; (void)off;
    if (!buf || sz == 0) return 0;
    char tmp[512];
    uint32_t to_print = (sz < sizeof(tmp) - 1) ? sz : sizeof(tmp) - 1;
    memcpy(tmp, buf, to_print);
    tmp[to_print] = '\0';
    term_print(tmp);
    return sz;
}

static vfs_file_ops_t dev_null_ops = { .read = dev_null_read, .write = dev_null_write, .readdir = NULL };
static vfs_file_ops_t dev_zero_ops = { .read = dev_zero_read, .write = dev_null_write, .readdir = NULL };
static vfs_file_ops_t dev_tty_ops  = { .read = dev_tty_read,  .write = dev_tty_write,  .readdir = NULL };

static const struct {
    const char *name;
    vfs_file_ops_t *ops;
} devfs_entries[] = {
    { "null", &dev_null_ops },
    { "zero", &dev_zero_ops },
    { "tty",  &dev_tty_ops  },
};

static int devfs_lookup(vfs_node_t *dir, const char *name, vfs_node_t **out_node) {
    for (int i = 0; i < 3; i++) {
        if (strcmp(devfs_entries[i].name, name) == 0) {
            vfs_node_t *n = vfs_alloc_node();
            strcpy(n->name, name);
            n->flags = VFS_FLAG_CHARDEV;
            n->inode = i + 1;
            n->file_ops = devfs_entries[i].ops;
            *out_node = n;
            return 0;
        }
    }
    return -1;
}

static vfs_dirent_t devfs_shared_dirent;
static vfs_dirent_t *devfs_readdir(vfs_file_t *file, uint32_t index) {
    if (index >= 3) return NULL;
    strcpy(devfs_shared_dirent.name, devfs_entries[index].name);
    devfs_shared_dirent.inode = index + 1;
    devfs_shared_dirent.size = 0;
    devfs_shared_dirent.type = VFS_FLAG_CHARDEV;
    return &devfs_shared_dirent;
}

static vfs_inode_ops_t devfs_inode_ops = {
    .lookup = devfs_lookup,
    .create = NULL,
    .mkdir  = NULL,
    .unlink = NULL,
    .stat   = NULL
};

static vfs_file_ops_t devfs_dir_ops = {
    .read = NULL,
    .write = NULL,
    .readdir = devfs_readdir
};

vfs_node_t *devfs_create_root(void) {
    vfs_node_t *root = vfs_alloc_node();
    strcpy(root->name, "dev");
    root->flags = VFS_FLAG_DIR;
    root->inode_ops = &devfs_inode_ops;
    root->file_ops = &devfs_dir_ops;
    return root;
}

/* ================================================================== */
/*  Legacy helpers                                                     */
/* ================================================================== */

uint8_t *vfs_read_file(const char *path, uint32_t *out_size) {
    if (!path || !out_size) return NULL;

    /* --- v2 path: через dentry/inode_ops --- */
    vfs_file_t *f = vfs_open(path, VFS_O_RDONLY);
    if (f) {
        uint32_t sz = f->node->size;
        *out_size = sz;
        if (sz == 0) { vfs_close(f); return NULL; }
        uint8_t *buf = (uint8_t *)kmalloc(sz);
        if (!buf) { vfs_close(f); return NULL; }
        vfs_read(f, buf, sz);
        vfs_close(f);
        return buf;
    }

    /* --- v1 fallback: через legacy linked list (ext2 finddir) ---
     * Убираем ведущий '/' перед поиском, т.к. ext2 хранит плоские имена
     * вида "bin/foo.elf" без ведущего слеша. */
    const char *flat = path;
    while (*flat == '/') flat++;
    if (*flat == '\0') return NULL;

    vfs_node_t *dev = vfs_root ? vfs_root->next : NULL;
    while (dev) {
        if (dev->finddir) {
            vfs_node_t *fn = dev->finddir(dev, (char *)flat);
            if (fn && fn->read && fn->size > 0) {
                uint32_t sz = fn->size;
                *out_size = sz;
                uint8_t *buf = (uint8_t *)kmalloc(sz);
                if (!buf) return NULL;
                fn->read(fn, 0, sz, buf);
                return buf;
            }
        }
        dev = dev->next;
    }
    return NULL;
}

int vfs_write_file(const char *path, const uint8_t *data, uint32_t size) {
    vfs_file_t *f = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (!f) return -1;
    int written = vfs_write(f, data, size);
    vfs_close(f);
    return written;
}

void vfs_register_device(vfs_node_t *node) {
    if (!node) return;
    /* Добавляем в конец списка vfs_root->next (v1 legacy). */
    if (!vfs_root->next) {
        vfs_root->next = node;
    } else {
        vfs_node_t *cur = vfs_root->next;
        while (cur->next) cur = cur->next;
        cur->next = node;
    }
    node->next = NULL;
}

/* ================================================================== */
/*  Debug                                                              */
/* ================================================================== */

void vfs_dump_mounts(void) {
    term_print("[VFS] Mount table:\n");
    for (int i = 0; i < mount_count; i++) {
        if (!mount_table[i].used) continue;
        char buf[128];
        sprintf(buf, "  [%d] %s -> %s\n", i,
                mount_table[i].mountpoint,
                mount_table[i].root_dentry ? mount_table[i].root_dentry->node->name : "(null)");
        term_print(buf);
    }
}

void vfs_ls(void) {
    term_print("[VFS] Directory listing not implemented fully in v2 debug helper.\n");
}

/* ===== Этап 3: нормализация путей и проверка каталогов (cwd) ============= */

int vfs_normalize(const char* cwd, const char* path, char* out, int outsz) {
    if (!out || outsz < 2) return -1;
    /* 1. Собираем "сырой" абсолютный путь во временном буфере. */
    char raw[512];
    int n = 0;
    if (path && path[0] == '/') {
        /* абсолютный — cwd игнорируется */
    } else {
        const char* c = (cwd && cwd[0]) ? cwd : "/";
        for (int i = 0; c[i] && n < (int)sizeof(raw) - 1; i++) raw[n++] = c[i];
        if (n == 0 || raw[n - 1] != '/') {
            if (n < (int)sizeof(raw) - 1) raw[n++] = '/';
        }
    }
    if (path) {
        for (int i = 0; path[i] && n < (int)sizeof(raw) - 1; i++) raw[n++] = path[i];
    }
    raw[n] = '\0';

    /* 2. Токенизируем по '/', сворачивая "." и "..". Стек компонент в out. */
    /* offs[] хранит позиции начала каждой компоненты в out (для pop ".."). */
    int offs[64];
    int depth = 0;
    int olen = 0;
    out[0] = '\0';
    int i = 0;
    while (raw[i]) {
        while (raw[i] == '/') i++;          /* пропускаем слэши */
        if (!raw[i]) break;
        int start = i;
        while (raw[i] && raw[i] != '/') i++;
        int complen = i - start;
        if (complen == 1 && raw[start] == '.') continue;          /* "." */
        if (complen == 2 && raw[start] == '.' && raw[start + 1] == '.') {
            if (depth > 0) { depth--; olen = offs[depth]; out[olen] = '\0'; }
            continue;                                              /* ".." */
        }
        if (depth >= 64) return -1;
        if (olen + 1 + complen >= outsz) return -1;
        offs[depth++] = olen;
        out[olen++] = '/';
        for (int k = 0; k < complen; k++) out[olen++] = raw[start + k];
        out[olen] = '\0';
    }
    if (olen == 0) { out[0] = '/'; out[1] = '\0'; olen = 1; }
    return olen;
}

int vfs_dir_exists(const char* logical) {
    if (!logical) return 0;
    if (logical[0] == '/' && logical[1] == '\0') return 1;     /* корень */

    /* Сначала пробуем v2-путь через dentry-дерево. */
    vfs_dentry_t *d = vfs_resolve_path(logical);
    if (d && d->node && (d->node->flags & VFS_FLAG_DIR)) return 1;

    /* Фоллбэк: плоский ext2-namespace ("bin/foo.elf").
     * Логический путь "/bin" -> prefix "bin/". Если хотя бы одна запись
     * в v1-списке начинается на этот префикс — каталог «существует». */
    const char* p = logical;
    while (*p == '/') p++;
    if (*p == '\0') return 1;   /* "/" уже обработан выше */

    char prefix[258];
    int n = 0;
    for (; p[n] && n < (int)sizeof(prefix) - 2; n++) prefix[n] = p[n];
    prefix[n++] = '/';
    prefix[n] = '\0';

    vfs_node_t* dev = vfs_root ? vfs_root->next : NULL;
    while (dev) {
        if (dev->readdir) {
            for (int idx = 0; idx < 1024; idx++) {
                vfs_dirent_t* de = (vfs_dirent_t*)dev->readdir(dev, idx);
                if (!de) break;
                /* запись начинается на prefix? */
                int m = 0;
                while (prefix[m] && de->name[m] == prefix[m]) m++;
                if (prefix[m] == '\0') return 1;
            }
        }
        dev = dev->next;
    }
    return 0;
}