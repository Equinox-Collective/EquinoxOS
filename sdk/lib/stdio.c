#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <equos.h>

int vsprintf(char* buffer, const char* format, va_list args) {
    char* ptr = buffer;
    const char* f = format;
    char temp_buf[64];

    while (*f) {
        if (*f != '%') {
            *ptr++ = *f++;
            continue;
        }

        f++; // Пропускаем '%'

        switch (*f) {
            case 'c': {
                char c = (char)va_arg(args, int);
                *ptr++ = c;
                break;
            }
            case 's': {
                char* s = va_arg(args, char*);
                if (!s) s = "(null)";
                while (*s) *ptr++ = *s++;
                break;
            }
            case 'd': {
                int val = va_arg(args, int);
                itoa(val, 10, temp_buf);
                char* t = temp_buf;
                while (*t) *ptr++ = *t++;
                break;
            }
            case 'x': {
                unsigned long long val = va_arg(args, unsigned long long);
                itoa_hex(val, temp_buf);
                char* t = temp_buf;
                while (*t) *ptr++ = *t++;
                break;
            }
            case '%': {
                *ptr++ = '%';
                break;
            }
        }
        f++;
    }
    *ptr = '\0';
    return ptr - buffer;
}

// ВОТ ЭТОГО НЕ ХВАТАЛО:
int sprintf(char* buffer, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int len = vsprintf(buffer, format, args);
    va_end(args);
    return len;
}

int printf(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int len = vsprintf(buffer, format, args);
    va_end(args);

    _syscall(SYS_PRINT, (uint64_t)buffer, 0, 0, 0, 0);
    return len;
}