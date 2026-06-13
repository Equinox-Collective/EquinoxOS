#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

/*
 * EquinoxOS — TTY / line discipline (Этап 5).
 *
 * Консоль системы — последовательный порт COM1: term_print() уже пишет туда
 * (stdout/stderr), а stdin теперь читается оттуда же (как serial console в
 * Linux). PS/2-клавиатура + framebuffer остаются за sysgui и здесь не трогаются.
 *
 * Реализована дисциплина линии:
 *   - Канонический режим (ICANON): построчный ввод с эхо, забоем (VERASE),
 *     завершение по Enter; Ctrl-D (VEOF) на пустой строке -> EOF (0).
 *   - Сырой режим (!ICANON): чтение по VMIN байт (без редактирования).
 *   - ISIG: Ctrl-C -> SIGINT, Ctrl-\ -> SIGQUIT текущему читающему процессу.
 *   - ECHO: эхо введённых символов в консоль.
 *
 * termios — глобальный (единственная консоль). Доступ через ioctl
 * TCGETS/TCSETS* (см. syscall.c, SYS_IOCTL) и SDK <termios.h>.
 */

#include <stdint.h>

#define KNCCS 19
typedef unsigned int  ktcflag_t;
typedef unsigned char kcc_t;

typedef struct {
    ktcflag_t c_iflag;
    ktcflag_t c_oflag;
    ktcflag_t c_cflag;
    ktcflag_t c_lflag;
    kcc_t     c_line;
    kcc_t     c_cc[KNCCS];
} ktermios_t;

/* c_lflag */
#define K_ISIG   0x0001
#define K_ICANON 0x0002
#define K_ECHO   0x0008
#define K_ECHOE  0x0010
#define K_ECHOK  0x0020
#define K_ECHONL 0x0040

/* c_iflag */
#define K_INLCR  0x0040
#define K_IGNCR  0x0080
#define K_ICRNL  0x0100
#define K_IXON   0x0400

/* c_oflag */
#define K_OPOST  0x0001
#define K_ONLCR  0x0004

/* c_cc indices (как в Linux) */
#define K_VINTR  0
#define K_VQUIT  1
#define K_VERASE 2
#define K_VKILL  3
#define K_VEOF   4
#define K_VTIME  5
#define K_VMIN   6
#define K_VSUSP  10

/* ioctl request codes (Linux-совместимые) */
#define K_TCGETS     0x5401
#define K_TCSETS     0x5402
#define K_TCSETSW    0x5403
#define K_TCSETSF    0x5404
#define K_TIOCGWINSZ 0x5413
#define K_TIOCSWINSZ 0x5414
#define K_TIOCGPGRP  0x540F  /* tcgetpgrp: получить foreground-группу терминала */
#define K_TIOCSPGRP  0x5410  /* tcsetpgrp: задать foreground-группу терминала */

struct kwinsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

void tty_init(void);

/* Блокирующее чтение из консоли (stdin). Возвращает кол-во прочитанных байт,
 * 0 при EOF (Ctrl-D на пустой строке), -1 при прерывании сигналом (EINTR). */
int  tty_read(void *buf, uint32_t size);

/* Доступ к termios для ioctl. Возвращают 0/-1. */
int  tty_get_termios(ktermios_t *out);
int  tty_set_termios(const ktermios_t *in);
void tty_get_winsize(struct kwinsize *ws);

#endif /* KERNEL_TTY_H */
