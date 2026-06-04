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
    if (!stream || !stream->buffer || stream->filename[0] == '\0') return 0;
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
  if (!stream || !ptr) return 0;

  size_t total = size * nmemb;

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
    int count = 0;
    // Очень базовая реализация sscanf
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