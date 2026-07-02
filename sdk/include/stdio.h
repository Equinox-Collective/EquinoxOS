#ifndef _STDIO_H
#define _STDIO_H
#ifdef __cplusplus
extern "C" {
#endif


#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef struct {
    int fd;
    uint8_t* buffer;
    uint32_t size;
    uint32_t pos;
    char filename[128];
} FILE;

#define SEEK_SET 0
#define SEEK_CUR 1
#define L_tmpnam 32
#define SEEK_END 2
#define BUFSIZ 1024
#define EOF (-1)
#define _IOFBF 0 // Full buffering (полная буферизация)
#define _IOLBF 1 // Line buffering (построчная)
#define _IONBF 2 // No buffering (без буферизации)

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

int printf(const char* format, ...);
int sprintf(char* str, const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);
int vsprintf(char* buffer, const char* format, va_list args);
int vsnprintf(char* str, size_t size, const char* format, va_list ap);
int sscanf(const char *str, const char *format, ...);
int fprintf(FILE* stream, const char* format, ...);
int vfprintf(FILE* stream, const char* format, va_list ap);
int puts(const char* s);
int putchar(int c);

FILE* fopen(const char* filename, const char* mode);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
int fclose(FILE* stream);
int fflush(FILE* stream);
int remove(const char* path);
int rename(const char* old_name, const char* new_name);

int feof(FILE *stream);
int ferror(FILE *stream);
int getc(FILE *stream);
int ungetc(int c, FILE *stream);
int fputs(const char *s, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
FILE *freopen(const char *filename, const char *mode, FILE *stream);
int setvbuf(FILE *stream, char *buf, int mode, size_t size);
int fflush(FILE *stream);
void clearerr(FILE *stream);
void rewind(FILE *stream);

char *tmpnam(char *s);
FILE *tmpfile(void);

#ifdef __cplusplus
}
#endif
#endif