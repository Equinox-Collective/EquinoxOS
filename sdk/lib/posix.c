// sdk/lib/posix.c
#include "../include/equos.h"
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <limits.h>

int errno = 0;

int access(const char *pathname, int mode) {
    return 0; 
}

// #define DEBUG_POSIX  // вкл. для отладки: печатает строку на каждый fopen.
// Выключено — печать на КАЖДЫЙ fopen шла в COM1 и заметно тормозила старт
// (sysgui открывает window/terminal/monitor/paint/explorer/notepad подряд).

FILE* fopen(const char* filename, const char* mode) {
#ifdef DEBUG_POSIX
    printf("fopen: %s (mode: %s)\n", filename, mode);
#endif
    
    FILE* f = (FILE*)malloc(sizeof(FILE));
    if (!f) return NULL;
    memset(f, 0, sizeof(FILE));
    strncpy(f->filename, filename, 127);

    if (mode[0] == 'r') {
        uint32_t size = 0;
        uint8_t* data = (uint8_t*)_syscall(2, (uint64_t)filename, (uint64_t)&size, 0, 0, 0);
        
        if (!data) {
            free(f);
            return NULL;
        }
        f->buffer = data;
        f->size = size;
        f->pos = 0;
    } else {
        // Режимы 'w', 'a'
        f->buffer = (uint8_t*)malloc(4096); // Начальный буфер 4КБ
        f->size = 4096;
        f->pos = 0;
        
        if (mode[0] == 'a') {
            uint32_t size = 0;
            uint8_t* data = (uint8_t*)_syscall(2, (uint64_t)filename, (uint64_t)&size, 0, 0, 0);
            if (data) {
                f->buffer = realloc(f->buffer, size + 4096);
                memcpy(f->buffer, data, size);
                f->size = size + 4096;
                f->pos = size;
            }
        }
    }
    return f;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream || !ptr || !stream->buffer) return 0;
    
    size_t total_to_read = size * nmemb;
    if (stream->pos >= stream->size) return 0;

    if (stream->pos + total_to_read > stream->size) {
        total_to_read = stream->size - stream->pos;
    }

    memcpy(ptr, stream->buffer + stream->pos, total_to_read);
    stream->pos += total_to_read;
    return total_to_read / size;
}

int fseek(FILE* stream, long offset, int whence) {
    if (!stream) return -1;
    if (whence == SEEK_SET) stream->pos = offset;      
    else if (whence == SEEK_CUR) stream->pos += offset;     
    else if (whence == SEEK_END) stream->pos = stream->size + offset; 
    
    if (stream->pos < 0) stream->pos = 0;
    if (stream->pos > stream->size) stream->pos = stream->size;
    return 0;
}

long ftell(FILE* stream) { 
    if (!stream) return -1;
    return (long)stream->pos; 
}

int fflush(FILE* stream) {
    /* stdout/stderr/NULL/fd-сентинелы выводятся сразу (буфера нет) — не
     * разыменовываем (иначе fflush((FILE*)1) -> page fault на stream->buffer). */
    if (!stream || stream == stdout || stream == stderr ||
        (uintptr_t)stream < 0x1000) return 0;
    if (!stream->buffer || stream->filename[0] == '\0') return 0;
    // Синхронизируем буфер с диском через SYS_WRITE_FILE (3)
    _syscall(3, (uint64_t)stream->filename, (uint64_t)stream->buffer, stream->pos, 0, 0);
    return 0;
}

int fclose(FILE* stream) {
    if (!stream) return EOF;
    fflush(stream);
    // Для 'r' буфер маппится ядром, мы его не трогаем (пока нет munmap)
    // Для 'w' мы его выделяли сами, но оставим для простоты.
    free(stream);
    return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
  if (!ptr) return 0;

  size_t total = size * nmemb;
  if (total == 0) return nmemb;

  /* stdout/stderr, NULL и fd-сентинелы (например (FILE*)1) НЕ разыменовываем как
   * FILE* — выводим в консоль через syscall 1 (SYS_PRINT), как уже делает
   * vfprintf. Защищает от page fault, если в качестве потока пришёл номер
   * дескриптора (fd 0/1/2) или испорченный указатель. stdout/stderr
   * сравниваем по символу, поэтому печать корректна даже если указатель
   * глобала перезаписан. */
  if (stream == stdout || stream == stderr || !stream ||
      (uintptr_t)stream < 0x1000) {
    /* Этап 2: пишем через fd (1=stdout, 2=stderr), а НЕ через SYS_PRINT, —
     * чтобы вывод уважал перенаправление (dup2 в пайп/файл). Консольный
     * конец fd 1/2 ядро по-прежнему печатает на экран. */
    int fd = (stream == stderr) ? 2 : 1;
    size_t off = 0;
    while (off < total) {
      size_t chunk = total - off;
      if (chunk > 0x40000) chunk = 0x40000; /* батчим крупные записи */
      int wr = sys_write_fd(fd, (const char *)ptr + off, (uint32_t)chunk);
      if (wr <= 0) break;
      off += (size_t)wr;
    }
    return nmemb;
  }

  if (stream->pos + total > stream->size) {
    size_t new_size = stream->pos + total + 4096;
    uint8_t *new_buf = realloc(stream->buffer, new_size);
    if (!new_buf) return 0;
    stream->buffer = new_buf;
    stream->size = new_size;
  }

  memcpy(stream->buffer + stream->pos, ptr, total);
  stream->pos += total;
  return nmemb;
}

int fputs(const char *s, FILE *stream) {
    size_t len = strlen(s);
    return (fwrite(s, 1, len, stream) == len) ? 0 : EOF;
}

int getc(FILE *stream) {
  unsigned char c;
  if (fread(&c, 1, 1, stream) == 1) return (int)c;
  return EOF;
}

int ungetc(int c, FILE *stream) {
  if (c == EOF || !stream || stream->pos == 0) return EOF;
  stream->pos--;
  stream->buffer[stream->pos] = (uint8_t)c;
  return c;
}

char *fgets(char *s, int size, FILE *stream) {
  int i = 0;
  while (i < size - 1) {
    int c = getc(stream);
    if (c == EOF) break;
    s[i++] = (char)c;
    if (c == '\n') break;
  }
  if (i == 0) return NULL;
  s[i] = '\0';
  return s;
}

int feof(FILE *stream) {
  return stream ? (stream->pos >= stream->size) : 1;
}

int ferror(FILE *stream) { return 0; }

void clearerr(FILE *stream) {}

FILE *freopen(const char *filename, const char *mode, FILE *stream) {
  fclose(stream);
  return fopen(filename, mode);
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) { return 0; }

char *tmpnam(char *s) {
  static char static_buf[L_tmpnam];
  sprintf(s ? s : static_buf, "/tmp/eq_%d.tmp", (int)time(NULL));
  return s ? s : static_buf;
}

void exit(int status) {
    _syscall(10, (uint64_t)status, 0, 0, 0, 0);
    while(1); 
}

int abs(int n) { return (n < 0) ? -n : n; }

int atoi(const char* s) {
    int res = 0;
    int sign = 1;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') res = res * 10 + (*s++ - '0');
    return res * sign;
}

double atof(const char *s) {
  double res = 0.0;
  double factor = 1.0;
  int decimal_found = 0;
  while (*s == ' ') s++;
  while (*s) {
    if (*s >= '0' && *s <= '9') {
      if (decimal_found) {
        factor /= 10.0;
        res = res + (*s - '0') * factor;
      } else {
        res = res * 10.0 + (*s - '0');
      }
    } else if (*s == '.') {
      decimal_found = 1;
    } else break;
    s++;
  }
  return res;
}

char* strdup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* n = malloc(len);
    if (n) memcpy(n, s, len);
    return n;
}

int sscanf(const char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int count = vsscanf(str, format, args);
    va_end(args);
    return count;
}

time_t time(time_t *t) {
  time_t res = (time_t)_syscall(6, 0, 0, 0, 0, 0);
  if (t) *t = res;
  return res;
}

char *strerror(int errnum) { return "Unknown error"; }
char *getenv(const char *name) { return NULL; }

double strtod(const char *nptr, char **endptr) {
  double res = atof(nptr);
  if (endptr) {
    while (*nptr && (isspace(*nptr) || isdigit(*nptr) || *nptr == '.')) nptr++;
    *endptr = (char *)nptr;
  }
  return res;
}

void abort(void) { exit(1); }
void (*signal(int sig, void (*func)(int)))(int) { return SIG_ERR; }
int raise(int sig) { return -1; }
clock_t clock(void) { return (clock_t)time(NULL); }
struct tm *localtime(const time_t *t) { static struct tm tmp; return &tmp; }
struct tm *gmtime(const time_t *t) { static struct tm tmp; return &tmp; }
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) { return 0; }
time_t mktime(struct tm *tm) { return -1; }
FILE *tmpfile(void) { return NULL; }
void rewind(FILE *stream) { if (stream) stream->pos = 0; }

// --- MISSING FUNCTIONS RESTORED ---
static struct lconv static_lconv = {".", "", ""};
struct lconv *localeconv(void) { return &static_lconv; }
char *setlocale(int category, const char *locale) { return "C"; }

int remove(const char* path) { return 0; }
int rename(const char* old_name, const char* new_name) { return 0; }
int system(const char* command) {
    if (!command) return 1;
    return sys_exec(command) == 1 ? 0 : -1;
}
int mkdir(const char* path, mode_t mode) { return 0; }
void DG_SetWindowTitle(const char* title) { }

static unsigned long next = 1;
int rand(void) {
    next = next * 1103515245 + 12345;
    return (unsigned int)(next / 65536) % 32768;
}
void srand(unsigned int seed) {
    next = seed;
}

void _exit(int status) {
    exit(status);
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long acc;
    int c;
    unsigned long any;
    int neg = 0;

    /* Пропускаем пробелы и считываем знак */
    do {
        c = *s++;
    } while (c == ' ' || (c >= '\t' && c <= '\r'));
    if (c == '-') {
        neg = 1;
        c = *s++;
    } else if (c == '+') {
        c = *s++;
    }

    /* Автоопределение системы счисления (0x для Hex, 0 для Octal) */
    if ((base == 0 || base == 16) &&
        c == '0' && (*s == 'x' || *s == 'X')) {
        c = s[1];
        s += 2;
        base = 16;
    }
    if (base == 0) {
        base = (c == '0') ? 8 : 10;
    }

    acc = 0;
    any = 0;
    for (;; c = *s++) {
        if (c >= '0' && c <= '9') {
            c -= '0';
        } else if (c >= 'A' && c <= 'Z') {
            c -= 'A' - 10;
        } else if (c >= 'a' && c <= 'z') {
            c -= 'a' - 10;
        } else {
            break;
        }
        if (c >= base) {
            break;
        }
        if (any < 0) {
            continue;
        }
        if (neg) {
            if (acc > (unsigned long)LONG_MAX + 1) {
                any = -1;
            } else {
                any = 1;
                acc = acc * base + c;
            }
        } else {
            if (acc > LONG_MAX) {
                any = -1;
            } else {
                any = 1;
                acc = acc * base + c;
            }
        }
    }
    if (any < 0) {
        acc = neg ? LONG_MIN : LONG_MAX;
        errno = ERANGE;
    } else if (neg) {
        acc = -acc;
    }
    if (endptr != 0) {
        *endptr = (char *)(any ? s - 1 : nptr);
    }
    return (long)acc;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long acc;
    int c;
    unsigned long any;
    int neg = 0;

    do {
        c = *s++;
    } while (c == ' ' || (c >= '\t' && c <= '\r'));
    if (c == '-') {
        neg = 1;
        c = *s++;
    } else if (c == '+') {
        c = *s++;
    }

    if ((base == 0 || base == 16) &&
        c == '0' && (*s == 'x' || *s == 'X')) {
        c = s[1];
        s += 2;
        base = 16;
    }
    if (base == 0) {
        base = (c == '0') ? 8 : 10;
    }

    acc = 0;
    any = 0;
    for (;; c = *s++) {
        if (c >= '0' && c <= '9') {
            c -= '0';
        } else if (c >= 'A' && c <= 'Z') {
            c -= 'A' - 10;
        } else if (c >= 'a' && c <= 'z') {
            c -= 'a' - 10;
        } else {
            break;
        }
        if (c >= base) {
            break;
        }
        if (any < 0) {
            continue;
        }
        if (acc > ULONG_MAX / base || (acc == ULONG_MAX / base && (unsigned long)c > ULONG_MAX % base)) {
            any = -1;
        } else {
            any = 1;
            acc = acc * base + c;
        }
    }
    if (any < 0) {
        acc = ULONG_MAX;
        errno = ERANGE;
    } else if (neg) {
        acc = -acc;
    }
    if (endptr != 0) {
        *endptr = (char *)(any ? s - 1 : nptr);
    }
    return acc;
}

/* ============================================================
 * POSIX fd API — backed by EquinoxOS syscalls 90-96
 * ============================================================ */
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* open() — translates POSIX flags to kernel flags (they are identical
 * since we defined them to match, but we go through the wrapper for
 * clarity and future compat). Variadic mode arg is accepted but ignored
 * (no permission bits on our FS yet). */
int open(const char *path, int flags, ...) {
    return sys_open(path, flags);
}

int close(int fd) {
    return sys_close_fd(fd);
}

ssize_t read(int fd, void *buf, size_t count) {
    if (count > 0x7fffffff) count = 0x7fffffff;
    return (ssize_t)sys_read_fd(fd, buf, (uint32_t)count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (count > 0x7fffffff) count = 0x7fffffff;
    return (ssize_t)sys_write_fd(fd, buf, (uint32_t)count);
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)sys_seek(fd, (int32_t)offset, whence);
}

int isatty(int fd) {
    /* fds 0/1/2 are "terminal", rest are regular files */
    return (fd >= 0 && fd <= 2) ? 1 : 0;
}

int unlink(const char *pathname) {
    /* Stub — no delete support yet in VFS */
    (void)pathname;
    errno = 1; /* EPERM */
    return -1;
}

int dup(int oldfd) {
    return sys_dup(oldfd);
}

int dup2(int oldfd, int newfd) {
    return sys_dup2(oldfd, newfd);
}

int pipe(int fds[2]) {
    return sys_pipe(fds);
}

char *getcwd(char *buf, size_t size) {
    if (!buf || size < 2) return NULL;
    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}

int chdir(const char *path) {
    (void)path;
    return 0; /* single-root VFS, always "/" */
}

pid_t getpid(void) {
    /* Этап 1: настоящий pid из ядра (раньше возвращался фейк = 2). */
    return (pid_t)sys_getpid();
}

/* --- Этап 1: процессная модель (POSIX-обёртки) --------------------------- *
 * fork():  0 в ребёнке, pid ребёнка в родителе, -1 при ошибке.
 * waitpid(): ждёт ребёнка, кодирует код выхода в *status как (code << 8),
 *            чтобы работали стандартные WIFEXITED/WEXITSTATUS. */
pid_t fork(void) {
    return (pid_t)sys_fork();
}

pid_t waitpid(pid_t pid, int *status, int options) {
    (void)options; /* WNOHANG пока не поддержан — всегда блокирующий */
    int code = 0;
    int64_t r = sys_waitpid((int64_t)pid, &code);
    if (r >= 0 && status) {
        /* Упаковываем как нормальный wait-status: младший байт = 0 (нет
         * сигнала), следующий байт = код выхода. */
        *status = (code & 0xFF) << 8;
    }
    return (pid_t)r;
}

pid_t wait(int *status) {
    return waitpid(0, status, 0);
}

pid_t getppid(void) {
    return 1;
}

/* --- Этап 1b: execve и обёртки -------------------------------------------- *
 * execve() заменяет образ текущего процесса. При успехе НЕ возвращается;
 * при ошибке возвращает -1 (errno не выставляем — отдельной таблицы пока нет).
 * execv()  — то же с текущим окружением (environ).
 * execvp() — пробует имя как есть, затем с префиксом "bin/". */
int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)sys_execve(path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    char *const envp[] = { 0 };
    return (int)sys_execve(path, argv, envp);
}

int execvp(const char *file, char *const argv[]) {
    char *const envp[] = { 0 };
    /* 1) как передано */
    sys_execve(file, argv, envp);
    /* 2) не нашлось -> пробуем bin/<file> */
    char buf[256];
    int i = 0;
    const char *pfx = "bin/";
    for (; pfx[i]; i++) buf[i] = pfx[i];
    int j = 0;
    for (; file[j] && (i + j) < 255; j++) buf[i + j] = file[j];
    buf[i + j] = 0;
    return (int)sys_execve(buf, argv, envp);
}

/* stat() — fills st_size from SYS_STAT_PATH, st_mode as regular file */
int stat(const char *pathname, struct stat *statbuf) {
    if (!statbuf) { errno = 14; return -1; } /* EFAULT */
    uint32_t sz = 0;
    int rc = sys_stat_path(pathname, &sz);
    if (rc < 0) { errno = 2; return -1; } /* ENOENT */
    statbuf->st_size = (off_t)sz;
    statbuf->st_mode = 0100644; /* S_IFREG | rw-r--r-- */
    return 0;
}

int fstat(int fd, struct stat *statbuf) {
    if (!statbuf) { errno = 14; return -1; }
    uint32_t sz = 0;
    int rc = sys_fstat(fd, &sz);
    if (rc < 0) { errno = 9; return -1; } /* EBADF */
    statbuf->st_size = (off_t)sz;
    statbuf->st_mode = (fd <= 2) ? 0020666 : 0100644;
    return 0;
}
