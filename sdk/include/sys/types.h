#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Базовые системные типы для POSIX
typedef int pid_t;
typedef int mode_t;
typedef long off_t;
typedef long ssize_t;

#ifdef __cplusplus
}
#endif

#endif