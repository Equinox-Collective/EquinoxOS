#ifndef _FCNTL_H
#define _FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

// Стандартные флаги POSIX (цифры взяты из стандарта, чтобы не было конфликтов)
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

// На случай, если он решит напрямую вызвать open() вместо fopen()
int open(const char *pathname, int flags, ...);

#ifdef __cplusplus
}
#endif

#endif