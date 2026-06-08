#ifndef FD_H
#define FD_H

/*
 * EquinoxOS — Kernel File Descriptor Table
 *
 * Provides a per-process (actually global for now, per-task later) fd table
 * that maps POSIX file descriptors (int) to VFS nodes + seek positions.
 *
 * fds 0/1/2 = stdin/stdout/stderr (special — handled by write path)
 * fds 3..FD_MAX-1 = regular files from VFS
 */

#include <stdint.h>
#include <stdbool.h>
#include "../fs/vfs.h"

#define FD_MAX       64     /* max open files per process (global table for now) */
#define FD_STDIN     0
#define FD_STDOUT    1
#define FD_STDERR    2

/* Open flags — mirror what SDK fcntl.h declares */
#define FD_O_RDONLY  0x0000
#define FD_O_WRONLY  0x0001
#define FD_O_RDWR    0x0002
#define FD_O_CREAT   0x0040
#define FD_O_TRUNC   0x0200
#define FD_O_APPEND  0x0400

typedef struct {
    bool      used;
    bool      is_write;     /* opened for writing */
    bool      is_append;    /* O_APPEND: writes go to end */
    uint32_t  inode;        /* EXT2 inode number (0 = not from ext2) */
    uint32_t  size;         /* file size at open time (updated on write) */
    uint32_t  pos;          /* current seek position */
    char      path[128];    /* original path (for re-write) */
    /* Pointer to the owning VFS device node — needed for read/write ops */
    vfs_node_t *dev;
    /* For files loaded fully into memory (read-only fast path) */
    uint8_t  *buf;          /* non-NULL: data is in this malloc'd buffer */
    uint32_t  buf_cap;      /* allocated capacity (write mode) */
} fd_entry_t;

/* Initialise the fd table (called from kernel init once) */
void fd_table_init(void);

/*
 * Open a file by path.
 * Returns fd >= 3 on success, -1 on error.
 */
int  fd_open(const char *path, int flags);

/*
 * Read up to `size` bytes into `buf` from fd at current position.
 * Returns bytes read, 0 at EOF, -1 on error.
 */
int  fd_read(int fd, void *buf, uint32_t size);

/*
 * Write `size` bytes from `buf` to fd at current position (or end if APPEND).
 * Returns bytes written, -1 on error.
 */
int  fd_write(int fd, const void *buf, uint32_t size);

/*
 * Seek within fd.  whence: 0=SET, 1=CUR, 2=END.
 * Returns new position, -1 on error.
 */
int  fd_seek(int fd, int32_t offset, int whence);

/*
 * Return current position.
 */
int  fd_tell(int fd);

/*
 * Close fd and release resources.
 */
int  fd_close(int fd);

/*
 * Stat: fill *out_size with file size.  Returns 0 ok, -1 error.
 */
int  fd_stat(int fd, uint32_t *out_size);

/*
 * Stat by path (without opening).
 */
int  fd_stat_path(const char *path, uint32_t *out_size);

#endif /* FD_H */
