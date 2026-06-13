#ifndef _TERMIOS_H
#define _TERMIOS_H

/* POSIX termios (подмножество, Этап 5). Раскладка struct termios совпадает
 * с ядром (src/system/usr/tty.h, ktermios_t) — ioctl TCGETS/TCSETS копируют
 * её байт-в-байт. */

typedef unsigned int  tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int  speed_t;

#define NCCS 19

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
};

/* c_cc индексы */
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSWTC   7
#define VSTART  8
#define VSTOP   9
#define VSUSP   10
#define VEOL    11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

/* c_iflag */
#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define INLCR   0x0040
#define IGNCR   0x0080
#define ICRNL   0x0100
#define IXON    0x0400
#define IXOFF   0x1000

/* c_oflag */
#define OPOST   0x0001
#define ONLCR   0x0004

/* c_lflag */
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define NOFLSH  0x0080
#define IEXTEN  0x8000

/* c_cflag (информативно; ядро не применяет скорость) */
#define CS8     0x0030
#define CREAD   0x0080
#define CLOCAL  0x0800

/* tcsetattr optional_actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* tcflush queue selectors (заглушка) */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

int  tcgetattr(int fd, struct termios *t);
int  tcsetattr(int fd, int optional_actions, const struct termios *t);
void cfmakeraw(struct termios *t);
int  tcflush(int fd, int queue_selector);
int  tcdrain(int fd);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int  cfsetispeed(struct termios *t, speed_t speed);
int  cfsetospeed(struct termios *t, speed_t speed);

#endif /* _TERMIOS_H */
