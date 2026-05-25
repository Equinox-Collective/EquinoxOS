#include "eshell.h"
#include "shell.h"
#include "../drivers/vesa/vesa.h"
#include "../../syslibc/string.h"
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
//                  Emergency Shell — реализация
// =============================================================================
//
// Стиль: как у Arch installer / busybox TTY. Чёрный экран, моноширинный
// 8x8 шрифт, история строк сверху вниз (новые строки появляются снизу,
// старые "уезжают" вверх). Никаких окон. Никакого compositor'а.
//
// Рисуем напрямую функциями *_direct из vesa.c — они пишут в fb_base_addr
// мимо backbuffer'а, так что нам не нужен ни цикл компоновки, ни
// running sysgui (его как раз killall убил).

// --- Параметры рендера -------------------------------------------------------

#define ESHELL_CHAR_W       8       // ширина 8x8 шрифта
#define ESHELL_CHAR_H       8
#define ESHELL_LINE_H       10      // вертикальный шаг между строками
#define ESHELL_LEFT_PAD     8
#define ESHELL_BOTTOM_PAD   8
#define ESHELL_FG           0xC8C8C8
#define ESHELL_FG_DIM       0x808080
#define ESHELL_FG_PROMPT    0x00C864
#define ESHELL_BG           0x000000

// --- Буферы ------------------------------------------------------------------

#define ESHELL_MAX_LINES    256
#define ESHELL_LINE_W       256     // максимум символов в строке (на хранение)
#define ESHELL_INPUT_MAX    128

static char  s_lines[ESHELL_MAX_LINES][ESHELL_LINE_W];
static int   s_line_count = 0;      // сколько строк уже завершено (\n)
static char  s_current[ESHELL_LINE_W]; // строка, которая ещё не закрыта \n
static int   s_current_len = 0;

static char  s_input[ESHELL_INPUT_MAX];
static int   s_input_len = 0;

volatile bool emergency_shell_active = false;
volatile bool emergency_shell_requested = false;

// --- Утилиты с состоянием ----------------------------------------------------

static void eshell_reset_state(void) {
    s_line_count = 0;
    s_current_len = 0;
    s_current[0] = '\0';
    s_input_len = 0;
    s_input[0] = '\0';
}

static void eshell_push_line(const char *line) {
    if (s_line_count == ESHELL_MAX_LINES) {
        // Скользящее окно: сдвигаем всё на одну вверх.
        for (int i = 0; i < ESHELL_MAX_LINES - 1; i++) {
            // strcpy безопасен, потому что обе строки в пределах буфера.
            strcpy(s_lines[i], s_lines[i + 1]);
        }
        s_line_count = ESHELL_MAX_LINES - 1;
    }
    int n = 0;
    while (line[n] && n < ESHELL_LINE_W - 1) {
        s_lines[s_line_count][n] = line[n];
        n++;
    }
    s_lines[s_line_count][n] = '\0';
    s_line_count++;
}

// Печать строки в наш буфер. Поддержка \n, табы расширяем в пробелы,
// ANSI escape-последовательности тупо съедаем (CSI ... letter).
static void eshell_buffer_write(const char *s) {
    if (!s) return;
    while (*s) {
        unsigned char c = (unsigned char)*s++;

        if (c == 0x1B) { // ESC
            // Пропускаем до первой буквы (m/H/J/...)
            if (*s == '[') {
                s++;
                while (*s && !((*s >= 'A' && *s <= 'Z') ||
                               (*s >= 'a' && *s <= 'z'))) {
                    s++;
                }
                if (*s) s++;
            }
            continue;
        }

        if (c == '\r') {
            s_current_len = 0;
            s_current[0] = '\0';
            continue;
        }

        if (c == '\n') {
            s_current[s_current_len] = '\0';
            eshell_push_line(s_current);
            s_current_len = 0;
            s_current[0] = '\0';
            continue;
        }

        if (c == '\t') {
            // Простейший таб: 4 пробела, без выравнивания по колонке.
            for (int k = 0; k < 4 && s_current_len < ESHELL_LINE_W - 1; k++) {
                s_current[s_current_len++] = ' ';
            }
            s_current[s_current_len] = '\0';
            continue;
        }

        if (c == '\b') {
            if (s_current_len > 0) {
                s_current_len--;
                s_current[s_current_len] = '\0';
            }
            continue;
        }

        if (c < 32 || c > 126) {
            // Прочие управляющие символы (включая \x11/\x12 для стрелок)
            // — игнорируем, чтобы не засорять отображение.
            continue;
        }

        if (s_current_len < ESHELL_LINE_W - 1) {
            s_current[s_current_len++] = (char)c;
            s_current[s_current_len] = '\0';
        }
    }
}

// --- Рендер -----------------------------------------------------------------

void eshell_redraw(void) {
    // 1. Чёрный фон во весь экран.
    draw_rect_direct(0, 0, (int)screen_width, (int)screen_height, ESHELL_BG);

    // 2. Опциональный header сверху — чтобы выглядело как Arch installer.
    vesa_draw_string_direct(
        "EquinoxOS emergency shell -- type 'help' for commands, 'exit' "
        "to reboot",
        ESHELL_LEFT_PAD, 4, ESHELL_FG_DIM);
    draw_rect_direct(0, ESHELL_LINE_H + 4,
                     (int)screen_width, 1, 0x202020);

    // 3. Самая нижняя строка — текущий input prompt.
    int input_y = (int)screen_height - ESHELL_LINE_H - ESHELL_BOTTOM_PAD;
    vesa_draw_string_direct("# ", ESHELL_LEFT_PAD, input_y,
                            ESHELL_FG_PROMPT);
    vesa_draw_string_direct(s_input,
                            ESHELL_LEFT_PAD + 2 * ESHELL_CHAR_W,
                            input_y, ESHELL_FG);

    // курсор (мигать пока не будем — это всё-таки emergency mode)
    int cursor_x = ESHELL_LEFT_PAD +
                   (2 + s_input_len) * ESHELL_CHAR_W;
    draw_rect_direct(cursor_x, input_y,
                     ESHELL_CHAR_W, ESHELL_CHAR_H, ESHELL_FG);

    // 4. История — снизу вверх. Сначала недозакрытая s_current
    // (то, что shell написал, но без финального \n).
    int y = input_y - ESHELL_LINE_H;

    if (s_current_len > 0 && y > ESHELL_LINE_H) {
        vesa_draw_string_direct(s_current, ESHELL_LEFT_PAD, y, ESHELL_FG);
        y -= ESHELL_LINE_H;
    }

    for (int i = s_line_count - 1; i >= 0 && y > ESHELL_LINE_H + 4; i--) {
        vesa_draw_string_direct(s_lines[i], ESHELL_LEFT_PAD, y, ESHELL_FG);
        y -= ESHELL_LINE_H;
    }
}

// --- Публичный API -----------------------------------------------------------

void eshell_print(const char *s) {
    eshell_buffer_write(s);
    eshell_redraw();
}

static void eshell_banner(void) {
    eshell_buffer_write(
        "Emergency shell.\n"
        "GUI subsystem stopped. Direct framebuffer mode.\n"
        "Commands: help ps kill killall fetch ls reboot exit\n"
        "\n");
}

void eshell_handle_char(char c) {
    if (!emergency_shell_active) return;

    if (c == '\n') {
        // 1. Эхо команды в историю.
        char echo[ESHELL_INPUT_MAX + 8];
        int j = 0;
        echo[j++] = '#';
        echo[j++] = ' ';
        for (int i = 0; i < s_input_len && j < (int)sizeof(echo) - 2; i++) {
            echo[j++] = s_input[i];
        }
        echo[j++] = '\n';
        echo[j] = '\0';
        eshell_buffer_write(echo);

        // 2. Спецкоманда `exit` — выйти и перезагрузиться.
        if (strcmp(s_input, "exit") == 0) {
            eshell_buffer_write("Rebooting...\n");
            eshell_redraw();
            struct { uint16_t l; uint64_t b; }
                __attribute__((packed)) idt = {0, 0};
            __asm__ volatile("lidt %0; int3" : : "m"(idt));
            for (;;) __asm__("hlt");
        }

        // 3. Иначе — отдаём готовую командную строку обычному shell.
        // shell.c сам напечатает результат через текущий sink (нас).
        shell_run_command(s_input);

        s_input_len = 0;
        s_input[0] = '\0';
        eshell_redraw();
        return;
    }

    if (c == '\b') {
        if (s_input_len > 0) {
            s_input_len--;
            s_input[s_input_len] = '\0';
            eshell_redraw();
        }
        return;
    }

    if (c >= 32 && c <= 126 && s_input_len < ESHELL_INPUT_MAX - 1) {
        s_input[s_input_len++] = c;
        s_input[s_input_len] = '\0';
        eshell_redraw();
    }
}

void eshell_handle_fkey(int n) {
    switch (n) {
    case 1:
        // F1 — help
        shell_run_command("help");
        eshell_redraw();
        break;
    case 2:
        // F2 — ps
        shell_run_command("ps");
        eshell_redraw();
        break;
    case 12:
        // F12 — reboot, на случай если клавиатура сломала ввод текста
        eshell_handle_char('e');
        eshell_handle_char('x');
        eshell_handle_char('i');
        eshell_handle_char('t');
        eshell_handle_char('\n');
        break;
    default:
        break;
    }
}

void eshell_enter(void) {
    // Перенаправляем весь shell-вывод на нас.
    shell_output_fn prev = shell_get_output();
    shell_set_output(eshell_print);

    eshell_reset_state();
    eshell_banner();

    emergency_shell_active = true;
    emergency_shell_requested = false;

    eshell_redraw();

    // Активная фаза. Дальше keyboard_callback зовёт eshell_handle_char()
    // напрямую (см. emergency_shell_active в keyboard.c). Здесь же мы
    // просто крутимся и даём прерываниям делать своё дело. Выход
    // только через команду `exit` (внутри eshell_handle_char -> reboot).
    while (emergency_shell_active) {
        __asm__ volatile("hlt");
    }

    // Если мы тут — значит кто-то снаружи сбросил флаг. Восстановим sink.
    shell_set_output(prev);
}
