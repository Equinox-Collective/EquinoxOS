#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <stdint.h>

/* ioctl request codes (Linux-совместимые, поддерживаемые ядром). */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int ioctl(int fd, unsigned long request, ...);

#endif /* _SYS_IOCTL_H */
