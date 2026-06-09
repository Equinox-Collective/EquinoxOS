/*
 * pipetest — приёмочный тест Этапа 2 (процессная fd-таблица + pipe/dup2).
 *
 * Тест 1 — pipe() + fork() + наследование fd:
 *   родитель пишет строку в пайп, ребёнок читает её до EOF.
 *
 * Тест 2 — перенаправление stdout через dup2 (основа `cmd | cmd` и `> file`):
 *   ребёнок делает dup2(write_end, 1) и печатает обычным printf();
 *   родитель читает захваченный вывод из read-конца пайпа.
 *
 * Ожидаемый вывод (порядок строк родителя/ребёнка может слегка отличаться,
 * но содержимое — такое):
 *   [test1] parent: pipe + fork
 *   [test1] child got: "Hello through the pipe!"
 *   [test1] child exit 0
 *   [test2] parent: dup2 stdout -> pipe
 *   [test2] captured child stdout: "printf went into the pipe!"
 *   [test2] child exit 0
 *   [pipetest] all done
 */
#include <equos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int read_all(int fd, char *buf, int cap) {
    int total = 0;
    while (total < cap - 1) {
        int n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0) break;          /* 0 = EOF, <0 = ошибка */
        total += n;
    }
    buf[total] = '\0';
    return total;
}

static int test1(void) {
    printf("[test1] parent: pipe + fork\n");

    int fds[2];
    if (pipe(fds) != 0) { printf("[test1] pipe() FAILED\n"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { printf("[test1] fork() FAILED\n"); return 1; }

    if (pid == 0) {
        /* Ребёнок-читатель: закрываем write-конец, читаем до EOF. */
        close(fds[1]);
        char buf[128];
        read_all(fds[0], buf, sizeof(buf));
        close(fds[0]);
        printf("[test1] child got: \"%s\"\n", buf);
        printf("[test1] child exit 0\n");
        exit(0);
    }

    /* Родитель-писатель: закрываем read-конец, пишем, закрываем write-конец
     * (это даёт ребёнку EOF). */
    close(fds[0]);
    const char *msg = "Hello through the pipe!";
    write(fds[1], msg, strlen(msg));
    close(fds[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    return 0;
}

static int test2(void) {
    printf("[test2] parent: dup2 stdout -> pipe\n");

    int fds[2];
    if (pipe(fds) != 0) { printf("[test2] pipe() FAILED\n"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { printf("[test2] fork() FAILED\n"); return 1; }

    if (pid == 0) {
        /* Ребёнок: подменяем stdout (fd 1) на write-конец пайпа.
         * Обычный printf теперь уходит в пайп, а не на экран. */
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);          /* дубликат на fd 1 остаётся */
        printf("printf went into the pipe!");
        /* fd 1 закроется в exit() (fd_table_destroy) -> родитель увидит EOF. */
        exit(0);
    }

    /* Родитель: закрываем write-конец, читаем захваченный вывод ребёнка. */
    close(fds[1]);
    char buf[128];
    read_all(fds[0], buf, sizeof(buf));
    close(fds[0]);
    printf("[test2] captured child stdout: \"%s\"\n", buf);

    int status = 0;
    waitpid(pid, &status, 0);
    printf("[test2] child exit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    test1();
    test2();
    printf("[pipetest] all done\n");
    return 0;
}
