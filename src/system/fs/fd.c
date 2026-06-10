/*
 * EquinoxOS — Per-process File Descriptor Table (fd.c, Этап 2)
 *
 * Слоты fd процесса хранятся в task->fdt (см. fd.h). Каждый слот указывает на
 * разделяемое "открытое описание файла" ofd_t с refcount. fork() разделяет
 * ofd'шки, execve() сохраняет таблицу, exit() закрывает всё.
 */

#include "fd.h"
#include "vfs.h"
#include "ext2.h"
#include "../mem/memory.h"
#include "../usr/ipc.h"
#include "../usr/task.h"
#include "../../syslibc/string.h"
#include "../../syslibc/stdio.h"
#include "../usr/tty.h"

extern void term_print(const char *str);

/* ------------------------------------------------------------------ *
 *  Консольные ofd'шки — единственные на всю систему, общие для всех
 *  процессов. Никогда не освобождаются (kind == OFD_CONSOLE).
 * ------------------------------------------------------------------ */
static ofd_t console_ofd[3] = {
    { .kind = OFD_CONSOLE, .refcount = 1, .console_no = 0 },
    { .kind = OFD_CONSOLE, .refcount = 1, .console_no = 1 },
    { .kind = OFD_CONSOLE, .refcount = 1, .console_no = 2 },
};

/* ------------------------------------------------------------------ *
 *  refcount открытого описания
 * ------------------------------------------------------------------ */
static void ofd_ref(ofd_t *o) {
    if (o) o->refcount++;
}

/* Сброс write-буфера файла на диск (как было в старом fd_close). */
static void ofd_flush_file(ofd_t *o) {
    if (!(o->is_write && o->buf && o->size > 0)) return;
    vfs_node_t *dev = vfs_root->next;
    while (dev) {
        if (dev->write) {
            vfs_node_t file_node;
            memset(&file_node, 0, sizeof(vfs_node_t));
            size_t nl = strlen(o->path);
            if (nl >= sizeof(file_node.name)) nl = sizeof(file_node.name) - 1;
            memcpy(file_node.name, o->path, nl);
            file_node.name[nl] = '\0';
            dev->write(&file_node, 0, o->size, o->buf);
            break;
        }
        dev = dev->next;
    }
}

static void ofd_unref(ofd_t *o) {
    if (!o) return;
    if (o->kind == OFD_CONSOLE) return; /* статические — не трогаем */
    if (--o->refcount > 0) return;

    if (o->kind == OFD_FILE) {
        ofd_flush_file(o);
        if (o->buf) kfree(o->buf);
    } else if (o->kind == OFD_PIPE_R) {
        pipe_unref(o->pipe_id, false);
    } else if (o->kind == OFD_PIPE_W) {
        pipe_unref(o->pipe_id, true);
    }
    kfree(o);
}

/* ------------------------------------------------------------------ *
 *  Доступ к таблице текущего процесса
 * ------------------------------------------------------------------ */
static fd_table_t *cur(void) {
    return task_current_fdt();
}

static bool fd_in_range(int fd) {
    return fd >= 0 && fd < FD_MAX;
}

static ofd_t *resolve(int fd) {
    fd_table_t *t = cur();
    if (!t || !fd_in_range(fd)) return NULL;
    return t->slots[fd];
}

/* Установить ofd в наименьший свободный слот >= 0; вернуть fd / -1. */
static int install_lowest(ofd_t *o) {
    fd_table_t *t = cur();
    if (!t) return -1;
    for (int i = 0; i < FD_MAX; i++) {
        if (!t->slots[i]) { t->slots[i] = o; return i; }
    }
    return -1; /* EMFILE */
}

/* ------------------------------------------------------------------ *
 *  Жизненный цикл таблицы
 * ------------------------------------------------------------------ */
fd_table_t *fd_table_create(void) {
    fd_table_t *t = (fd_table_t *)kmalloc(sizeof(fd_table_t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->slots[0] = &console_ofd[0];
    t->slots[1] = &console_ofd[1];
    t->slots[2] = &console_ofd[2];
    return t;
}

fd_table_t *fd_table_clone(fd_table_t *src) {
    fd_table_t *t = (fd_table_t *)kmalloc(sizeof(fd_table_t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    if (src) {
        for (int i = 0; i < FD_MAX; i++) {
            t->slots[i] = src->slots[i];
            if (t->slots[i]) ofd_ref(t->slots[i]);
        }
    } else {
        t->slots[0] = &console_ofd[0];
        t->slots[1] = &console_ofd[1];
        t->slots[2] = &console_ofd[2];
    }
    return t;
}

void fd_table_destroy(fd_table_t *t) {
    if (!t) return;
    for (int i = 0; i < FD_MAX; i++) {
        if (t->slots[i]) { ofd_unref(t->slots[i]); t->slots[i] = NULL; }
    }
    kfree(t);
}

/* ------------------------------------------------------------------ *
 *  open / close
 * ------------------------------------------------------------------ */
int fd_open(const char *path, int flags) {
    if (!path || path[0] == '\0') return -1;

    ofd_t *o = (ofd_t *)kmalloc(sizeof(ofd_t));
    if (!o) return -1;
    memset(o, 0, sizeof(*o));
    o->kind     = OFD_FILE;
    o->refcount = 1;

    size_t plen = strlen(path);
    if (plen >= sizeof(o->path) - 1) plen = sizeof(o->path) - 1;
    memcpy(o->path, path, plen);
    o->path[plen] = '\0';

    int is_write  = (flags & FD_O_WRONLY) || (flags & FD_O_RDWR);
    o->is_write   = (bool)is_write;
    o->is_append  = (flags & FD_O_APPEND) != 0;

    if (!is_write) {
        uint32_t size = 0;
        uint8_t *data = vfs_read_file(path, &size);
        if (!data) { kfree(o); return -1; }
        o->buf = data;
        o->size = size;
        o->pos = 0;
        o->buf_cap = 0;
    }

    int fd = install_lowest(o);
    if (fd < 0) { ofd_unref(o); return -1; }
    return fd;
}

int fd_close(int fd) {
    fd_table_t *t = cur();
    if (!t || !fd_in_range(fd) || !t->slots[fd]) return -1;
    ofd_unref(t->slots[fd]);
    t->slots[fd] = NULL;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  read / write — маршрутизация по виду ofd
 * ------------------------------------------------------------------ */
int fd_read(int fd, void *buf, uint32_t size) {
    ofd_t *o = resolve(fd);
    if (!o) return -1;

    switch (o->kind) {
    case OFD_FILE:
        if (!o->buf) return -1;
        if (o->pos >= o->size) return 0; /* EOF */
        {
            uint32_t avail = o->size - o->pos;
            if (size > avail) size = avail;
            memcpy(buf, o->buf + o->pos, size);
            o->pos += size;
            return (int)size;
        }
    case OFD_PIPE_R:
        return (int)pipe_read(o->pipe_id, buf, size);
    case OFD_PIPE_W:
        return -1; /* нельзя читать write-конец */
    case OFD_CONSOLE:
        /* Этап 5: stdin читается из консоли (COM1) через дисциплину линии. */
        if (o->console_no == 0) return tty_read(buf, size);
        return -1;  /* чтение из stdout/stderr недопустимо */
    }
    return -1;
}

/* Запись len байт на консоль (term_print ждёт NUL-строку). */
static void console_write(const void *buf, uint32_t size) {
    char kb[513];
    uint32_t off = 0;
    const char *p = (const char *)buf;
    while (off < size) {
        uint32_t chunk = size - off;
        if (chunk > sizeof(kb) - 1) chunk = sizeof(kb) - 1;
        memcpy(kb, p + off, chunk);
        kb[chunk] = '\0';
        term_print(kb);
        off += chunk;
    }
}

int fd_write(int fd, const void *buf, uint32_t size) {
    ofd_t *o = resolve(fd);
    if (!o) return -1;
    if (!buf || size == 0) return 0;

    switch (o->kind) {
    case OFD_CONSOLE:
        if (o->console_no == 0) return -1; /* запись в stdin */
        console_write(buf, size);
        return (int)size;
    case OFD_PIPE_W:
        return (int)pipe_write(o->pipe_id, buf, size);
    case OFD_PIPE_R:
        return -1;
    case OFD_FILE: {
        if (!o->is_write) return -1;
        uint32_t write_pos = o->is_append ? o->size : o->pos;
        if (write_pos + size > o->buf_cap) {
            uint32_t new_cap = write_pos + size + 4096;
            uint8_t *nb = (uint8_t *)kmalloc(new_cap);
            if (!nb) return -1;
            if (o->buf) { memcpy(nb, o->buf, o->size); kfree(o->buf); }
            o->buf = nb;
            o->buf_cap = new_cap;
        }
        memcpy(o->buf + write_pos, buf, size);
        uint32_t end = write_pos + size;
        if (end > o->size) o->size = end;
        if (!o->is_append) o->pos = end;
        return (int)size;
    }
    }
    return -1;
}

int fd_seek(int fd, int32_t offset, int whence) {
    ofd_t *o = resolve(fd);
    if (!o || o->kind != OFD_FILE) return -1;

    int32_t new_pos;
    switch (whence) {
        case 0: new_pos = offset; break;
        case 1: new_pos = (int32_t)o->pos + offset; break;
        case 2: new_pos = (int32_t)o->size + offset; break;
        default: return -1;
    }
    if (new_pos < 0) new_pos = 0;
    if (new_pos > (int32_t)o->size) new_pos = (int32_t)o->size;
    o->pos = (uint32_t)new_pos;
    return (int)o->pos;
}

int fd_tell(int fd) {
    ofd_t *o = resolve(fd);
    if (!o || o->kind != OFD_FILE) return -1;
    return (int)o->pos;
}

int fd_stat(int fd, uint32_t *out_size) {
    ofd_t *o = resolve(fd);
    if (!o) return -1;
    if (out_size) *out_size = (o->kind == OFD_FILE) ? o->size : 0;
    return 0;
}

int fd_statx(int fd, int *out_kind, uint32_t *out_size) {
    ofd_t *o = resolve(fd);
    if (!o) return -1;
    if (out_kind) *out_kind = (int)o->kind;
    if (out_size) *out_size = (o->kind == OFD_FILE) ? o->size : 0;
    return 0;
}

int fd_stat_path(const char *path, uint32_t *out_size) {
    if (!path) return -1;
    vfs_node_t *dev = vfs_root->next;
    while (dev) {
        if (dev->finddir) {
            vfs_node_t *fn = dev->finddir(dev, (char *)path);
            if (fn) { if (out_size) *out_size = fn->size; return 0; }
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

/* ------------------------------------------------------------------ *
 *  dup / dup2 / pipe
 * ------------------------------------------------------------------ */
int fd_dup(int oldfd) {
    ofd_t *o = resolve(oldfd);
    if (!o) return -1;
    int fd = install_lowest(o);
    if (fd < 0) return -1;
    ofd_ref(o);
    return fd;
}

int fd_dup2(int oldfd, int newfd) {
    ofd_t *o = resolve(oldfd);
    if (!o) return -1;
    if (!fd_in_range(newfd)) return -1;
    if (oldfd == newfd) return newfd;

    fd_table_t *t = cur();
    if (!t) return -1;
    if (t->slots[newfd]) ofd_unref(t->slots[newfd]);
    t->slots[newfd] = o;
    ofd_ref(o);
    return newfd;
}

int fd_make_pipe(int out_fds[2]) {
    int id = pipe_create();
    if (id < 0) return -1;
    /* один read-конец + один write-конец. */
    pipe_set_ends(id, 1, 1);

    ofd_t *r = (ofd_t *)kmalloc(sizeof(ofd_t));
    ofd_t *w = (ofd_t *)kmalloc(sizeof(ofd_t));
    if (!r || !w) { if (r) kfree(r); if (w) kfree(w); pipe_close(id); return -1; }
    memset(r, 0, sizeof(*r)); memset(w, 0, sizeof(*w));
    r->kind = OFD_PIPE_R; r->refcount = 1; r->pipe_id = id;
    w->kind = OFD_PIPE_W; w->refcount = 1; w->pipe_id = id;

    int rfd = install_lowest(r);
    if (rfd < 0) { kfree(r); kfree(w); pipe_close(id); return -1; }
    int wfd = install_lowest(w);
    if (wfd < 0) { fd_close(rfd); kfree(w); return -1; }

    out_fds[0] = rfd;
    out_fds[1] = wfd;
    return 0;
}
