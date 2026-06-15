/* =============================================================================
 * EquinoxOS — bin/sh.elf — Этап 11: системный шелл (REPL поверх SYS_SHELL_EXEC)
 * =============================================================================
 *
 * Что это и зачем
 * ---------------
 * Раньше sh.elf был тонкой обёрткой над bash.elf: банер + execve("/bin/bash.elf").
 * Этап 11 переворачивает архитектуру: теперь sh.elf — НАСТОЯЩИЙ интерактивный
 * шелл EquinoxOS со своим REPL, а bash превращается в обычную внешнюю команду,
 * которую можно дёрнуть набрав `bash`.
 *
 * Зачем перенесли REPL в sh.elf, а не оставили в bash?
 *   1) bash тянет за собой /etc/passwd lookups, terminfo, readline, job control —
 *      каждый запуск даёт по 5 fault-ов в ядро и ломается на любой мелочи.
 *   2) Мы хотим, чтобы все наши кастомные команды (см. src/system/shell/shell.c —
 *      `ls`, `cat`, `ps`, `meminfo`, и т.д.) работали из коробки. Самый дешёвый
 *      способ — гонять каждую введённую строку через SYS_SHELL_EXEC (case 73),
 *      который умеет диспатчить весь зоопарк ring-0 шелл-команд.
 *   3) enGUI terminal.lua уже использует SYS_SHELL_EXEC. Теперь точно та же
 *      семантика будет и в физическом терминале, и в GUI — единственный
 *      источник правды для команд.
 *
 * Архитектура REPL
 * ----------------
 * Каждый ввод проходит конвейер:
 *
 *   1) Локальные builtin'ы sh.elf (cd, pwd, exit, export, env, help, bash) —
 *      выполняются прямо здесь, не лезут в ядро. Это вещи, требующие user-side
 *      состояния (текущая директория, environment).
 *   2) Иначе — отдаём строку в SYS_SHELL_EXEC. Ядерный диспетчер либо отрабатывает
 *      команду и возвращает её stdout в буфер, либо печатает "Command not found:".
 *   3) Если "Command not found:" — пробуем execve в /bin/<команда>.elf. Это путь
 *      для будущих юзерских утилит (например /bin/bash.elf, /bin/musltest.elf).
 *   4) Иначе — печатаем выхлоп ядерного шелла как есть.
 *
 * Запуск
 * ------
 * Ядро на старте (kmain) проверяет наличие /bin/sh.elf. Нет файла — большой
 * предупреждающий лог в COM1 и боевой emergency-режим, т.е. сборка без sh.elf
 * считается сломанной (как Linux без /sbin/init). Запустить руками можно из
 * kernel shell или autoexec — командой `sh`.
 *
 * Падения дочерних процессов
 * --------------------------
 * Этап 10 поправил panic_handler: kernel-Page-Fault на user-VA из ring-0
 * больше не валит ОС, он убивает виновный процесс с SIGSEGV. То есть если
 * пользователь дёрнет `bash` и тот упадёт где-то в своём execve("ls"), валится
 * только bash, sh.elf продолжает REPL.
 *
 * Известные ограничения текущего REPL
 * -----------------------------------
 *   - Нет линий-редактора (Backspace/стрелки/история). Сырое read(0). Пока
 *     достаточно для прокачки логики; readline вкорячим позже.
 *   - cd не вызывает ядро (нет chdir-сисколла в EquinoxOS) — мы держим CWD
 *     внутри sh.elf и подставляем его в PS1 и в SYS_SHELL_EXEC через PWD env.
 *   - SYS_SHELL_EXEC принимает строки до 256 байт (см. syscall.c). Длиннее —
 *     ядро сделает усечение, что соответствует ожиданию.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <stdint.h>

extern char **environ;

/* ----- константы ---------------------------------------------------------- */

#define SH_LINE_MAX    256
#define SH_OUTBUF_MAX  8192
#define SH_CWD_MAX     128

/* SYS_SHELL_EXEC: musl int 0x81 идёт через Linux-шлюз (linux_syscall_handler).
 * Linux 73 = fsync, занят. EquinoxOS-extension: 1000 → native SYS_SHELL_EXEC (73).
 * См. syscall.c: `case 1000: regs->rax = 73; break;` */
#define SYS_SHELL_EXEC 1000

/* ----- состояние ---------------------------------------------------------- */

static char sh_cwd[SH_CWD_MAX] = "/";

/* ----- мелкий I/O без printf-форматтера ----------------------------------- */

static void out(const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n) write(1, s, n);
}

static void outc(char c) { write(1, &c, 1); }

static void cat_if_exists(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    char buf[512];
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        write(1, buf, (size_t)n);
    }
    close(fd);
}

/* ----- inline SYS_SHELL_EXEC --------------------------------------------- */

/* Возвращает число записанных байт в outbuf (без NUL). Может быть 0 если
 * ядерный шелл ничего не напечатал (например, успешная команда без вывода). */
static long sys_shell_exec(const char *line, char *outbuf, unsigned long cap) {
    long ret;
    __asm__ volatile("int $0x81"
        : "=a"(ret)
        : "a"((long)SYS_SHELL_EXEC), "D"(line), "S"(outbuf), "d"(cap)
        : "rcx", "r11", "memory");
    return ret;
}

/* ----- banner ------------------------------------------------------------- */

static void print_banner(void) {
    out(
        "\n"
        "\x1b[36m       eeeeeeee        \x1b[0m\n"
        "\x1b[36m     eee      eee      \x1b[0m\n"
        "\x1b[36m    eee      eee       \x1b[37mEquinoxOS\x1b[0m  /  \x1b[32msh 2.0\x1b[0m\n"
        "\x1b[36m   eeeeeeeeeee         \x1b[37mShell:\x1b[0m   /bin/sh.elf (REPL + SYS_SHELL_EXEC)\n"
        "\x1b[36m   eee                 \x1b[37mLibc:\x1b[0m    musl 1.2.5  (ring 3 / int 0x81)\n"
        "\x1b[36m    eee      eee       \x1b[37mTerm:\x1b[0m    equos (COM1 line discipline)\n"
        "\x1b[36m     eeeeeeeeee        \x1b[0m\n"
        "\n"
        "Type 'help' for builtins, 'bash' for GNU bash 5.2.37, Ctrl-D / 'exit' to leave.\n"
        "\n"
    );
}

/* ----- prompt ------------------------------------------------------------- */

static void prompt(void) {
    /* root@equos:CWD#  — короткий, цветной, всегда читабельный. */
    out("\x1b[32mroot@equos\x1b[0m:\x1b[34m");
    out(sh_cwd);
    out("\x1b[0m# ");
}

/* ----- утилиты парсинга --------------------------------------------------- */

static void trim_trailing(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t'))
        s[--n] = '\0';
}

static void trim_leading(char **ps) {
    while (**ps == ' ' || **ps == '\t') (*ps)++;
}

/* Имя команды до первого пробела. Возвращает указатель в той же строке. */
static void split_verb(const char *line, char *verb, size_t cap) {
    size_t i = 0;
    while (line[i] && line[i] != ' ' && line[i] != '\t' && i + 1 < cap) {
        verb[i] = line[i];
        i++;
    }
    verb[i] = '\0';
}

/* ----- builtin: help ------------------------------------------------------ */

static void builtin_help(void) {
    out(
        "sh.elf builtins:\n"
        "  help               — эта подсказка\n"
        "  exit, logout       — завершить шелл\n"
        "  cd <path>          — сменить CWD внутри sh.elf\n"
        "  pwd                — напечатать CWD\n"
        "  env                — текущее окружение\n"
        "  export NAME=VALUE  — выставить переменную окружения\n"
        "  bash [args]        — запустить GNU bash 5.2.37\n"
        "\n"
        "Любая другая команда сначала идёт в ядерный шелл (SYS_SHELL_EXEC),\n"
        "иначе — execve(/bin/<команда>.elf). Команды ядра: попробуй `ls`, `ps`,\n"
        "`meminfo`, `version`, `clear`, `cat <file>` и т.д.\n"
    );
}

/* ----- builtin: cd / pwd -------------------------------------------------- */

/* Простой normaliser путей: поддерживает абсолютные, относительные, ".." и ".". */
static void path_join(char *dst, size_t cap, const char *cwd, const char *rel) {
    if (rel[0] == '/') {
        strncpy(dst, rel, cap - 1);
        dst[cap - 1] = '\0';
        return;
    }
    snprintf(dst, cap, "%s%s%s",
             cwd,
             (cwd[strlen(cwd) - 1] == '/') ? "" : "/",
             rel);
}

static void path_normalize(char *p) {
    /* Сжимаем // -> /, выкусываем /./ , обрабатываем /../ */
    char tmp[SH_CWD_MAX * 2];
    size_t out_pos = 0;
    size_t i = 0;
    if (p[0] != '/') { tmp[out_pos++] = '/'; }
    while (p[i] && out_pos + 1 < sizeof(tmp)) {
        if (p[i] == '/' && p[i + 1] == '/') { i++; continue; }
        if (p[i] == '/' && p[i + 1] == '.' &&
            (p[i + 2] == '/' || p[i + 2] == '\0')) { i += 2; continue; }
        if (p[i] == '/' && p[i + 1] == '.' && p[i + 2] == '.' &&
            (p[i + 3] == '/' || p[i + 3] == '\0')) {
            /* откатываемся на один компонент назад */
            while (out_pos > 1 && tmp[out_pos - 1] != '/') out_pos--;
            if (out_pos > 1) out_pos--; /* съесть сам / */
            i += 3;
            continue;
        }
        tmp[out_pos++] = p[i++];
    }
    if (out_pos == 0) tmp[out_pos++] = '/';
    if (out_pos > 1 && tmp[out_pos - 1] == '/') out_pos--;
    tmp[out_pos] = '\0';
    strncpy(p, tmp, SH_CWD_MAX - 1);
    p[SH_CWD_MAX - 1] = '\0';
}

static void builtin_cd(const char *args) {
    if (!args || !*args) { strcpy(sh_cwd, "/"); setenv("PWD", sh_cwd, 1); return; }
    char joined[SH_CWD_MAX * 2];
    path_join(joined, sizeof(joined), sh_cwd, args);
    path_normalize(joined);
    strncpy(sh_cwd, joined, sizeof(sh_cwd) - 1);
    sh_cwd[sizeof(sh_cwd) - 1] = '\0';
    setenv("PWD", sh_cwd, 1);
}

static void builtin_pwd(void) { out(sh_cwd); outc('\n'); }

static void builtin_env(void) {
    if (!environ) return;
    for (char **e = environ; *e; e++) {
        out(*e); outc('\n');
    }
}

static void builtin_export(const char *args) {
    /* export NAME=VALUE — без NAME= ничего не делаем. */
    if (!args || !*args) return;
    const char *eq = strchr(args, '=');
    if (!eq) {
        out("export: expected NAME=VALUE\n");
        return;
    }
    char name[64];
    size_t nl = (size_t)(eq - args);
    if (nl >= sizeof(name)) nl = sizeof(name) - 1;
    memcpy(name, args, nl);
    name[nl] = '\0';
    setenv(name, eq + 1, 1);
}

/* ----- bash launcher ------------------------------------------------------ */

/* Форкаем bash через fork+execve. fork в EquinoxOS-musl реализован поверх
 * task_fork; если его нет — рассматриваем как фатально неисправимый bash. */
static void launch_bash(const char *rest) {
    (void)rest; /* пока без аргументов; будущий парсер раскрошит rest на argv */
    pid_t pid = fork();
    if (pid < 0) {
        out("\x1b[31m[sh.elf]\x1b[0m fork() failed; bash skipped.\n");
        return;
    }
    if (pid == 0) {
        char *bargv[] = {
            (char *)"bash",
            (char *)"--rcfile", (char *)"/.bashrc",
            (char *)"-i",
            NULL
        };
        execve("/bin/bash.elf", bargv, environ);
        execve("bin/bash.elf",  bargv, environ);
        execve("bash.elf",      bargv, environ);
        out("\x1b[31m[sh.elf]\x1b[0m bash.elf not loadable.\n");
        _exit(127);
    }
    /* Родитель ждёт; примитивный wait через busy-loop, потому что waitpid у
     * нас может ещё не быть. Если есть — отлично, musl сама дернёт. */
    int status = 0;
    (void)status;
    waitpid(pid, &status, 0);
}

/* ----- execve fallback для unknown команд -------------------------------- */

/* Если ядерный шелл вернул "Command not found:" — пробуем найти /bin/<verb>.elf
 * и запустить через fork+execve. Возвращает 0 если запустили (родитель дождался),
 * -1 если не нашли. */
static int try_exec_binary(const char *verb, const char *full_line) {
    char path[160];
    snprintf(path, sizeof(path), "/bin/%s.elf", verb);
    /* быстрый stat — если файла нет, не дёргаем fork. */
    struct stat st;
    if (stat(path, &st) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        out("\x1b[31m[sh.elf]\x1b[0m fork() failed.\n");
        return -1;
    }
    if (pid == 0) {
        /* TODO(этап 12): нормальный argv-разбор. Пока проксируем как одна\n         * строка после verb. */
        char rest[SH_LINE_MAX];
        const char *r = full_line + strlen(verb);
        while (*r == ' ') r++;
        strncpy(rest, r, sizeof(rest) - 1);
        rest[sizeof(rest) - 1] = '\0';
        char *bargv[] = { (char *)verb, rest[0] ? rest : NULL, NULL };
        execve(path, bargv, environ);
        _exit(127);
    }
    int status = 0;
    (void)status;
    waitpid(pid, &status, 0);
    return 0;
}

/* ----- маркер "не найдено" в выхлопе ядерного шелла ---------------------- */

static int output_says_not_found(const char *buf) {
    /* shell_dispatch_line печатает "\x1b[31mCommand not found: \x1b[0m<cmd>\n".
     * Достаточно искать подстроку без ANSI — устойчиво к любым изменениям цвета. */
    return strstr(buf, "Command not found:") != NULL;
}

/* ----- main REPL --------------------------------------------------------- */

static int read_line(char *buf, size_t cap) {
    /* Тупой read до \n. read(0) в EquinoxOS line-discipline отдаёт всю строку
     * целиком, поэтому одного вызова достаточно. */
    long n = read(0, buf, cap - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    print_banner();
    cat_if_exists("/etc/motd");

    /* Окружение «как в нормальной системе». */
    setenv("SHELL",   "/bin/sh.elf",      0);
    setenv("PATH",    "/bin:/usr/bin",    0);
    setenv("HOME",    "/",                0);
    setenv("USER",    "root",             0);
    setenv("LOGNAME", "root",             0);
    setenv("TERM",    "equos",            0);
    setenv("PWD",     sh_cwd,             1);

    char line[SH_LINE_MAX];
    char outbuf[SH_OUTBUF_MAX];

    for (;;) {
        prompt();
        int n = read_line(line, sizeof(line));
        if (n < 0) { out("\n"); break; }

        trim_trailing(line);
        char *p = line;
        trim_leading(&p);
        if (*p == '\0') continue;

        char verb[64];
        split_verb(p, verb, sizeof(verb));

        /* --- локальные builtin'ы --- */
        if (!strcmp(verb, "exit") || !strcmp(verb, "logout") ||
            !strcmp(verb, "quit"))                                   { break; }
        if (!strcmp(verb, "help"))                                   { builtin_help(); continue; }
        if (!strcmp(verb, "cd")) {
            const char *args = p + 2;
            while (*args == ' ') args++;
            builtin_cd(args);
            continue;
        }
        if (!strcmp(verb, "pwd"))                                    { builtin_pwd(); continue; }
        if (!strcmp(verb, "env"))                                    { builtin_env(); continue; }
        if (!strcmp(verb, "export")) {
            const char *args = p + 6;
            while (*args == ' ') args++;
            builtin_export(args);
            continue;
        }
        if (!strcmp(verb, "bash")) {
            const char *args = p + 4;
            while (*args == ' ') args++;
            launch_bash(args);
            continue;
        }

        /* --- ядерный диспетчер --- */
        outbuf[0] = '\0';
        long rc = sys_shell_exec(p, outbuf, sizeof(outbuf));
        (void)rc;

        if (output_says_not_found(outbuf)) {
            /* ядерный шелл не знает команду — пробуем бинарь */
            if (try_exec_binary(verb, p) == 0) continue;
            /* и не бинарь — печатаем ядерный ответ как есть */
            out(outbuf);
        } else {
            out(outbuf);
        }
    }

    out("logout\n");
    return 0;
}
