/* sdk/lib/termios.c — Этап 5: пользовательские обёртки termios/ioctl.
 *
 * tcgetattr/tcsetattr — это ioctl(TCGETS/TCSETS). cfmakeraw переводит termios
 * в «сырой» режим (без ICANON/ECHO/ISIG, VMIN=1/VTIME=0). Скорость порта
 * фиксирована (ядро не применяет baud), поэтому cf*speed — заглушки. */

#include <termios.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <errno.h>
#include <equos.h>

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *argp = va_arg(ap, void *);
    va_end(ap);
    int rc = sys_ioctl(fd, (uint64_t)request, argp);
    if (rc < 0) { errno = 25 /* ENOTTY */; return -1; }
    return rc;
}

int tcgetattr(int fd, struct termios *t) {
    if (!t) { errno = 22; return -1; }
    return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int optional_actions, const struct termios *t) {
    if (!t) { errno = 22; return -1; }
    unsigned long req = TCSETS;
    if (optional_actions == TCSADRAIN) req = TCSETSW;
    else if (optional_actions == TCSAFLUSH) req = TCSETSF;
    return ioctl(fd, req, (void *)t);
}

void cfmakeraw(struct termios *t) {
    if (!t) return;
    t->c_iflag &= ~(IGNBRK | BRKINT | IGNPAR | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHONL | IEXTEN);
    t->c_cflag &= ~CS8;
    t->c_cflag |=  CS8;
    t->c_cc[VMIN]  = 1;
    t->c_cc[VTIME] = 0;
}

int tcflush(int fd, int queue_selector) { (void)fd; (void)queue_selector; return 0; }
int tcdrain(int fd) { (void)fd; return 0; }

speed_t cfgetispeed(const struct termios *t) { (void)t; return 38400; }
speed_t cfgetospeed(const struct termios *t) { (void)t; return 38400; }
int cfsetispeed(struct termios *t, speed_t speed) { (void)t; (void)speed; return 0; }
int cfsetospeed(struct termios *t, speed_t speed) { (void)t; (void)speed; return 0; }
