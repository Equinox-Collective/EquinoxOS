/* =============================================================================
 * EquinoxOS — bin/sh.elf — REPL + GUI pipe I/O (English Debug Edition)
 * =============================================================================
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
#include <stdbool.h>

// Include native EquinoxOS system calls
#include <equos.h>

extern char **environ;

#define SH_LINE_MAX    256
#define SH_OUTBUF_MAX  8192
#define SH_CWD_MAX     128

#define SYS_SHELL_EXEC 1000

static char sh_cwd[SH_CWD_MAX] = "/";

/* FD configuration and mode */
static int sh_in_fd  = 0;
static int sh_out_fd = 1;
static bool sh_gui_mode = false;

static void out(const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n) write(sh_out_fd, s, n);
}

static void outc(char c) { write(sh_out_fd, &c, 1); }

static void cat_if_exists(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    char buf[512];
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        write(sh_out_fd, buf, (size_t)n);
    }
    close(fd);
}

static long sys_shell_exec(const char *line, char *outbuf, unsigned long cap) {
    long ret;
    __asm__ volatile("int $0x81"
        : "=a"(ret)
        : "a"((long)SYS_SHELL_EXEC), "D"(line), "S"(outbuf), "d"(cap)
        : "rcx", "r11", "memory");
    return ret;
}

static void print_banner(void) {
    if (sh_gui_mode) {
        out(
            "\n"
            "\x1b[36m       eeeeeeee        \x1b[0m\n"
            "\x1b[36m     eee      eee      \x1b[0m\n"
            "\x1b[36m    eee      eee       \x1b[37mEquinoxOS\x1b[0m  /  \x1b[32msh 2.0\x1b[0m\n"
            "\x1b[36m   eeeeeeeeeee         \x1b[37mShell:\x1b[0m   /bin/sh.elf\n"
            "\x1b[36m   eee                 \x1b[37mTerm:\x1b[0m    sysgui (LVGL pipe)\n"
            "\x1b[36m    eee      eee       \x1b[0m\n"
            "\x1b[36m     eeeeeeeeee        \x1b[0m\n"
            "\n"
            "Type 'help' for builtins, 'exit' to close.\n\n"
        );
        return;
    }
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

static void prompt(void) {
    out("\x1b[32mroot@equos\x1b[0m:\x1b[34m");
    out(sh_cwd);
    out("\x1b[0m# ");
}

static void trim_trailing(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t'))
        s[--n] = '\0';
}

static void trim_leading(char **ps) {
    while (**ps == ' ' || **ps == '\t') (*ps)++;
}

static void split_verb(const char *line, char *verb, size_t cap) {
    size_t i = 0;
    while (line[i] && line[i] != ' ' && line[i] != '\t' && i + 1 < cap) {
        verb[i] = line[i];
        i++;
    }
    verb[i] = '\0';
}

static void builtin_help(void) {
    out(
        "sh.elf builtins:\n"
        "  help               - this help message\n"
        "  exit, logout       - terminate shell\n"
        "  cd <path>          - change CWD inside sh.elf\n"
        "  pwd                - print CWD\n"
        "  env                - current environment\n"
        "  export NAME=VALUE  - set environment variable\n"
        "  bash [args]        - launch GNU bash 5.2.37\n"
        "\n"
        "Any other command will be processed by the kernel shell (SYS_SHELL_EXEC),\n"
        "otherwise - execve(/bin/<command>.elf). Kernel commands: `ls`, `ps`,\n"
        "  `meminfo`, `version`, `clear`, `cat <file>`, etc.\n"
    );
}

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
            while (out_pos > 1 && tmp[out_pos - 1] != '/') out_pos--;
            if (out_pos > 1) out_pos--;
            i += 3;
            continue;
        }
        tmp[out_pos++] = p[i++];
    }
    if (out_pos == 0) tmp[out_pos++] = '/';
    if (out_pos > 1 && tmp[out_pos - 1] == '/') out_pos--;
    tmp[out_pos] = '\0';
}

static void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gui")) {
            sh_gui_mode = true;
        } else if (!strncmp(argv[i], "--in-fd=", 8)) {
            sh_in_fd = atoi(argv[i] + 8);
        } else if (!strncmp(argv[i], "--out-fd=", 9)) {
            sh_out_fd = atoi(argv[i] + 9);
        }
    }
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

static void launch_bash(const char *rest) {
    (void)rest;
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
        // Use native sys_execve for stability
        sys_execve("/bin/bash.elf", bargv, environ);
        sys_execve("bin/bash.elf",  bargv, environ);
        sys_execve("bash.elf",      bargv, environ);
        out("\x1b[31m[sh.elf]\x1b[0m bash.elf not loadable.\n");
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
}

static int try_exec_binary(const char *verb, const char *full_line) {
    char path[160];
    snprintf(path, sizeof(path), "/bin/%s.elf", verb);
    struct stat st;
    if (stat(path, &st) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        out("\x1b[31m[sh.elf]\x1b[0m fork() failed.\n");
        return -1;
    }
    if (pid == 0) {
        char rest[SH_LINE_MAX];
        const char *r = full_line + strlen(verb);
        while (*r == ' ') r++;
        strncpy(rest, r, sizeof(rest) - 1);
        rest[sizeof(rest) - 1] = '\0';
        char *bargv[] = { (char *)verb, rest[0] ? rest : NULL, NULL };
        
        // Use native sys_execve
        sys_execve(path, bargv, environ);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return 0;
}

static int output_says_not_found(const char *buf) {
    return strstr(buf, "Command not found:") != NULL;
}

/* Reads a line up to \n. Works both on COM1 and GUI pipe. */
static int read_line(char *buf, size_t cap) {
    size_t pos = 0;
    while (pos + 1 < cap) {
        char c;
        long n = read(sh_in_fd, &c, 1);
        if (n < 0) return (pos == 0) ? -1 : (int)pos;
        if (n == 0) {
            if (pos == 0) return -1;
            buf[pos] = '\0';
            return (int)pos;
        }
        if (c == '\r') continue;

        // Обработка Backspace (ASCII 8 = \b, ASCII 127 = DEL)
        if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                // Стандартный трюк: стираем символ на экране терминала
                write(sh_out_fd, "\b \b", 3);
            }
            continue;
        }

        if (c == '\n') {
            write(sh_out_fd, &c, 1); // Эхо перевода строки
            buf[pos] = '\0';
            return (int)pos;
        }

        // Эхо обычного символа обратно в GUI терминал
        write(sh_out_fd, &c, 1);
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return (int)pos;
}

static void sh_repl(void) {
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

        outbuf[0] = '\0';
        long rc = sys_shell_exec(p, outbuf, sizeof(outbuf));
        (void)rc;

        if (output_says_not_found(outbuf)) {
            if (try_exec_binary(verb, p) == 0) continue;
            out(outbuf);
        } else {
            out(outbuf);
        }
    }

    out("logout\n");
}

int main(int argc, char **argv) {
    parse_args(argc, argv);

    print_banner();
    if (!sh_gui_mode)
        cat_if_exists("/etc/motd");

    setenv("SHELL",   "/bin/sh.elf",      0);
    setenv("PATH",    "/bin:/usr/bin",    0);
    setenv("HOME",    "/",                0);
    setenv("USER",    "root",             0);
    setenv("LOGNAME", "root",             0);
    setenv("TERM",    sh_gui_mode ? "equos-lvgl" : "equos", 1);
    setenv("PWD",     sh_cwd,             1);
    if (sh_gui_mode)
        setenv("EQUINOS_GUI", "1", 1);

    sh_repl();
    return 0;
}