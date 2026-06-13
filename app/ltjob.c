/*
 * ltjob.c — Этап 6e-1: управление процессами / группами через vendored-musl.
 *
 * Линкуется с musl (как ltsig/dirtest). Проверяет путь:
 *   fork(2)    -> int 0x81 -> case 57 -> SYS_FORK
 *   wait4(2)   -> case 61 (кодирование Linux wait-status: WIFEXITED/WEXITSTATUS)
 *   getpgrp/getpgid/setpgid/setsid -> case 111/121/109/112
 *
 * Сценарии:
 *   1) fork + ребёнок _exit(42); родитель waitpid → WIFEXITED && WEXITSTATUS==42.
 *   2) getpgrp() == getpid() (процесс — лидер своей группы).
 *   3) setpgid(0,0)==0 и getpgid(0)==getpid().
 *   4) fork ребёнка; setpgid(child,child); getpgid(child)==child; reap → status 7.
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    int fails = 0;
    printf("ltjob: process/job control 6e-1\n");

    /* 1) fork + код выхода через wait-status */
    pid_t c1 = fork();
    if (c1 == 0) { _exit(42); }
    if (c1 < 0) { printf("  fork(1) failed\n"); return 1; }
    int st = 0;
    pid_t r = waitpid(c1, &st, 0);
    if (r == c1 && WIFEXITED(st) && WEXITSTATUS(st) == 42)
        printf("  [1] child exited, WEXITSTATUS=%d OK\n", WEXITSTATUS(st));
    else { printf("  [1] FAIL r=%d WIFEXITED=%d WEXITSTATUS=%d\n",
                  (int)r, WIFEXITED(st), WEXITSTATUS(st)); fails++; }

    /* 2) getpgrp() == getpid() */
    pid_t me = getpid();
    if (getpgrp() == me) printf("  [2] getpgrp()==getpid()=%d OK\n", (int)me);
    else { printf("  [2] FAIL getpgrp=%d pid=%d\n", (int)getpgrp(), (int)me); fails++; }

    /* 3) setpgid(0,0) + getpgid(0) */
    if (setpgid(0, 0) == 0 && getpgid(0) == me)
        printf("  [3] setpgid/getpgid self OK\n");
    else { printf("  [3] FAIL getpgid=%d\n", (int)getpgid(0)); fails++; }

    /* 4) поместить ребёнка в собственную группу */
    pid_t c2 = fork();
    if (c2 == 0) { _exit(7); }
    if (c2 < 0) { printf("  fork(2) failed\n"); return 1; }
    /* зомби остаётся в таблице до reap, поэтому setpgid сработает без гонки */
    if (setpgid(c2, c2) == 0 && getpgid(c2) == c2)
        printf("  [4] child placed in own pgrp=%d OK\n", (int)c2);
    else { printf("  [4] FAIL getpgid(c2)=%d\n", (int)getpgid(c2)); fails++; }
    int st2 = 0;
    waitpid(c2, &st2, 0);
    if (!(WIFEXITED(st2) && WEXITSTATUS(st2) == 7))
        { printf("  [4b] FAIL reap status\n"); fails++; }

    printf(fails ? "ltjob: FAIL\n" : "ltjob: PASS\n");
    return fails ? 1 : 0;
}
