#ifndef SDL_config_equinox_h_
#define SDL_config_equinox_h_

#define SDL_config_h_

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

/* У тебя 64-битная система */
#define SIZEOF_VOIDP 8

/* Говорим, какие стандартные функции у нас есть в posix.c/stdio.c */
#define HAVE_LIBC 1
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_ABS 1
#define HAVE_MEMCPY 1
#define HAVE_MEMSET 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#define HAVE_STRDUP 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_ATOI 1
#define HAVE_ATOF 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_VSSCANF 1
#define HAVE_VSNPRINTF 1
#define HAVE_ACOS 1
#define HAVE_ASIN 1
#define HAVE_ATAN 1
#define HAVE_ATAN2 1
#define HAVE_CEIL 1
#define HAVE_COS 1
#define HAVE_FABS 1
#define HAVE_FLOOR 1
#define HAVE_LOG 1
#define HAVE_LOG10 1
#define HAVE_POW 1
#define HAVE_SIN 1
#define HAVE_SQRT 1
#define HAVE_TAN 1

/* Отключаем все тяжелые платформенные подсистемы */
#define SDL_AUDIO_DISABLED 1
#define SDL_JOYSTICK_DISABLED 1
#define SDL_HAPTIC_DISABLED 1
#define SDL_SENSOR_DISABLED 1
#define SDL_LOADSO_DISABLED 1
#define SDL_THREADS_DISABLED 1

/* Включаем наш собственный кастомный видеодрайвер Equinox */
#define SDL_VIDEO_DRIVER_EQUINOX 1
#define SDL_TIMER_DUMMY 1

/* Оставляем программный рендерер пикселей */
#define SDL_VIDEO_RENDER_SW 1

#endif /* SDL_config_equinox_h_ */