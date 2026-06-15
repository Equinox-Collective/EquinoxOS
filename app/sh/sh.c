/* =============================================================================
 * EquinoxOS — bin/sh.elf — Этап 10: системный шелл
 * =============================================================================
 *
 * Что это и зачем
 * ---------------
 * До этого этапа единственным «шеллом» был ring-0 процессор команд в
 * src/system/shell/shell.c. Он работал внутри ядра: и emergency-режим, и
 * GUI-терминал, и autoexec ходили в него напрямую. Это удобно для bring-up,
 * но архитектурно неправильно: шелл — это пользовательский процесс.
 *
 * Поэтому теперь есть `bin/sh.elf` — НАСТОЯЩИЙ системный шелл EquinoxOS,
 * ring-3 ELF поверх vendored musl. Его задача:
 *
 *   1) Напечатать опознавательный баннер EquinoxOS + /etc/motd (если есть).
 *   2) Выставить «нормальное» окружение (PATH, HOME, SHELL, TERM, USER, PS1).
 *   3) execve("/bin/bash.elf", ...) — bash остаётся «двигателем», sh.elf
 *      даёт ему лицо и контекст. Если когда-нибудь захотим выпилить bash и
 *      писать собственный парсер — заменяем потроха sh.elf, интерфейс «sh»
 *      снаружи не меняется.
 *
 * Системный компонент
 * -------------------
 * Ядро на старте (kmain) проверяет наличие /bin/sh.elf. Нет файла — большой
 * предупреждающий лог в COM1 и боевой emergency-режим. То есть сборка ОС без
 * sh.elf — это уже сломанная сборка, как Linux без /sbin/init.
 *
 * Запуск
 * ------
 *   - из kernel shell:        `sh`
 *   - из emergency shell:     `sh` (тот же диспетчер)
 *   - из autoexec/COM1 теста: пишем `sh` строкой в iso_root/autoexec
 *
 * Падения bash
 * ------------
 * Этап 10 поправил panic_handler: kernel-Page-Fault на user-VA из ring-0
 * больше не валит ОС — он убивает виновный процесс с SIGSEGV. То есть даже
 * если bash где-то даёт битый указатель в сисколл, валится только bash,
 * sh.elf (а точнее его execve-замена) возвращает ошибку, и пользователь
 * получит prompt назад в emergency без перезагрузки.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

extern char **environ;

/* Аккуратный print без printf, чтобы баннер красиво лёг даже если stdio musl'а
 * почему-то не флашится перед execve (а оно ДОЛЖНО, write(1) сразу же
 * уходит в ядро). Используем write() напрямую — никакой буферизации. */
static void out(const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n) write(1, s, n);
}

/* Печатаем содержимое файла «как есть», игнорируем отсутствие — это features,
 * /etc/motd может не быть на минимальном образе. */
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

/* Логотип + версия. Цвета 8-bit ANSI — bash их прокатит как есть. */
static void print_banner(void) {
    out(
        "\n"
        "\x1b[36m       eeeeeeee        \x1b[0m\n"
        "\x1b[36m     eee      eee      \x1b[0m\n"
        "\x1b[36m    eee      eee       \x1b[37mEquinoxOS\x1b[0m  /  \x1b[32msh 1.0\x1b[0m\n"
        "\x1b[36m   eeeeeeeeeee         \x1b[37mShell:\x1b[0m   /bin/sh.elf -> /bin/bash.elf 5.2.37\n"
        "\x1b[36m   eee                 \x1b[37mLibc:\x1b[0m    musl 1.2.5  (ring 3 / int 0x81)\n"
        "\x1b[36m    eee      eee       \x1b[37mTerm:\x1b[0m    equos (COM1 line discipline)\n"
        "\x1b[36m     eeeeeeeeee        \x1b[0m\n"
        "\n"
        "Type 'help' for builtins, or any external command. Ctrl-D / 'exit' to leave.\n"
        "\n"
    );
}

/* Самостоятельный аварийный mini-REPL: если bash отсутствует или execve()
 * вернул ошибку — пользователь не остаётся с чёрным экраном, мы отдаём
 * хотя бы echo-цикл и подсказку, что bash не подгрузился. Дёргаем тот же
 * SYS_SHELL_EXEC, что и emergency shell, через _syscall.
 *
 * Этот цикл — не «параллельный шелл», а fallback. Нормальный путь — execve. */
static void emergency_repl(void) {
    out("\x1b[31m[sh.elf]\x1b[0m bash unavailable, falling back to kernel rescue shell.\n");
    out("        Type 'reboot' to restart. Anything else echoes back.\n");
    char line[256];
    for (;;) {
        out("equos-rescue# ");
        long n = read(0, line, sizeof(line) - 1);
        if (n <= 0) { out("\n"); break; }
        line[n] = '\0';
        /* срезаем хвостовой \n */
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (line[0] == '\0') continue;
        if (!strcmp(line, "exit") || !strcmp(line, "reboot")) {
            out("rebooting...\n");
            _exit(0);
        }
        out(line);
        out("\n");
    }
    _exit(0);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    print_banner();
    cat_if_exists("/etc/motd");

    /* Окружение «как в нормальной системе». Если предыдущий запуск уже что-то
     * наставил (sh.elf -> bash -> sh.elf по cascade) — не ломаем. */
    setenv("SHELL", "/bin/sh.elf",                         0);
    setenv("PATH",  "/bin:/usr/bin",                       0);
    setenv("HOME",  "/",                                   0);
    setenv("USER",  "root",                                0);
    setenv("LOGNAME", "root",                              0);
    setenv("TERM",  "equos",                               0);
    setenv("PS1",
           "\\[\\e[32m\\]\\u@equos\\[\\e[0m\\]:"
           "\\[\\e[34m\\]\\w\\[\\e[0m\\]\\$ ",
           0);
    setenv("PWD",   "/",                                   0);

    /* Передаём bash минимально-достаточный набор аргументов. argv[0]="bash"
     * (не "/bin/bash.elf") — bash смотрит basename для определения личности
     * (sh vs bash vs rbash). --rcfile фиксирует .bashrc на корне, потому что
     * HOME=/ может быть переопределён юзером. -i помечает сессию как
     * interactive, чтобы bash включил readline/job control. */
    char *bargv[] = {
        (char *)"bash",
        (char *)"--rcfile", (char *)"/.bashrc",
        (char *)"-i",
        NULL
    };

    /* VFS у нас плоский — bash.elf может лежать как "bin/bash.elf" так и
     * "/bin/bash.elf" (resolver обрезает leading slash, но не все ядерные
     * пути в одинаковой форме принимают). Пробуем оба, потом сдаёмся. */
    execve("/bin/bash.elf", bargv, environ);
    execve("bin/bash.elf",  bargv, environ);
    execve("bash.elf",      bargv, environ);  /* срабатывает bin/-fallback в task_load_image */

    out("\x1b[31m[sh.elf]\x1b[0m bash.elf not loadable (tried /bin, bin/, bare). "
        "Check `ls bin/` in kernel shell and `make apps` output for bash.elf link errors.\n");
    emergency_repl();
    return 1;
}
