#include "terminal.h"
#include "../syslibc/string.h"
#include "../system/misc/timer.h"

#define TERM_LINES 100
#define TERM_COLS 80
static bool cursor_visible = true;
extern char shell_buffer[256]; // Твой буфер ввода из shell.c
static char term_buffer[TERM_LINES][TERM_COLS];
static uint32_t color_buffer[TERM_LINES][TERM_COLS];
static int cursor_x = 0;
static int cursor_y = 0;
static uint32_t current_fg = 0x50FA7B;
bool terminal_matrix_mode = false;

// Состояния парсера ANSI
typedef enum {
  ANSI_STATE_IDLE,
  ANSI_STATE_ESC,
  ANSI_STATE_BRACKET,
  ANSI_STATE_PARAMS
} ansi_state_t;

static ansi_state_t state = ANSI_STATE_IDLE;
static int ansi_params[8];
static int ansi_param_idx = 0;

void terminal_print_char(char c) {
  if (state == ANSI_STATE_IDLE) {
    if (c == 27) {
      state = ANSI_STATE_ESC;
      return;
    }

    if (c == '\n') {
      cursor_x = 0;
      cursor_y++;
    } else if (c == '\r') {
      cursor_x = 0;
    } else if (c == '\b') {
      if (cursor_x > 0) {
        cursor_x--;
        term_buffer[cursor_y][cursor_x] = ' '; // Стираем символ физически
      }
    } else {
      if (cursor_x < TERM_COLS - 1) {
        term_buffer[cursor_y][cursor_x] = c;
        color_buffer[cursor_y][cursor_x] = current_fg;
        cursor_x++;
      }
    }
  } else if (state == ANSI_STATE_ESC) {
    if (c == '[') {
      state = ANSI_STATE_BRACKET;
      ansi_param_idx = 0;
      memset(ansi_params, 0, sizeof(ansi_params));
    } else {
      state = ANSI_STATE_IDLE;
    }
  } else if (state == ANSI_STATE_BRACKET) {
    if (c == '?') {
      return;
    } // Просто пропускаем знак вопроса для последовательностей типа \e[?25l
    if (c >= '0' && c <= '9') {
      ansi_params[ansi_param_idx] =
          ansi_params[ansi_param_idx] * 10 + (c - '0');
    } else if (c == ';') {
      if (ansi_param_idx < 7)
        ansi_param_idx++;
    } else {
      int val = (ansi_params[0] == 0) ? 1 : ansi_params[0]; // По умолчанию 1

      switch (c) {
      case 'A':
        cursor_y = (cursor_y - val < 0) ? 0 : cursor_y - val;
        break; // Up
      case 'B':
        cursor_y =
            (cursor_y + val >= TERM_LINES) ? TERM_LINES - 1 : cursor_y + val;
        break; // Down
      case 'C':
        cursor_x =
            (cursor_x + val >= TERM_COLS) ? TERM_COLS - 1 : cursor_x + val;
        break; // Right
      case 'D':
        cursor_x = (cursor_x - val < 0) ? 0 : cursor_x - val;
        break; // Left

      case 'H':
      case 'f': // Прямая установка: \e[row;colH
        cursor_y = (ansi_params[0] > 0) ? ansi_params[0] - 1 : 0;
        cursor_x = (ansi_params[1] > 0) ? ansi_params[1] - 1 : 0;
        break;

      case 'K':                    // Стирание в строке
        if (ansi_params[0] == 0) { // От курсора до конца строки
          for (int i = cursor_x; i < TERM_COLS; i++)
            term_buffer[cursor_y][i] = ' ';
        } else if (ansi_params[0] == 2) { // Вся строка
          memset(term_buffer[cursor_y], ' ', TERM_COLS);
        }
        break;

      case 'J': // Очистка экрана
        if (ansi_params[0] == 2) {
          terminal_clear();
          cursor_x = 0;
          cursor_y = 0;
        }
        break;

      case 'm': // Цвета
        for (int i = 0; i <= ansi_param_idx; i++) {
          int p = ansi_params[i];
          if (p == 0)
            current_fg = 0x50FA7B;
          else if (p == 31)
            current_fg = 0xFF5555;
          else if (p == 32)
            current_fg = 0x50FA7B;
          else if (p == 33)
            current_fg = 0xF1FA8C;
          else if (p == 34)
            current_fg = 0xBD93F9;
          else if (p == 35)
            current_fg = 0xFF79C6; // Pink
          else if (p == 36)
            current_fg = 0x8BE9FD;
          else if (p == 37)
            current_fg = 0xF8F8F2;
        }
        break;

      case 'h':
        if (ansi_params[0] == 25)
          cursor_visible = true;
        break; // Show cursor
      case 'l':
        if (ansi_params[0] == 25)
          cursor_visible = false;
        break; // Hide cursor
      }
      state = ANSI_STATE_IDLE;
    }
  }

  // Скроллинг
  if (cursor_y >= TERM_LINES) {
    for (int i = 0; i < TERM_LINES - 1; i++) {
      memcpy(term_buffer[i], term_buffer[i + 1], TERM_COLS);
      memcpy(color_buffer[i], color_buffer[i + 1], TERM_COLS * 4);
    }
    memset(term_buffer[TERM_LINES - 1], 0, TERM_COLS);
    cursor_y = TERM_LINES - 1;
  }
}
void terminal_clear() {
  for (int i = 0; i < TERM_LINES; i++) {
    memset(term_buffer[i], 0, TERM_COLS);
    for (int j = 0; j < TERM_COLS; j++) {
      color_buffer[i][j] = 0x50FA7B; // Гарантируем, что цвет не 0
    }
  }
  cursor_x = 0;
  cursor_y = 0;
}
void terminal_print(const char *str) {
  while (*str) {
    terminal_print_char(*str++);
  }
}

void terminal_render(window_t *self) {
  // 1. Фон терминала
  if (terminal_matrix_mode) {
    gui_window_draw_rect(self, 0, 0, self->w, self->h, 0x000000);
    /* Matrix rain — tick is now in ms (PIT @ 1 kHz). Slow the visual rate
     * down by ÷20 so the animation keeps the same on-screen speed it had
     * back when 1 tick = 20 ms. */
    uint32_t mtick = tick / 20;
    for (int i = 0; i < 200; i++) {
       int x = (i * 13) % self->w;
       int speed = 2 + (i % 4);
       int y = ((mtick * speed) + i * 53) % self->h;
       char c[2] = { (char)(33 + ((mtick + i) % 90)), 0 };
       uint32_t color = (i % 5 == 0) ? 0xFFFFFF : 0x00FF00;
       gui_window_draw_string(self, c, x, y, color);
    }
  } else {
    gui_window_draw_rect(self, 0, 0, self->w, self->h, 0x0F0F12);
  }

  // 2. Рамка для области ввода (внизу)
  int prompt_y = self->h - 25;
  gui_window_draw_rect(self, 0, prompt_y, self->w, 25, 0x1A1A24);
  gui_window_draw_rect(self, 0, prompt_y, self->w, 1, 0x333344); // Разделитель

  // 3. РИСУЕМ ТО, ЧТО ТЫ ПЕЧАТАЕШЬ ПРЯМО СЕЙЧАС
  gui_window_draw_string(self, ">>", 8, prompt_y + 6, 0x8BE9FD); // Синий промпт
  gui_window_draw_string(self, shell_buffer, 32, prompt_y + 6,
                         0xF8F8F2); // Белый текст юзера

  // 4. Считаем область для истории (буфера)
  int line_height = 14;
  int visible_lines = (prompt_y - 10) / line_height;

  int start_line = 0;
  if (cursor_y >= visible_lines) {
    start_line = cursor_y - visible_lines + 1;
  }

  // 5. РИСУЕМ ИСТОРИЮ (ANSI БУФЕР)
  for (int i = 0; i < visible_lines; i++) {
    int line_idx = start_line + i;
    if (line_idx >= TERM_LINES)
      break;

    for (int x = 0; x < TERM_COLS; x++) {
      char c = term_buffer[line_idx][x];
      if (c != 0) {
        char s[2] = {c, 0};
        uint32_t color = color_buffer[line_idx][x];
        if (color == 0)
          color = 0x50FA7B; // Защита от "невидимки"
        gui_window_draw_string(self, s, 8 + x * 8, 8 + i * line_height, color);
      }
    }
  }

  // 6. Мигающий курсор в строке ввода (500 ms blink, tick = ms)
  if ((tick / 500) % 2 == 0) {
    int input_cursor_x = 32 + strlen(shell_buffer) * 8;
    gui_window_draw_rect(self, input_cursor_x, prompt_y + 16, 8, 2, 0x8BE9FD);
  }
}