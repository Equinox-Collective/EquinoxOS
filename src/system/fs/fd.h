#ifndef FD_H
#define FD_H

/*
 * EquinoxOS — Kernel File Descriptor Table
 *
 * Тонкая обёртка над VFS: int fd → vfs_file_t*.
 * fd 0/1/2 = stdin/stdout/stderr (зарезервированы, не выдаются fd_open).
 * fd 3..FD_MAX-1 = обычные файлы через VFS.
 */

#include <stdint.h>
#include <stdbool.h>

#define FD_MAX       64
#define FD_STDIN     0
#define FD_STDOUT    1
#define FD_STDERR    2

/* Флаги открытия (совпадают с VFS_O_* в vfs.h) */
#define FD_O_RDONLY  0x0000
#define FD_O_WRONLY  0x0001
#define FD_O_RDWR    0x0002
#define FD_O_CREAT   0x0040
#define FD_O_TRUNC   0x0200
#define FD_O_APPEND  0x0400

/* Инициализация таблицы (вызывается один раз из kmain) */
void fd_table_init(void);

/* Открыть файл по пути. Возвращает fd >= 3 или -1 при ошибке. */
int  fd_open(const char *path, int flags);

/* Читать до size байт из fd. Возвращает прочитанное, 0 = EOF, -1 = ошибка. */
int  fd_read(int fd, void *buf, uint32_t size);

/* Писать size байт в fd. Возвращает записанное, -1 = ошибка. */
int  fd_write(int fd, const void *buf, uint32_t size);

/* Seek. whence: 0=SET, 1=CUR, 2=END. Возвращает новую позицию или -1. */
int  fd_seek(int fd, int32_t offset, int whence);

/* Текущая позиция. */
int  fd_tell(int fd);

/* Закрыть fd, сбросить write-буферы на диск. */
int  fd_close(int fd);

/* Stat открытого fd: размер. */
int  fd_stat(int fd, uint32_t *out_size);

/* Stat по пути без открытия. */
int  fd_stat_path(const char *path, uint32_t *out_size);

#endif /* FD_H */
