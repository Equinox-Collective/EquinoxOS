/*
 * exectest — приёмочный тест Этапа 1b (fork + execve).
 *
 * Самоссылочный сценарий (одна бинарка):
 *   - запуск без аргументов  -> родительская ветка: fork(), ребёнок делает
 *     execve("exectest.elf", {"exectest.elf", "child"}), родитель ждёт его.
 *   - запуск с аргументом     -> "execee"-ветка: печатает argv и exit(7).
 *
 * Ожидаемый вывод:
 *   [parent] my pid = <P>, forking...
 *   [child]  pid = <C>, execve(exectest.elf, {.., "child"})
 *   [execee] new image is live! argc=2 argv[1]=child
 *   [parent] child <C> exited with code 7
 */
#include <equos.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    /* --- "execee"-ветка: мы пришли сюда через execve --- */
    if (argc >= 2) {
        printf("[execee] new image is live! argc=%d argv[1]=%s\n",
               argc, argv[1] ? argv[1] : "(null)");
        exit(7);
    }

    /* --- родительская ветка --- */
    printf("[parent] my pid = %ld, forking...\n", (long)getpid());

    pid_t pid = fork();
    if (pid < 0) {
        printf("[fork] FAILED\n");
        return 1;
    }

    if (pid == 0) {
        printf("[child]  pid = %ld, execve(bin/exectest.elf, {.., \"child\"})\n",
               (long)getpid());
        /* VFS хранит файлы как "bin/<name>"; ядро также умеет дописывать "bin/"
         * к голому имени, но передаём полный путь явно. */
        char *av[] = { "bin/exectest.elf", "child", 0 };
        char *ev[] = { 0 };
        execve("bin/exectest.elf", av, ev);
        /* сюда попадаем только если execve провалился */
        printf("[child]  execve FAILED (still old image)\n");
        exit(99);
    }

    int status = 0;
    pid_t reaped = waitpid(pid, &status, 0);
    if (reaped == pid && WIFEXITED(status)) {
        printf("[parent] child %ld exited with code %d (expect 7)\n",
               (long)reaped, WEXITSTATUS(status));
    } else {
        printf("[parent] waitpid returned %ld (status=%d)\n",
               (long)reaped, status);
    }
    return 0;
}
