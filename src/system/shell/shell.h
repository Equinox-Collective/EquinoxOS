#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>

// Sink, в который оболочка пишет вывод. По умолчанию — term_print
// (GUI-терминал), но можно перенастроить на emergency shell, лог,
// буфер приложения, init.lua и т.п.
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

#endif
