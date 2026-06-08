/*
 * EquinoxOS — Kernel File Descriptor Table (fd.c)
 *
 * Maps POSIX fd integers onto VFS nodes + seek positions.
 * fds 0/1/2 are reserved (stdin/stdout/stderr).
 * All other fds are regular VFS-backed file handles.
 *
 * Thread-safety: none yet — the scheduler is cooperative enough that a
 * spinlock is not needed for the simple sequential workloads we have.
 * Add one if concurrent fd use becomes an issue.
 */

#include "fd.h"
#include "vfs.h"
#include "ext2.h"
#include "../mem/memory.h"
#include "../../syslibc/string.h"
#include "../../syslibc/stdio.h"

extern void term_print(const char *str);

static fd_entry_t fd_table[FD_MAX];

void fd_table_init(void) {
    memset(fd_table, 0, sizeof(fd_table));
    /* Reserve 0/1/2 so they are never handed out by fd_open */
    fd_table[FD_STDIN].used  = true;
    fd_table[FD_STDOUT].used = true;
    fd_table[FD_STDERR].used = true;
}

/* Allocate the lowest free slot >= 3 */
static int alloc_fd(void) {
    for (int i = 3; i < FD_MAX; i++) {
        if (!fd_table[i].used) return i;
    }
    return -1; /* EMFILE */
}

static bool valid_fd(int fd) {
    return (fd >= 3 && fd < FD_MAX && fd_table[fd].used);
}

/* ------------------------------------------------------------------ */

int fd_open(const char *path, int flags) {
    if (!path || path[0] == '\0') return -1;

    int fd = alloc_fd();
    if (fd < 0) return -1;

    fd_entry_t *e = &fd_table[fd];
    memset(e, 0, sizeof(fd_entry_t));

    /* Copy path safely */
    size_t plen = strlen(path);
    if (plen >= sizeof(e->path) - 1) plen = sizeof(e->path) - 1;
    memcpy(e->path, path, plen);
    e->path[plen] = '\0';

    int is_write  = (flags & FD_O_WRONLY) || (flags & FD_O_RDWR);
    int is_append = (flags & FD_O_APPEND) != 0;
    int is_trunc  = (flags & FD_O_TRUNC)  != 0;
    (void)(flags & FD_O_CREAT); /* O_CREAT: file created on close if writing */

    e->is_write  = (bool)is_write;
    e->is_append = (bool)is_append;

    if (!is_write) {
        /* Read mode: load file into buffer via VFS */
        uint32_t size = 0;
        uint8_t *data = vfs_read_file(path, &size);
        if (!data) {
            /* File not found */
            return -1;
        }
        e->buf  = data;
        e->size = size;
        e->pos  = 0;
        e->buf_cap = 0; /* read-only, no cap tracking */
    } else {
        /* Write / create / append mode */
        uint32_t init_cap = 4096;
        e->buf = (uint8_t *)kmalloc(init_cap);
        if (!e->buf) return -1;
        e->buf_cap = init_cap;
        e->size    = 0;
        e->pos     = 0;

        if (is_append && !is_trunc) {
            /* Load existing content so we can append to it */
            uint32_t existing_size = 0;
            uint8_t *existing = vfs_read_file(path, &existing_size);
            if (existing && existing_size > 0) {
                if (existing_size + 1 > init_cap) {
                    uint8_t *nb = (uint8_t *)kmalloc(existing_size + 4096);
                    if (nb) {
                        kfree(e->buf);
                        e->buf = nb;
                        e->buf_cap = existing_size + 4096;
                    }
                }
                memcpy(e->buf, existing, existing_size);
                e->size = existing_size;
                e->pos  = existing_size; /* append: seek to end */
                kfree(existing);
            }
        }
        /* O_CREAT|O_TRUNC: start empty — already the case */
    }

    e->used = true;
    return fd;
}

int fd_read(int fd, void *buf, uint32_t size) {
    if (!valid_fd(fd)) return -1;
    fd_entry_t *e = &fd_table[fd];

    if (e->is_write && !e->is_append) {
        /* Technically could allow reads on O_RDWR but keep simple for now */
    }
    if (!e->buf) return -1;
    if (e->pos >= e->size) return 0; /* EOF */

    uint32_t avail = e->size - e->pos;
    if (size > avail) size = avail;
    memcpy(buf, e->buf + e->pos, size);
    e->pos += size;
    return (int)size;
}

int fd_write(int fd, const void *buf, uint32_t size) {
    if (!valid_fd(fd)) return -1;
    fd_entry_t *e = &fd_table[fd];

    if (!e->is_write) return -1;
    if (!buf || size == 0) return 0;

    uint32_t write_pos = e->is_append ? e->size : e->pos;

    /* Grow buffer if needed */
    if (write_pos + size > e->buf_cap) {
        uint32_t new_cap = write_pos + size + 4096;
        uint8_t *nb = (uint8_t *)kmalloc(new_cap);
        if (!nb) return -1;
        if (e->buf) {
            memcpy(nb, e->buf, e->size);
            kfree(e->buf);
        }
        e->buf     = nb;
        e->buf_cap = new_cap;
    }

    memcpy(e->buf + write_pos, buf, size);
    uint32_t end = write_pos + size;
    if (end > e->size) e->size = end;
    if (!e->is_append) e->pos = end;

    return (int)size;
}

int fd_seek(int fd, int32_t offset, int whence) {
    if (!valid_fd(fd)) return -1;
    fd_entry_t *e = &fd_table[fd];

    int32_t new_pos;
    switch (whence) {
        case 0: new_pos = offset; break;                         /* SEEK_SET */
        case 1: new_pos = (int32_t)e->pos + offset; break;      /* SEEK_CUR */
        case 2: new_pos = (int32_t)e->size + offset; break;     /* SEEK_END */
        default: return -1;
    }
    if (new_pos < 0) new_pos = 0;
    if (new_pos > (int32_t)e->size) new_pos = (int32_t)e->size;
    e->pos = (uint32_t)new_pos;
    return (int)e->pos;
}

int fd_tell(int fd) {
    if (!valid_fd(fd)) return -1;
    return (int)fd_table[fd].pos;
}

int fd_close(int fd) {
    if (!valid_fd(fd)) return -1;
    fd_entry_t *e = &fd_table[fd];

    /* Flush write buffers to VFS */
    if (e->is_write && e->buf && e->size > 0) {
        /* Write the whole buffer to VFS as a single overwrite */
        vfs_node_t *dev = vfs_root->next;
        while (dev) {
            if (dev->write) {
                vfs_node_t file_node;
                memset(&file_node, 0, sizeof(vfs_node_t));
                size_t nl = strlen(e->path);
                if (nl >= sizeof(file_node.name)) nl = sizeof(file_node.name) - 1;
                memcpy(file_node.name, e->path, nl);
                file_node.name[nl] = '\0';
                dev->write(&file_node, 0, e->size, e->buf);
                break;
            }
            dev = dev->next;
        }
    }

    if (e->buf) {
        kfree(e->buf);
        e->buf = NULL;
    }
    memset(e, 0, sizeof(fd_entry_t));
    return 0;
}

int fd_stat(int fd, uint32_t *out_size) {
    if (!valid_fd(fd)) return -1;
    if (out_size) *out_size = fd_table[fd].size;
    return 0;
}

int fd_stat_path(const char *path, uint32_t *out_size) {
    if (!path) return -1;

    /* Search VFS for the file, get its size without fully loading it */
    vfs_node_t *dev = vfs_root->next;
    while (dev) {
        if (dev->finddir) {
            vfs_node_t *fn = dev->finddir(dev, (char *)path);
            if (fn) {
                if (out_size) *out_size = fn->size;
                return 0;
            }
        } else if (dev->readdir) {
            for (int i = 0; i < 512; i++) {
                vfs_dirent_t *de = dev->readdir(dev, i);
                if (!de) break;
                if (strcmp(de->name, path) == 0) {
                    if (out_size) *out_size = de->size;
                    return 0;
                }
            }
        }
        dev = dev->next;
    }
    return -1;
}
