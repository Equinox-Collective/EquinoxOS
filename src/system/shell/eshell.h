#ifndef ESHELL_H
#define ESHELL_H

#include <stdbool.h>

// =============================================================================
//                  Emergency Shell (eshell) — "Arch installer mode"
// =============================================================================
//
// Это НЕ окно и не GUI-приложение. Это аварийная оболочка, которая
// рисует прямо в видеобуфер чёрный экран и текстовую сессию шелла
// (см. shell.h). Вызывается:
//   • вручную из пользовательского кода (eshell_enter()),
//   • из init.lua/приложения, которое хочет "терминал поверх всего",
//   • автоматически системой при экстренном бинде SUPER+ALT+F10.
//
// Эта подсистема живёт параллельно обычной оболочке (shell.c):
// command-processor у них один, отличается только sink (eshell получает
// вывод и сам рисует его в фреймбуфер снизу вверх).

// Когда true, keyboard.c шлёт весь ввод в eshell, а не в GUI/term_win.
extern volatile bool emergency_shell_active;

// Запросить переход в emergency shell. Безопасно вызывать из IRQ
// (только выставляет флаг). Реальный вход произойдёт в kmain main loop.
extern volatile bool emergency_shell_requested;

// Развернуть emergency shell прямо сейчас. Перерисовывает экран в
// чёрный, выводит баннер, ставит sink shell-а на eshell_print и
// крутит свой ввод до тех пор, пока пользователь не наберёт `exit`.
void eshell_enter(void);

// Принять символ ввода (вызывается keyboard_callback'ом, когда
// emergency_shell_active == true).
void eshell_handle_char(char c);

// Спец-клавиши (F1..F12, стрелки) в emergency shell.
void eshell_handle_fkey(int n);

// Печать произвольной строки. Это и есть sink, который ставится в
// shell_set_output() при входе в eshell. Поддерживает '\n' и просто
// игнорирует ANSI escape-последовательности.
void eshell_print(const char *s);

// Полная перерисовка экрана (чёрный фон + история снизу вверх + строка
// ввода). Вызывается изнутри после каждого изменения состояния.
void eshell_redraw(void);

#endif
