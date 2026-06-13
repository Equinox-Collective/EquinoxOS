/*
 * EquinoxOS — Kernel File Descriptor Table (fd.c)
 *
 * Maps POSIX fd integers (3..FD_MAX-1) to vfs_file_t handles.
 * fds 0/1/2 = stdin/stdout/stderr (reserved).
 *
 * All I/O goes through the new VFS (vfs_open / vfs_read / vfs_write /
 * vfs_seek / vfs_close). fd.c no longer touches ext2 directly.
 */

#include "fd.h"
#include "vfs.h"
#include "../mem/memory.h"
#include "../../syslibc/string.h"
#include "../../syslibc/stdio.h"

extern void term_print(const char *str);

/* ------------------------------------------------------------------ */
/* FD table: каждый слот хранит указатель на открытый VFS-хендл       */
/* ------------------------------------------------------------------ */

typedef struct {
    bool        used;
    vfs_file_t *vf;       /* открытый VFS-хендл */
    char        path[256]; /* копия пути для stat и отладки */
} fd_slot_t;

static fd_slot_t fd_table[FD_MAX];

void fd_table_init(void) {
    memset(fd_table, 0, sizeof(fd_table));
    /* Резервируем 0/1/2 — они никогда не выдаются fd_open */
    fd_table[FD_STDIN].used  = true;
    fd_table[FD_STDOUT].used = true;
    fd_table[FD_STDERR].used = true;
}

static int alloc_fd(void) {
    for (int i = 3; i < FD_MAX; i++) {
        if (!fd_table[i].used) return i;
    }
    return -1;
}

static bool valid_fd(int fd) {
    return (fd >= 3 && fd < FD_MAX && fd_table[fd].used);
}

/* ------------------------------------------------------------------ */

int fd_open(const char *path, int flags) {
    if (!path || path[0] == '\0') return -1;

    int fd = alloc_fd();
    if (fd < 0) return -1;

    /* Транслируем флаги fd.h → vfs.h (они идентичны по значениям) */
    int vfs_flags = flags;

    vfs_file_t *vf = vfs_open(path, vfs_flags);
    if (!vf) return -1;

    fd_slot_t *slot = &fd_table[fd];
    slot->used = true;
    slot->vf   = vf;

    size_t plen = strlen(path);
    if (plen >= sizeof(slot->path)) plen = sizeof(slot->path) - 1;
    memcpy(slot->path, path, plen);
    slot->path[plen] = '\0';

    return fd;
}

int fd_read(int fd, void *buf, uint32_t size) {
    if (!valid_fd(fd)) return -1;
    return vfs_read(fd_table[fd].vf, buf, size);
}

int fd_write(int fd, const void *buf, uint32_t size) {
    if (!valid_fd(fd)) return -1;
    return vfs_write(fd_table[fd].vf, buf, size);
}

int fd_seek(int fd, int32_t offset, int whence) {
    if (!valid_fd(fd)) return -1;
    return vfs_seek(fd_table[fd].vf, offset, whence);
}

int fd_tell(int fd) {
    if (!valid_fd(fd)) return -1;
    return (int)fd_table[fd].vf->pos;
}

int fd_close(int fd) {
    if (!valid_fd(fd)) return -1;
    vfs_close(fd_table[fd].vf);
    memset(&fd_table[fd], 0, sizeof(fd_slot_t));
    return 0;
}

int fd_stat(int fd, uint32_t *out_size) {
    if (!valid_fd(fd)) return -1;
    vfs_stat_t st;
    if (vfs_fstat(fd_table[fd].vf, &st) < 0) return -1;
    if (out_size) *out_size = st.size;
    return 0;
}

int fd_stat_path(const char *path, uint32_t *out_size) {
    if (!path) return -1;
    vfs_stat_t st;
    if (vfs_stat(path, &st) < 0) return -1;
    if (out_size) *out_size = st.size;
    return 0;
}
