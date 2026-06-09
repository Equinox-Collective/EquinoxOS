/*
 * ttytest — интерактивный тест Этапа 5 (tty/termios) на консоли COM1.
 *
 * Запускать в serial-консоли (`make run`, окно где `-serial stdio`).
 *
 * Тесты:
 *   A) isatty + размер окна (TIOCGWINSZ).
 *   B) Канонический режим: ввод строки с эхо/забоем, Enter завершает.
 *      Введи "exit" чтобы пропустить остаток и выйти.
 *   C) SIGINT: нажми Ctrl-C — сработает обработчик (ISIG в каноне).
 *   D) Сырой режим (cfmakeraw): посимвольное чтение без эха; печатаем коды;
 *      'q' завершает; termios восстанавливается.
 *
 * Это интерактивный тест — «ожидаемый вывод» зависит от того, что ты вводишь.
 */
#include <equos.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>

static volatile int got_sigint = 0;
static void on_sigint(int s) {
    (void)s;
    printf("\n[handler] caught SIGINT\n");
    got_sigint = 1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("=== ttytest (Этап 5) ===\n");

    /* A) isatty + winsize */
    printf("[A] isatty(0)=%d isatty(1)=%d\n", isatty(0), isatty(1));
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0)
        printf("[A] winsize: %d rows x %d cols\n", ws.ws_row, ws.ws_col);
    else
        printf("[A] TIOCGWINSZ failed\n");

    /* B) канонический ввод строки */
    printf("[B] Введи строку и нажми Enter (или 'exit'):\n> ");
    char line[128];
    int n = (int)read(0, line, sizeof(line) - 1);
    if (n < 0) n = 0;
    line[n] = '\0';
    /* убрать завершающий перевод строки для аккуратного вывода */
    if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
    printf("[B] прочитано %d байт: \"%s\"\n", n, line);
    if (strcmp(line, "exit") == 0) {
        printf("=== ttytest done (early) ===\n");
        return 0;
    }

    /* C) SIGINT по Ctrl-C (канонический режим, ISIG активен) */
    signal(SIGINT, on_sigint);
    printf("[C] Нажми Ctrl-C для проверки SIGINT...\n");
    while (!got_sigint) {
        char tmp[32];
        int r = (int)read(0, tmp, sizeof(tmp));
        if (r < 0) continue;        /* прервано сигналом — проверим флаг */
        /* если ввели обычную строку — подскажем ещё раз */
        if (!got_sigint) printf("[C] (всё ещё жду Ctrl-C)\n");
    }
    printf("[C] SIGINT получен, продолжаем.\n");

    /* D) сырой режим */
    struct termios saved, raw;
    if (tcgetattr(0, &saved) != 0) {
        printf("[D] tcgetattr failed, пропуск\n");
        printf("=== ttytest done ===\n");
        return 0;
    }
    raw = saved;
    cfmakeraw(&raw);
    tcsetattr(0, TCSANOW, &raw);
    printf("[D] Сырой режим: жми клавиши (без эха), коды печатаются; 'q' = выход\r\n");
    for (;;) {
        char c;
        int r = (int)read(0, &c, 1);
        if (r <= 0) continue;
        printf("[0x%02x %c]\r\n", (unsigned char)c,
               (c >= 32 && c < 127) ? c : '.');
        if (c == 'q') break;
    }
    tcsetattr(0, TCSANOW, &saved);   /* восстановить */
    printf("\n[D] termios восстановлен.\n");

    printf("=== ttytest done ===\n");
    return 0;
}
