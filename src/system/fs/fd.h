#ifndef FD_H
#define FD_H

/*
 * EquinoxOS — Per-process File Descriptor Table (Этап 2)
 *
 * Архитектура (как в Unix):
 *   - fd (int)          -> слот в таблице ТЕКУЩЕГО процесса (task->fdt)
 *   - слот указывает на "открытое описание файла" (ofd_t), которое МОЖЕТ
 *     разделяться несколькими fd (dup/dup2) и несколькими процессами (fork).
 *     У ofd_t есть refcount: освобождается, когда закрыт последний fd.
 *
 *   Виды ofd_t:
 *     OFD_FILE     — обычный файл (буфер в памяти + позиция; flush на close)
 *     OFD_PIPE_R   — read-конец пайпа (ipc.c)
 *     OFD_PIPE_W   — write-конец пайпа
 *     OFD_CONSOLE  — stdin/stdout/stderr (терминал/клавиатура)
 *
 *   fork():   таблица копируется, ofd'шки РАЗДЕЛЯЮТСЯ (refcount++).
 *   execve(): таблица СОХРАНЯЕТСЯ (нет O_CLOEXEC) — так шелл наследует пайпы.
 *   exit():   все fd закрываются (refcount--).
 *
 *   Перенаправление (`>` / `|`): dup2(pipe_or_file_fd, 1) подменяет stdout —
 *   запись в fd 1 через write()/printf уходит в пайп/файл, а не на экран.
 */

#include <stdint.h>
#include <stdbool.h>
#include "../fs/vfs.h"

#define FD_MAX       64
#define FD_STDIN     0
#define FD_STDOUT    1
#define FD_STDERR    2

/* Open flags — mirror SDK fcntl.h */
#define FD_O_RDONLY  0x0000
#define FD_O_WRONLY  0x0001
#define FD_O_RDWR    0x0002
#define FD_O_CREAT   0x0040
#define FD_O_TRUNC   0x0200
#define FD_O_APPEND  0x0400

typedef enum {
    OFD_FILE = 1,
    OFD_PIPE_R,
    OFD_PIPE_W,
    OFD_CONSOLE
} ofd_kind_t;

/* Открытое описание файла — разделяемый объект с refcount. */
typedef struct ofd {
    ofd_kind_t kind;
    int        refcount;

    /* OFD_FILE */
    uint8_t   *buf;        /* содержимое (read: загружено целиком; write: растёт) */
    uint32_t   size;       /* текущий размер данных */
    uint32_t   pos;        /* позиция чтения/записи (разделяется dup/fork) */
    uint32_t   buf_cap;    /* ёмкость буфера (write) */
    bool       is_write;
    bool       is_append;
    char       path[128];

    /* OFD_PIPE_R / OFD_PIPE_W */
    int        pipe_id;

    /* OFD_CONSOLE */
    int        console_no; /* 0/1/2 */
} ofd_t;

/* Таблица дескрипторов одного процесса. */
typedef struct fd_table {
    ofd_t *slots[FD_MAX];
} fd_table_t;

/* ---- Жизненный цикл таблицы (вызывается из task.c) ---------------------- */
fd_table_t *fd_table_create(void);            /* новая таблица: 0/1/2 = консоль */
fd_table_t *fd_table_clone(fd_table_t *src);  /* fork: разделить ofd'шки (ref++) */
void        fd_table_destroy(fd_table_t *t);  /* exit: закрыть всё, освободить   */

/* ---- POSIX-операции (работают над таблицей ТЕКУЩЕГО процесса) ----------- */
int  fd_open(const char *path, int flags);
int  fd_read(int fd, void *buf, uint32_t size);
int  fd_write(int fd, const void *buf, uint32_t size);
int  fd_seek(int fd, int32_t offset, int whence);
int  fd_tell(int fd);
int  fd_close(int fd);
int  fd_stat(int fd, uint32_t *out_size);
int  fd_stat_path(const char *path, uint32_t *out_size);
/* Этап 6c: расширенный fstat для Linux struct stat — отдаёт вид ofd (ofd_kind_t)
 * и размер. out_kind получает значение ofd_kind_t (OFD_FILE, OFD_PIPE_R/W,
 * OFD_CONSOLE), чтобы шлюз выставил st_mode (S_IFREG, S_IFIFO, S_IFCHR).
 * Возвращает 0 / -1. */
int  fd_statx(int fd, int *out_kind, uint32_t *out_size);

/* Этап 2: дублирование и пайпы. */
int  fd_dup(int oldfd);                 /* -> новый fd / -1 */
int  fd_dup2(int oldfd, int newfd);     /* -> newfd / -1 */
int  fd_make_pipe(int out_fds[2]);      /* 0 ok (out[0]=read, out[1]=write) / -1 */

#endif /* FD_H */
