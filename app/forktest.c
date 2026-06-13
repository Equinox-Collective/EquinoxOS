/*
 * forktest — приёмочный тест Этапа 1 (fork / exit-status / waitpid).
 *
 * Ожидаемый вывод:
 *   [parent] my pid = <P>
 *   [parent] forked child pid = <C>
 *   [child]  hello from child, pid = <C>, ppid seen via getpid
 *   [parent] child <C> exited with code 42
 */
#include <equos.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    printf("[parent] my pid = %ld\n", (long)getpid());

    pid_t pid = fork();
    if (pid < 0) {
        printf("[fork] FAILED\n");
        return 1;
    }

    if (pid == 0) {
        /* --- ветка ребёнка --- */
        printf("[child]  hello from child, pid = %ld\n", (long)getpid());
        exit(42);
    }

    /* --- ветка родителя --- */
    printf("[parent] forked child pid = %ld\n", (long)pid);

    int status = 0;
    pid_t reaped = waitpid(pid, &status, 0);
    if (reaped == pid && WIFEXITED(status)) {
        printf("[parent] child %ld exited with code %d\n",
               (long)reaped, WEXITSTATUS(status));
    } else {
        printf("[parent] waitpid returned %ld (status=%d)\n",
               (long)reaped, status);
    }
    return 0;
}
