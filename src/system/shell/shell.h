#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>

// Sink, в который оболочка пишет вывод. По умолчанию — term_print
// (GUI-терминал), но можно перенастроить на буфер emergency-режима,
// лог, буфер приложения, init.lua и т.п.
typedef void (*shell_output_fn)(const char *);

// --- Старое API (обратная совместимость) ---------------------------------
// shell_init() сейчас исключительно про GUI-терминал: чистит окно
// терминала и печатает приветствие/логотип. Если ты не используешь
// term_win, вызывать его не обязательно — оболочка работает и без него.
void shell_init(void);
void shell_handle_char(char c);

// --- Новое API: оболочка как переиспользуемый command-processor ----------
// Поменять/получить текущий sink. NULL восстанавливает дефолт (term_print).
void shell_set_output(shell_output_fn out);
shell_output_fn shell_get_output(void);

// Однократно выполнить готовую командную строку. Удобно для init.lua
// и приложений: можно "позвать" шелл, не имея под ним GUI-окна.
void shell_run_command(const char *cmd);

// Спец-клавиши F1..F12 (вызывается из keyboard.c, когда фокус в шелл-сессии).
void shell_handle_fkey(int n);

// =========================================================================
//             Emergency-режим (раньше был отдельным eshell.c)
// =========================================================================
//
// Это НЕ отдельная оболочка и не GUI-приложение. Это просто другой режим
// рендера ТОЙ ЖЕ оболочки: она перенастраивает свой output sink на
// внутренний "draw straight to framebuffer" и сама отрисовывает чёрный
// экран в стиле Arch installer / busybox TTY (история снизу вверх,
// строка ввода фиксирована внизу). Никаких окон, никакого compositor'а.
//
// Используется:
//   • вручную из кода — shell_emergency_enter();
//   • автоматически системой по биндингу SUPER+ALT+F10 (см. kernel.c,
//     который опрашивает shell_emergency_requested в главном цикле).

// Когда true, keyboard.c шлёт ввод в shell_emergency_handle_char(),
// а не в GUI/term_win.
extern volatile bool shell_emergency_active;

// Можно безопасно выставлять из IRQ. Главный цикл kmain опрашивает этот
// флаг и сам вызывает shell_emergency_enter() в нормальном контексте.
extern volatile bool shell_emergency_requested;

// Развернуть emergency-режим прямо сейчас (чёрный экран, baner, ждёт ввода).
// Возвращает управление только если флаг shell_emergency_active сброшен
// снаружи; обычно из режима выходят через команду `exit` (триггерит reboot).
void shell_emergency_enter(void);

// Вызовы из keyboard_callback'а, когда shell_emergency_active == true.
void shell_emergency_handle_char(char c);
void shell_emergency_handle_fkey(int n);

#endif
