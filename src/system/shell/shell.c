#include "shell.h"
#include "shellsyntx.h"
#include "../drivers/devices/pcspeaker/pcspeaker.h"
#include "../drivers/vesa/bmp.h"
#include "../drivers/vesa/vesa.h"
#include "../fs/vfs.h"
#include "../../syslibc/stdio.h"
#include "../../syslibc/string.h"
#include "../mem/memory.h"
#include "../usr/task.h"
#include "../mem/pmm.h"
#include "../drivers/hardware/net/arp.h"
#include "../drivers/hardware/net/dns.h"
#include "../drivers/hardware/net/icmp.h"
#include "../drivers/hardware/net/net.h"
#include "../drivers/hardware/net/tcp.h"
#include "../drivers/hardware/net/udp.h"
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
//                              ВЫВОД ОБОЛОЧКИ
// =============================================================================
//
// Раньше shell.c звал term_print() напрямую и был намертво прибит к
// GUI-терминалу (term_win). Теперь это полноценная оболочка-процессор:
// весь вывод идёт через подменяемый sink. Дефолт — term_print, но
// emergency-режим, init.lua, утилита или приложение могут перенаправить
// вывод к себе одним вызовом shell_set_output().

extern void term_print(const char *str);

static void default_output(const char *s) { term_print(s); }
static shell_output_fn s_out = default_output;

static void sh_print(const char *s) {
    if (s_out)
        s_out(s);
}

void shell_set_output(shell_output_fn out) {
    s_out = out ? out : default_output;
}

shell_output_fn shell_get_output(void) { return s_out; }

// =============================================================================
//                            ВСПОМОГАТЕЛЬНОЕ
// =============================================================================

// Оставлено на будущее под net-команды (ping, arp).
__attribute__((unused))
static uint32_t parse_ip(const char* s) {
    uint32_t res = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            s++;
        }
        res = (res << 8) | val;
        if (*s == '.') s++;
    }
    return res;
}

// Простейший atoi для команд kill/sleep и т.п. — не зависит от syslibc.
static int sh_atoi(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return sign * v;
}

extern void init_fs();
extern void list_files();
extern void create_file(char *name, char *content);
extern void read_file(char *name);
extern void exec_module_elf();
extern bool should_run_app;
extern void show();

// Реализована в kernel.c. Безопасно завершает все пользовательские
// задачи и запрашивает emergency-режим оболочки на чёрном экране.
extern void emergency_kill_all_and_shell(void);

// =============================================================================
//                              СОСТОЯНИЕ
// =============================================================================

char shell_buffer[64] = {0};
int shell_idx = 0;
#define MAX_HISTORY 10
char history[MAX_HISTORY][64];
int history_count = 0;
int history_browse_idx = -1;

// =============================================================================
//                              УТИЛИТЫ ВЫВОДА
// =============================================================================

static void print_logo(void) {
    sh_print("\e[36m        eeeeeeee        \n");
    sh_print("\e[36m      eee      eee      \n");
    sh_print("\e[36m     eee      eee      \e[37mEquinox\e[0m OS\n");
    sh_print("\e[36m    eeeeeeeeeee         \e[37mCore: \e[32mx86_64\n");
    sh_print("\e[36m    eee                 \e[37mShell: \e[32meqsh 1.1\n");
    sh_print("\e[36m     eee      eee      \n");
    sh_print("\e[36m      eeeeeeeeee       \n");
    sh_print("\e[0m\n");
}

static void shell_prompt(void) {
    sh_print("\e[32muser@equos\e[0m:\e[34m/\e[0m$ ");
}

void shell_init(void) {
    print_logo();
    shell_prompt();
}

// =============================================================================
//                              КОМАНДЫ
// =============================================================================

/* cmd_help() заменён на cmd_help_fn() ниже — он печатает реестр
 * SHELL_COMMANDS[] из shellsyntx.h, чтобы не дублировать список. */

static void cmd_ls(void) {
    vfs_node_t *dev = vfs_root->next;
    while (dev) {
        sh_print("\e[34m[");
        sh_print(dev->name);
        sh_print("]\e[0m  ");
        dev = dev->next;
    }
    sh_print("\n");
}

static void cmd_fetch(void) {
    print_logo();
    char mem[64];
    sprintf(mem, "Memory: %d MB / %d MB\n",
            pmm_get_used_memory() / 1024 / 1024,
            pmm_get_total_memory() / 1024 / 1024);
    sh_print(mem);
}

// Свой `ps`, который уважает текущий sink (task_list_all() пишет
// напрямую в term_print и поэтому годится только для GUI-терминала).
static void cmd_ps(void) {
    task_t *start = task_get_list_head();
    if (!start) {
        sh_print("ps: scheduler not initialized\n");
        return;
    }
    sh_print("\e[33m PID   STATE       CR3              BRK\e[0m\n");
    task_t *curr = start;
    do {
        char buf[128];
        const char *state = curr->running ? "RUNNING" : "STOPPED";
        sprintf(buf, " %d     %s     %x   %x\n",
                (uint32_t)curr->id, state,
                (uint32_t)curr->cr3, (uint32_t)curr->brk);
        sh_print(buf);
        curr = curr->next;
    } while (curr && curr != start);
}

static void cmd_kill(const char *args) {
    while (*args == ' ') args++;
    if (!*args) {
        sh_print("kill: usage: kill <pid>\n");
        return;
    }
    int pid = sh_atoi(args);
    if (pid <= 0) {
        sh_print("kill: invalid pid\n");
        return;
    }
    if (!task_terminate_by_pid((uint64_t)pid)) {
        char buf[64];
        sprintf(buf, "kill: failed to terminate pid %d\n", pid);
        sh_print(buf);
    }
}

static void cmd_reboot(void) {
    sh_print("Rebooting...\n");
    // Триггерим triple fault через загрузку битого IDT.
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) idt = {0, 0};
    __asm__ volatile("lidt %0; int3" : : "m"(idt));
    for (;;) __asm__("hlt");
}

// =============================================================================
//                       РЕЕСТР КОМАНД (см. shellsyntx.h)
// =============================================================================
//
// Каждая команда — это обёртка с сигнатурой shell_cmd_fn(args). Команды
// без аргументов принимают `(void)args`. Реестр SHELL_COMMANDS[] — это
// единственный источник правды; диспетчер ниже и cmd_help() читают
// именно его.

static void cmd_help_fn(const char *args);
static void cmd_ls_fn(const char *args)     { (void)args; cmd_ls(); }
static void cmd_fetch_fn(const char *args)  { (void)args; cmd_fetch(); }
static void cmd_ps_fn(const char *args)     { (void)args; cmd_ps(); }
static void cmd_reboot_fn(const char *args) { (void)args; cmd_reboot(); }
static void cmd_kill_fn(const char *args)   { cmd_kill(args); }
static void cmd_gui_fn(const char *args) {
    (void)args;
    sh_print("Starting Equinox GUI...\n");
}
static void cmd_clear_fn(const char *args) {
    (void)args;
    if (s_out == default_output) {
        // Nothing, src/gui is deleted.
    } else {
        sh_print("\n\n");
    }
}
static void cmd_killall_fn(const char *args) {
    (void)args;
    sh_print("killall: terminating user processes...\n");
    // ВАЖНО: эту команду пользователь обычно печатает в GUI-терминале,
    // т.е. сейчас мы ВНУТРИ keyboard IRQ (keyboard_handler ->
    // keyboard_callback -> shell_handle_char -> dispatch -> сюда). EOI
    // ещё не отправлен. Если позвать emergency_kill_all_and_shell()
    // прямо здесь, он застрянет в hlt-loop в shell_emergency_enter()
    // ДО возврата из IRQ — keyboard IRQ останется in-service на PIC,
    // больше скан-кодов не придёт, emergency окажется "немым".
    // Поэтому только сигналим — kmain в главном цикле подхватит флаг
    // уже в нормальном контексте.
    shell_emergency_requested = true;
}
static void cmd_run_fn(const char *args) {
    while (*args == ' ') args++;
    // В emergency-режиме НЕ даём запускать ELF'ы. Иначе пользователь
    // запускает, скажем, `run bin/sysgui.elf` и получает гонку: новый
    // task рисует поверх emergency-фреймбуфера, emergency-обработчик
    // продолжает поедать клавиатуру в while(shell_emergency_active)
    // hlt-цикле и перерисовывать чёрный фон поверх sysgui. Чтобы
    // вернуться к нормальному GUI — нужен `exit` (reboot) и обычная
    // загрузка sysgui из kmain.
    if (shell_emergency_active) {
        sh_print("run: disabled in emergency mode. Type 'exit' to reboot.\n");
        return;
    }
    if (!*args) {
        sh_print("run: usage: run <file.elf>\n");
        return;
    }
    /* task_exec пересоздаёт буфер сам, передавать const можно. */
    task_exec((char *)args);
}

// --- Сам реестр --------------------------------------------------------
//
// ВНИМАНИЕ: порядок имеет значение для shell_find_command — более
// длинные общие префиксы должны идти РАНЬШЕ ("killall" перед "kill",
// хотя у нас kill takes_args=true и killall takes_args=false, так что
// конфликта нет).

const shell_command_t SHELL_COMMANDS[] = {
    { "help",    false, cmd_help_fn,    "show this help" },
    { "fetch",   false, cmd_fetch_fn,   "system info / logo" },
    { "clear",   false, cmd_clear_fn,   "clear screen" },
    { "ls",      false, cmd_ls_fn,      "list VFS devices" },
    { "ps",      false, cmd_ps_fn,      "list processes" },
    { "kill",    true,  cmd_kill_fn,    "kill <pid> — terminate process" },
    { "killall", false, cmd_killall_fn, "kill every user process + drop to emergency shell" },
    { "run",     true,  cmd_run_fn,     "run <file.elf> — exec ELF from VFS" },
    { "reboot",  false, cmd_reboot_fn,  "reboot the machine (triple fault)" },
    { "gui",     false, cmd_gui_fn,     "(stub) start Equinox GUI" },
};

const int SHELL_COMMANDS_COUNT =
    (int)(sizeof(SHELL_COMMANDS) / sizeof(SHELL_COMMANDS[0]));

static void cmd_help_fn(const char *args) {
    (void)args;
    sh_print("\e[33mBuiltins:\e[0m\n");
    for (int i = 0; i < SHELL_COMMANDS_COUNT; i++) {
        char line[160];
        const shell_command_t *c = &SHELL_COMMANDS[i];
        const char *suffix = c->takes_args ? " ..." : "";
        sprintf(line, "  %s%s - %s\n", c->verb, suffix, c->help);
        sh_print(line);
    }
}

const shell_command_t *shell_find_command(const char *line) {
    for (int i = 0; i < SHELL_COMMANDS_COUNT; i++) {
        const shell_command_t *c = &SHELL_COMMANDS[i];
        size_t vl = strlen(c->verb);
        if (c->takes_args) {
            // "<verb> <args>"
            if (strncmp(line, c->verb, vl) == 0 && line[vl] == ' ')
                return c;
        } else {
            // "<verb>" точное равенство
            if (strncmp(line, c->verb, vl) == 0 && line[vl] == '\0')
                return c;
        }
    }
    return NULL;
}

// =============================================================================
//                              ИСПОЛНИТЕЛЬ
// =============================================================================
//
// shell_dispatch_line() — общий диспетчер без побочных эффектов сессии
// (без истории, без prompt'а). Возвращает true, если команда найдена.

static bool shell_dispatch_line(const char *cmd) {
    if (!cmd || cmd[0] == '\0') return true;

    const shell_command_t *c = shell_find_command(cmd);
    if (!c) {
        sh_print("\e[31mCommand not found: \e[0m");
        sh_print(cmd);
        sh_print("\n");
        return false;
    }

    const char *args = cmd + strlen(c->verb);
    if (*args == ' ') args++; /* skip разделитель */
    c->handler(args);
    return true;
}

void shell_execute_line(const char *line, shell_output_fn out) {
    if (!line) return;
    shell_output_fn prev = shell_get_output();
    if (out) shell_set_output(out);
    shell_dispatch_line(line);
    shell_set_output(prev);
}

static void shell_execute(char *cmd) {
    if (cmd[0] == '\0')
        return;

    // История.
    if (history_count < MAX_HISTORY) {
        strcpy(history[history_count++], cmd);
    } else {
        for (int i = 0; i < MAX_HISTORY - 1; i++)
            strcpy(history[i], history[i + 1]);
        strcpy(history[MAX_HISTORY - 1], cmd);
    }
    history_browse_idx = history_count;

    sh_print("\n");
    shell_dispatch_line(cmd);

    // В emergency-режиме prompt мы НЕ печатаем: у emergency-рендера
    // свой prompt в нижней строке, его рисует emergency_redraw().
    if (s_out == default_output) {
        shell_prompt();
    }
}

void shell_run_command(const char *cmd) {
    char buf[64];
    int i = 0;
    if (cmd) {
        while (cmd[i] && i < 63) {
            buf[i] = cmd[i];
            i++;
        }
    }
    buf[i] = '\0';
    shell_execute(buf);
}

// =============================================================================
//                              ВВОД
// =============================================================================

void shell_handle_char(char c) {
    if (c == '\n') {
        shell_execute(shell_buffer);
        memset(shell_buffer, 0, 64);
        shell_idx = 0;
    } else if (c == '\b') {
        if (shell_idx > 0) {
            shell_idx--;
            shell_buffer[shell_idx] = '\0';
        }
    } else if (c == '\t') {
        if (shell_idx > 0) {
            if (strstr(shell_buffer, "run ") == shell_buffer) {
                strcat(shell_buffer, ".elf");
                shell_idx = strlen(shell_buffer);
            }
        }
    } else if (c == '\x11') { // UP — история
        if (history_count > 0 && history_browse_idx > 0) {
            history_browse_idx--;
            strcpy(shell_buffer, history[history_browse_idx]);
            shell_idx = strlen(shell_buffer);
        }
    } else if (c == '\x12') { // DOWN
        if (history_browse_idx < history_count - 1) {
            history_browse_idx++;
            strcpy(shell_buffer, history[history_browse_idx]);
            shell_idx = strlen(shell_buffer);
        } else {
            history_browse_idx = history_count;
            memset(shell_buffer, 0, 64);
            shell_idx = 0;
        }
    } else if (shell_idx < 62 && c >= 32 && c <= 126) {
        shell_buffer[shell_idx++] = c;
        shell_buffer[shell_idx] = '\0';
    }
}

void shell_handle_fkey(int n) {
    // По умолчанию F-клавиши не интерпретируются — emergency-режим имеет
    // свою таблицу (см. shell_emergency_handle_fkey ниже). Здесь можно
    // повесить хоткеи обычной оболочки в будущем.
    (void)n;
}

// =============================================================================
//                EMERGENCY-РЕЖИМ (бывший eshell.c, теперь часть shell)
// =============================================================================
//
// Идея: тот же command-processor (см. shell_execute выше), но другой
// рендер. Мы:
//   1) ставим sink на emergency_sink() — он буферизует строки;
//   2) при каждом обновлении состояния пере-рисовываем чёрный экран
//      напрямую в фреймбуфер (vesa *_direct), bottom-up;
//   3) обрабатываем ввод сами (shell_emergency_handle_char), потому что
//      приоритет ввода с клавиатуры в этот режим даёт keyboard.c.

#define EM_CHAR_W       8
#define EM_CHAR_H       8
#define EM_LINE_H       10
#define EM_LEFT_PAD     8
#define EM_BOTTOM_PAD   8
#define EM_FG           0xC8C8C8
#define EM_FG_DIM       0x808080
#define EM_FG_PROMPT    0x00C864
#define EM_BG           0x000000

#define EM_MAX_LINES    256
#define EM_LINE_W       256
#define EM_INPUT_MAX    128

static char  em_lines[EM_MAX_LINES][EM_LINE_W];
static int   em_line_count = 0;
static char  em_current[EM_LINE_W];     // незакрытая (без \n) строка
static int   em_current_len = 0;

static char  em_input[EM_INPUT_MAX];
static int   em_input_len = 0;

volatile bool shell_emergency_active = false;
volatile bool shell_emergency_requested = false;

static void em_reset_state(void) {
    em_line_count = 0;
    em_current_len = 0;
    em_current[0] = '\0';
    em_input_len = 0;
    em_input[0] = '\0';
}

static void em_push_line(const char *line) {
    if (em_line_count == EM_MAX_LINES) {
        for (int i = 0; i < EM_MAX_LINES - 1; i++)
            strcpy(em_lines[i], em_lines[i + 1]);
        em_line_count = EM_MAX_LINES - 1;
    }
    int n = 0;
    while (line[n] && n < EM_LINE_W - 1) {
        em_lines[em_line_count][n] = line[n];
        n++;
    }
    em_lines[em_line_count][n] = '\0';
    em_line_count++;
}

// Буферизуем строки, выкидывая ANSI escape и нерисуемые символы.
static void em_buffer_write(const char *s) {
    if (!s) return;
    while (*s) {
        unsigned char c = (unsigned char)*s++;

        if (c == 0x1B) {                 // ESC
            if (*s == '[') {
                s++;
                while (*s && !((*s >= 'A' && *s <= 'Z') ||
                               (*s >= 'a' && *s <= 'z'))) s++;
                if (*s) s++;
            }
            continue;
        }
        if (c == '\r') {
            em_current_len = 0;
            em_current[0] = '\0';
            continue;
        }
        if (c == '\n') {
            em_current[em_current_len] = '\0';
            em_push_line(em_current);
            em_current_len = 0;
            em_current[0] = '\0';
            continue;
        }
        if (c == '\t') {
            for (int k = 0; k < 4 && em_current_len < EM_LINE_W - 1; k++)
                em_current[em_current_len++] = ' ';
            em_current[em_current_len] = '\0';
            continue;
        }
        if (c == '\b') {
            if (em_current_len > 0)
                em_current[--em_current_len] = '\0';
            continue;
        }
        if (c < 32 || c > 126) continue;

        if (em_current_len < EM_LINE_W - 1) {
            em_current[em_current_len++] = (char)c;
            em_current[em_current_len] = '\0';
        }
    }
}

static void em_redraw(void) {
    // 1. Чёрный фон во весь экран.
    draw_rect_direct(0, 0, (int)screen_width, (int)screen_height, EM_BG);

    // 2. Header сверху (как у Arch installer).
    vesa_draw_string_direct(
        "EquinoxOS emergency shell -- type 'help' for commands, 'exit' "
        "to reboot",
        EM_LEFT_PAD, 4, EM_FG_DIM);
    draw_rect_direct(0, EM_LINE_H + 4, (int)screen_width, 1, 0x202020);

    // 3. Нижняя строка — input prompt.
    int input_y = (int)screen_height - EM_LINE_H - EM_BOTTOM_PAD;
    vesa_draw_string_direct("# ", EM_LEFT_PAD, input_y, EM_FG_PROMPT);
    vesa_draw_string_direct(em_input,
                            EM_LEFT_PAD + 2 * EM_CHAR_W, input_y, EM_FG);
    int cursor_x = EM_LEFT_PAD + (2 + em_input_len) * EM_CHAR_W;
    draw_rect_direct(cursor_x, input_y, EM_CHAR_W, EM_CHAR_H, EM_FG);

    // 4. История — снизу вверх. Сначала недозакрытая em_current.
    int y = input_y - EM_LINE_H;
    if (em_current_len > 0 && y > EM_LINE_H) {
        vesa_draw_string_direct(em_current, EM_LEFT_PAD, y, EM_FG);
        y -= EM_LINE_H;
    }
    for (int i = em_line_count - 1; i >= 0 && y > EM_LINE_H + 4; i--) {
        vesa_draw_string_direct(em_lines[i], EM_LEFT_PAD, y, EM_FG);
        y -= EM_LINE_H;
    }
}

// Sink для shell_set_output: получает то, что печатают команды
// (sh_print()), буферизует и перерисовывает экран.
static void emergency_sink(const char *s) {
    em_buffer_write(s);
    em_redraw();
}

static void em_banner(void) {
    em_buffer_write(
        "Emergency shell.\n"
        "GUI subsystem stopped. Direct framebuffer mode.\n"
        "Commands: help ps kill killall fetch ls reboot exit\n"
        "\n");
}

void shell_emergency_handle_char(char c) {
    if (!shell_emergency_active) return;

    if (c == '\n') {
        // 1. Эхо команды в историю.
        char echo[EM_INPUT_MAX + 8];
        int j = 0;
        echo[j++] = '#';
        echo[j++] = ' ';
        for (int i = 0; i < em_input_len && j < (int)sizeof(echo) - 2; i++)
            echo[j++] = em_input[i];
        echo[j++] = '\n';
        echo[j] = '\0';
        em_buffer_write(echo);

        // 2. Спецкоманда `exit` — выходим через triple fault.
        if (strcmp(em_input, "exit") == 0) {
            em_buffer_write("Rebooting...\n");
            em_redraw();
            struct { uint16_t l; uint64_t b; }
                __attribute__((packed)) idt = {0, 0};
            __asm__ volatile("lidt %0; int3" : : "m"(idt));
            for (;;) __asm__("hlt");
        }

        // 3. Иначе — обычный исполнитель оболочки. Вывод придёт
        // обратно через emergency_sink() и сам перерисует экран.
        shell_run_command(em_input);
        em_input_len = 0;
        em_input[0] = '\0';
        em_redraw();
        return;
    }
    if (c == '\b') {
        if (em_input_len > 0) {
            em_input[--em_input_len] = '\0';
            em_redraw();
        }
        return;
    }
    if (c >= 32 && c <= 126 && em_input_len < EM_INPUT_MAX - 1) {
        em_input[em_input_len++] = c;
        em_input[em_input_len] = '\0';
        em_redraw();
    }
}

void shell_emergency_handle_fkey(int n) {
    switch (n) {
    case 1:  shell_run_command("help"); em_redraw(); break;
    case 2:  shell_run_command("ps");   em_redraw(); break;
    case 12: // F12 — экстренный reboot, даже если ввод текста сломан
        shell_emergency_handle_char('e');
        shell_emergency_handle_char('x');
        shell_emergency_handle_char('i');
        shell_emergency_handle_char('t');
        shell_emergency_handle_char('\n');
        break;
    default: break;
    }
}

void shell_emergency_enter(void) {
    // Запоминаем прошлый sink и подменяем своим. Уважительно: после
    // выхода (если когда-нибудь произойдёт) восстановим.
    shell_output_fn prev = shell_get_output();
    shell_set_output(emergency_sink);

    em_reset_state();
    em_banner();

    shell_emergency_active = true;
    shell_emergency_requested = false;
    em_redraw();

    // Дальше ввод идёт через keyboard_callback → shell_emergency_handle_char.
    // Здесь мы просто hlt'имся, давая прерываниям делать работу. Выход
    // обычно — `exit` (см. выше, делает reboot и сюда не возвращается).
    while (shell_emergency_active) {
        __asm__ volatile("hlt");
    }
    shell_set_output(prev);
}
