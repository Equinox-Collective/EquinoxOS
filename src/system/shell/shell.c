#include "shell.h"
#include "../drivers/devices/pcspeaker/pcspeaker.h"
#include "../drivers/vesa/bmp.h"
#include "../fs/vfs.h"
#include "../../gui/gui.h"
#include "../../gui/terminal.h"
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
// emergency shell, init.lua, утилита или приложение могут перенаправить
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
// задачи и запрашивает emergency shell на чёрном экране.
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
    terminal_clear();
    print_logo();
    shell_prompt();
}

// =============================================================================
//                              КОМАНДЫ
// =============================================================================

static void cmd_help(void) {
    sh_print(
        "\e[33mBuiltins:\e[0m\n"
        "  help               - show this help\n"
        "  fetch              - system info / logo\n"
        "  clear              - clear screen\n"
        "  ls                 - list VFS devices\n"
        "  run <file.elf>     - exec ELF from VFS\n"
        "  ps                 - list processes\n"
        "  kill <pid>         - terminate process by PID\n"
        "  killall            - kill every user process + drop to "
        "emergency shell\n"
        "  reboot             - reboot the machine (triple fault)\n");
}

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
        // task_terminate_by_pid сам пишет через term_print, но если sink
        // был перенастроен — пользователь всё равно увидит исход здесь.
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
//                              ИСПОЛНИТЕЛЬ
// =============================================================================

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

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls();
    } else if (strcmp(cmd, "fetch") == 0) {
        cmd_fetch();
    } else if (strcmp(cmd, "clear") == 0) {
        // Если sink — emergency shell, у него своя реализация clear через
        // пустой ввод. Здесь чистим именно GUI-терминал, только если он
        // и есть наш текущий sink.
        if (s_out == default_output) {
            terminal_clear();
        } else {
            // Просто пара пустых строк — нормальный poor man's clear.
            sh_print("\n\n");
        }
    } else if (strcmp(cmd, "gui") == 0) {
        sh_print("Starting Equinox GUI...\n");
    } else if (strcmp(cmd, "ps") == 0) {
        cmd_ps();
    } else if (memcmp(cmd, "kill ", 5) == 0) {
        cmd_kill(cmd + 5);
    } else if (strcmp(cmd, "killall") == 0) {
        sh_print("killall: terminating user processes...\n");
        emergency_kill_all_and_shell();
    } else if (strcmp(cmd, "reboot") == 0) {
        cmd_reboot();
    } else if (memcmp(cmd, "run ", 4) == 0) {
        task_exec(cmd + 4);
    } else {
        sh_print("\e[31mCommand not found: \e[0m");
        sh_print(cmd);
        sh_print("\n");
    }

    shell_prompt();
}

void shell_run_command(const char *cmd) {
    // Копируем во временный буфер: shell_execute мутирует/использует strtok
    // на словах позже, plus вызывающий может передать literal-строку.
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
    // По умолчанию F-клавиши не интерпретируются (ловить их умеют
    // приложения/eshell). Здесь можно повесить хоткеи оболочки в будущем.
    (void)n;
}
