/*
 * ttytest — interactive test of Stage 5 (tty/termios) on the COM1 console.
 *
 * Run in the serial console (`make run`, the `-serial mon:stdio` window).
 * Type into the SAME window where kernel output appears.
 *
 * NOTE: prompts are ASCII so they render correctly regardless of the host
 * terminal codepage (no UTF-8/CP866 mojibake).
 *
 * Tests:
 *   A) isatty + window size (TIOCGWINSZ).
 *   B) Canonical mode: line input with echo/backspace, Enter ends it.
 *      Type "exit" to skip the rest.
 *   C) SIGINT: press Ctrl-C -> handler fires (ISIG in canonical mode).
 *   D) Raw mode (cfmakeraw): single-char reads, no echo, codes printed;
 *      'q' quits; termios is restored.
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
    printf("=== ttytest (Stage 5: tty/termios) ===\n");

    /* A) isatty + winsize */
    printf("[A] isatty(0)=%d isatty(1)=%d\n", isatty(0), isatty(1));
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0)
        printf("[A] winsize: %d rows x %d cols\n", ws.ws_row, ws.ws_col);
    else
        printf("[A] TIOCGWINSZ failed\n");

    /* B) canonical line input */
    printf("[B] Type a line and press Enter (try Backspace; 'exit' to skip):\n> ");
    char line[128];
    int n = (int)read(0, line, sizeof(line) - 1);
    if (n < 0) n = 0;
    line[n] = '\0';
    if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
    printf("[B] read %d bytes: \"%s\"\n", n, line);
    if (strcmp(line, "exit") == 0) {
        printf("=== ttytest done (early) ===\n");
        return 0;
    }

    /* C) SIGINT via Ctrl-C (canonical mode, ISIG active) */
    signal(SIGINT, on_sigint);
    printf("[C] Press Ctrl-C to test SIGINT...\n");
    while (!got_sigint) {
        char tmp[32];
        int r = (int)read(0, tmp, sizeof(tmp));
        if (r < 0) continue;               /* interrupted by signal */
        if (!got_sigint) printf("[C] (still waiting for Ctrl-C)\n");
    }
    printf("[C] SIGINT received, continuing.\n");

    /* D) raw mode */
    struct termios saved, raw;
    if (tcgetattr(0, &saved) != 0) {
        printf("[D] tcgetattr failed, skipping\n");
        printf("=== ttytest done ===\n");
        return 0;
    }
    raw = saved;
    cfmakeraw(&raw);
    tcsetattr(0, TCSANOW, &raw);
    printf("[D] Raw mode: press keys (no echo), codes printed; 'q' = quit\r\n");
    for (;;) {
        char c;
        int r = (int)read(0, &c, 1);
        if (r <= 0) continue;
        printf("[0x%02x %c]\r\n", (unsigned char)c,
               (c >= 32 && c < 127) ? c : '.');
        if (c == 'q') break;
    }
    tcsetattr(0, TCSANOW, &saved);          /* restore */
    printf("\n[D] termios restored.\n");

    printf("=== ttytest done ===\n");
    return 0;
}
