#include "keyboard.h"
#include "../../../../gui/gui.h"
#include "../../../core/io.h"
#include "../../../shell/shell.h"
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
//                          PS/2 keyboard driver
// =============================================================================
//
// Делает три вещи:
//   1) Складывает скан-коды в кольцевой буфер (для тех, кто опрашивает
//      keyboard_pop() / sys_get_scancode()).
//   2) Ведёт состояние модификаторов: Shift, Ctrl, Alt, Super (Win).
//   3) Раздаёт ASCII-символы и спец-клавиши в текущий "фокус":
//        • Emergency shell, если он активен.
//        • Иначе — окно с фокусом (term_win / notepad_win).
//
// Поддержка extended-скан-кодов (0xE0): нужно для клавиш Super (Win),
// правого Alt/Ctrl и стрелок. Когда видим 0xE0, ставим флаг и следующий
// байт интерпретируем как extended.

extern volatile uint8_t last_scancode;
extern bool is_app_running;
extern void notepad_handle_char(char c);

static uint8_t key_buffer[128];
static int key_head = 0;
static int key_tail = 0;

static bool shift_pressed = false;
static bool ctrl_pressed  = false;
static bool alt_pressed   = false;
static bool super_pressed = false;   // Win / Super
static bool extended      = false;   // только что видели 0xE0

bool keyboard_super_pressed(void) { return super_pressed; }
bool keyboard_alt_pressed(void)   { return alt_pressed; }
bool keyboard_ctrl_pressed(void)  { return ctrl_pressed; }
bool keyboard_shift_pressed(void) { return shift_pressed; }

static const char ascii_table[] = {
    0,   27,  '1',  '2',  '3',  '4', '5', '6',  '7', '8', '9', '0',
    '-', '=', '\b', '\t', 'q',  'w', 'e', 'r',  't', 'y', 'u', 'i',
    'o', 'p', '[',  ']',  '\n', 0,   'a', 's',  'd', 'f', 'g', 'h',
    'j', 'k', 'l',  ';',  '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm',  ',',  '.',  '/', 0,   '*',  0,   ' '};

static const char ascii_table_shift[] = {
    0,   27,  '!',  '@',  '#',  '$', '%', '^', '&', '*', '(', ')',
    '_', '+', '\b', '\t', 'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{',  '}',  '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H',
    'J', 'K', 'L',  ':',  '"',  '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M',  '<',  '>',  '?', 0,   '*', 0,   ' '};

char get_ascii_char(uint8_t scancode) {
  // Поддержка shift здесь делается через сайд-эффект на shift_pressed —
  // оставлено как было, чтобы не ломать существующих вызывающих.
  if (scancode == 0x2A || scancode == 0x36) {
    shift_pressed = true;
    return 0;
  }
  if (scancode == 0xAA || scancode == 0xB6) {
    shift_pressed = false;
    return 0;
  }
  if (scancode & 0x80) return 0;
  if (scancode >= sizeof(ascii_table)) return 0;
  return shift_pressed ? ascii_table_shift[scancode] : ascii_table[scancode];
}

void keyboard_push(uint8_t scancode) {
  int next = (key_head + 1) % 128;
  if (next != key_tail) {
    key_buffer[key_head] = scancode;
    key_head = next;
  }
}

uint8_t keyboard_pop(void) {
  if (key_head == key_tail) return 0;
  uint8_t sc = key_buffer[key_tail];
  key_tail = (key_tail + 1) % 128;
  return sc;
}

// PS/2 set 1 scancodes for F-keys. F11/F12 идут не подряд.
static int scancode_to_fkey(uint8_t code) {
  if (code >= 0x3B && code <= 0x44) return code - 0x3B + 1; // F1..F10
  if (code == 0x57) return 11;
  if (code == 0x58) return 12;
  return -1;
}

void keyboard_callback(void) {
  uint8_t scancode = inb(0x60);

  // 1. Extended prefix. Запоминаем для собственной интерпретации
  // модификаторов (super/правый-ctrl и т.д.) И одновременно кладём 0xE0
  // в кольцевой буфер: иначе ring-3 приложения, читающие через
  // SYS_GET_SCANCODE (например, doom — см. DG_GetKey в
  // app/doom/doomgeneric_equos.c), никогда не увидят prefix\'а и не
  // распознают стрелки/правые модификаторы. Реальный код придёт
  // следующим прерыванием.
  if (scancode == 0xE0) {
    extended = true;
    keyboard_push(0xE0);
    return;
  }

  bool is_release = (scancode & 0x80) != 0;
  uint8_t code = scancode & 0x7F;
  bool was_extended = extended;
  extended = false;

  // 2. Модификаторы.
  //    Левый Ctrl: 0x1D / 0x9D. Правый Ctrl: E0 0x1D / E0 0x9D.
  //    Левый Alt:  0x38 / 0xB8. Правый Alt (AltGr): E0 0x38 / E0 0xB8.
  //    Левый Shift:0x2A / 0xAA. Правый Shift:      0x36 / 0xB6.
  //    Super (Win) — ТОЛЬКО extended: E0 0x5B / E0 0x5C.
  //
  // ВАЖНО про Shift: shift_pressed раньше обновлялся только как побочный
  // эффект из get_ascii_char(), а get_ascii_char() ниже не вызывается для
  // release-скан-кодов (есть `if (is_release) return;`). В итоге, если
  // пользователь нажимал Shift и одновременно ловил emergency (Super+Alt
  // +F10) — release-код 0xAA пролетал мимо, shift_pressed залипал в true
  // и весь последующий ввод шёл в верхнем регистре, причём отменить это
  // было невозможно. Теперь обновляем shift_pressed прямо здесь, на
  // press и release, до раннего возврата по is_release.
  if (code == 0x1D) {
    ctrl_pressed = !is_release;
  } else if (code == 0x38) {
    alt_pressed = !is_release;
  } else if (code == 0x2A || code == 0x36) {
    shift_pressed = !is_release;
  } else if (was_extended && (code == 0x5B || code == 0x5C)) {
    super_pressed = !is_release;
  }

  // 3. Полные скан-коды (с битом отпускания) — в кольцо для опроса.
  keyboard_push(scancode);

  if (is_release) return; // дальше нас интересуют только нажатия

  // 4. Системные хоткеи. SUPER+ALT+F10 — emergency-режим оболочки
  // (killall + чёрный экран). Обрабатывается ВНЕ зависимости от того,
  // кто сейчас в фокусе.
  int fkey = scancode_to_fkey(code);
  if (fkey > 0 && super_pressed && alt_pressed && fkey == 10) {
    // Из IRQ ничего тяжёлого не делаем — просто сигналим главному
    // циклу в kmain.
    shell_emergency_requested = true;
    return;
  }

  // 5. Куда отдавать ввод.
  if (shell_emergency_active) {
    if (fkey > 0) {
      shell_emergency_handle_fkey(fkey);
      return;
    }
    // Сначала shift/extended выпиливаем — get_ascii_char уже учитывает
    // shift_pressed по сайд-эффекту выше.
    char c = was_extended ? 0 : get_ascii_char(scancode);
    if (c > 0) shell_emergency_handle_char(c);
    return;
  }

  // Не-emergency. Передаём по фокусу.
  char c = was_extended ? 0 : get_ascii_char(scancode);

  if (focused_window == term_win) {
    if (c > 0) {
      shell_handle_char(c);
    } else {
      // F-клавиши в обычной shell-сессии — отдаём на верхний уровень.
      if (fkey > 0) {
        shell_handle_fkey(fkey);
        return;
      }
      switch (code) {
      case 0x48: shell_handle_char('\x11'); break; // Up
      case 0x50: shell_handle_char('\x12'); break; // Down
      case 0x0F: shell_handle_char('\t');   break; // Tab
      }
    }
  } else if (focused_window == notepad_win && c > 0) {
    notepad_handle_char(c);
  }
}
