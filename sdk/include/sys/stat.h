#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>

// Заглушка структуры stat для проверки атрибутов файлов
struct stat {
    mode_t    st_mode;
    off_t     st_size;
};

// Макросы для проверки типов файлов
#define S_ISDIR(m)  (((m) & 0170000) == 0040000)
#define S_ISCHR(m)  (((m) & 0170000) == 0020000)
#define S_ISBLK(m)  (((m) & 0170000) == 0060000)
#define S_ISREG(m)  (((m) & 0170000) == 0100000)

int stat(const char *pathname, struct stat *statbuf);
int fstat(int fd, struct stat *statbuf);

#endif