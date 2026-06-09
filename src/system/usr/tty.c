/* src/system/usr/tty.c — Этап 5: дисциплина линии поверх COM1.
 *
 * Чтение полностью синхронное: tty_read опрашивает serial_received() в цикле
 * с yield(), редактирует строку (канон. режим) и эхоит символы. Сигналы
 * Ctrl-C/Ctrl-\ доставляются прямо здесь (signal_send текущему процессу) с
 * возвратом -1 (EINTR) — signal_deliver в конце syscall_handler выполнит
 * обработчик/действие по умолчанию. */

#include "tty.h"
#include "task.h"
#include "signal.h"
#include "../drivers/hardware/serial/serial.h"

static ktermios_t tty_termios;

void tty_init(void) {
    ktermios_t *t = &tty_termios;
    t->c_iflag = K_ICRNL;
    t->c_oflag = K_OPOST | K_ONLCR;   /* хранится; вывод не переписываем */
    t->c_cflag = 0;
    t->c_lflag = K_ISIG | K_ICANON | K_ECHO | K_ECHOE | K_ECHOK;
    t->c_line  = 0;
    for (int i = 0; i < KNCCS; i++) t->c_cc[i] = 0;
    t->c_cc[K_VINTR]  = 3;    /* ^C  */
    t->c_cc[K_VQUIT]  = 28;   /* ^\  */
    t->c_cc[K_VERASE] = 127;  /* DEL */
    t->c_cc[K_VKILL]  = 21;   /* ^U  */
    t->c_cc[K_VEOF]   = 4;    /* ^D  */
    t->c_cc[K_VSUSP]  = 26;   /* ^Z  */
    t->c_cc[K_VMIN]   = 1;
    t->c_cc[K_VTIME]  = 0;
}

/* --- эхо в консоль (прямо в serial, без term_print) --- */
static void echo_ch(char c) { serial_putchar(COM1, c); }
static void echo_str(const char *s) { while (*s) serial_putchar(COM1, *s++); }

/* Доставляемый (не SIGCHLD) сигнал ожидает -> прервать чтение (EINTR). */
static int interrupted(void) {
    if (!current_task) return 0;
    uint64_t deliverable = current_task->sig_pending & ~current_task->sig_blocked;
    deliverable &= ~(1ULL << KSIGCHLD);   /* SIGCHLD не прерывает read */
    return deliverable != 0;
}

/* Ждать один байт из COM1. Возвращает 0..255, или -1 если прервано сигналом. */
static int wait_byte(void) {
    for (;;) {
        if (interrupted()) return -1;
        if (serial_received(COM1)) return (int)(unsigned char)serial_getchar(COM1);
        yield();
    }
}

int tty_read(void *buf, uint32_t size) {
    if (!buf || size == 0) return 0;
    char *out = (char *)buf;
    ktermios_t *t = &tty_termios;

    /* ---------- сырой режим ---------- */
    if (!(t->c_lflag & K_ICANON)) {
        uint32_t vmin = t->c_cc[K_VMIN];
        uint32_t n = 0;
        /* Блокируемся, пока не наберём min(VMIN,size) байт (VMIN==0 -> сколько
         * есть прямо сейчас, без блокировки). */
        if (vmin == 0) {
            while (n < size && serial_received(COM1)) {
                int ch = (int)(unsigned char)serial_getchar(COM1);
                if ((t->c_lflag & K_ISIG) && ch == t->c_cc[K_VINTR]) {
                    signal_send(current_task->id, KSIGINT); return -1;
                }
                if ((t->c_lflag & K_ISIG) && ch == t->c_cc[K_VQUIT]) {
                    signal_send(current_task->id, KSIGQUIT); return -1;
                }
                out[n++] = (char)ch;
                if (t->c_lflag & K_ECHO) echo_ch((char)ch);
            }
            return (int)n;
        }
        while (n < size) {
            int ch = wait_byte();
            if (ch < 0) return (n > 0) ? (int)n : -1;
            if ((t->c_lflag & K_ISIG) && ch == t->c_cc[K_VINTR]) {
                signal_send(current_task->id, KSIGINT); return -1;
            }
            if ((t->c_lflag & K_ISIG) && ch == t->c_cc[K_VQUIT]) {
                signal_send(current_task->id, KSIGQUIT); return -1;
            }
            out[n++] = (char)ch;
            if (t->c_lflag & K_ECHO) echo_ch((char)ch);
            if (n >= vmin) break;   /* набрали минимум — отдаём */
        }
        return (int)n;
    }

    /* ---------- канонический режим ---------- */
    uint32_t n = 0;
    for (;;) {
        int ch = wait_byte();
        if (ch < 0) return -1;            /* EINTR */

        /* CR/NL обработка (ICRNL: \r -> \n; IGNCR: \r игнор; INLCR: \n -> \r) */
        if (ch == '\r') {
            if (t->c_iflag & K_IGNCR) continue;
            if (t->c_iflag & K_ICRNL) ch = '\n';
        } else if (ch == '\n' && (t->c_iflag & K_INLCR)) {
            ch = '\r';
        }

        if ((t->c_lflag & K_ISIG) && ch == t->c_cc[K_VINTR]) {
            signal_send(current_task->id, KSIGINT); return -1;
        }
        if ((t->c_lflag & K_ISIG) && ch == t->c_cc[K_VQUIT]) {
            signal_send(current_task->id, KSIGQUIT); return -1;
        }

        /* VEOF (Ctrl-D): пустая строка -> EOF; иначе отдать накопленное без \n. */
        if (ch == t->c_cc[K_VEOF]) {
            if (n == 0) return 0;         /* EOF */
            return (int)n;
        }

        /* VERASE (забой) */
        if (ch == t->c_cc[K_VERASE] || ch == 8 /* ^H */) {
            if (n > 0) {
                n--;
                if (t->c_lflag & K_ECHO) echo_str("\b \b");
            }
            continue;
        }

        /* VKILL (^U): стереть всю строку */
        if (ch == t->c_cc[K_VKILL]) {
            while (n > 0) { n--; if (t->c_lflag & K_ECHO) echo_str("\b \b"); }
            continue;
        }

        if (ch == '\n') {
            if (t->c_lflag & K_ECHO) echo_str("\r\n");
            if (n < size) out[n++] = '\n';
            return (int)n;               /* строка завершена */
        }

        /* обычный символ */
        if (n < size) {
            out[n++] = (char)ch;
            if (t->c_lflag & K_ECHO) echo_ch((char)ch);
            /* если буфер заполнен — отдаём как есть (без ожидания \n) */
            if (n >= size) return (int)n;
        } else {
            /* буфер вызывающего меньше строки: отдаём что есть */
            return (int)n;
        }
    }
}

int tty_get_termios(ktermios_t *out) {
    if (!out) return -1;
    *out = tty_termios;
    return 0;
}

int tty_set_termios(const ktermios_t *in) {
    if (!in) return -1;
    tty_termios = *in;
    return 0;
}

void tty_get_winsize(struct kwinsize *ws) {
    if (!ws) return;
    ws->ws_row = 25;
    ws->ws_col = 80;
    ws->ws_xpixel = 0;
    ws->ws_ypixel = 0;
}
