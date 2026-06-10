/*
 * ltjob2.c — Этап 6e-2: tcsetpgrp/tcgetpgrp (foreground-группа терминала)
 *            + WNOHANG в wait4. Линкуется с vendored-musl.
 *
 *   tcgetpgrp -> ioctl(fd, TIOCGPGRP=0x540F, &pgrp) -> SYS_IOCTL case
 *   tcsetpgrp -> ioctl(fd, TIOCSPGRP=0x5410, &pgrp)
 *   waitpid(...,WNOHANG) -> wait4(opt&1) -> task_waitpid_ex(nohang=1)
 *
 * Сценарии:
 *   1) tcgetpgrp(0) по умолчанию == getpgrp() (мы на переднем плане).
 *   2) tcsetpgrp(0,pid)==0, затем tcgetpgrp(0)==pid (round-trip).
 *   3) fork ребёнка, который ещё крутится → waitpid(WNOHANG) вернёт 0.
 *   4) блокирующий waitpid дожидается → WEXITSTATUS==5.
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    int fails = 0;
    printf("ltjob2: tcsetpgrp/tcgetpgrp + WNOHANG 6e-2\n");

    pid_t me = getpid();

    /* 1) tcgetpgrp по умолчанию == собственная группа */
    pid_t fg = tcgetpgrp(0);
    if (fg == getpgrp()) printf("  [1] tcgetpgrp(0) default==%d OK\n", (int)fg);
    else { printf("  [1] FAIL tcgetpgrp=%d pgrp=%d\n", (int)fg, (int)getpgrp()); fails++; }

    /* 2) tcsetpgrp + tcgetpgrp round-trip */
    if (tcsetpgrp(0, me) == 0 && tcgetpgrp(0) == me)
        printf("  [2] tcsetpgrp round-trip pgrp=%d OK\n", (int)me);
    else { printf("  [2] FAIL tcgetpgrp=%d\n", (int)tcgetpgrp(0)); fails++; }

    /* 3) WNOHANG: ребёнок ещё крутится → waitpid вернёт 0 */
    pid_t c = fork();
    if (c == 0) { for (volatile long i = 0; i < 20000000L; i++) {} _exit(5); }
    if (c < 0) { printf("  fork failed\n"); return 1; }
    int st = 0;
    pid_t r = waitpid(c, &st, WNOHANG);
    if (r == 0) printf("  [3] WNOHANG -> 0 (child running) OK\n");
    else { printf("  [3] FAIL WNOHANG r=%d\n", (int)r); fails++; }

    /* 4) блокирующий reap */
    r = waitpid(c, &st, 0);
    if (r == c && WIFEXITED(st) && WEXITSTATUS(st) == 5)
        printf("  [4] blocking reap WEXITSTATUS=5 OK\n");
    else { printf("  [4] FAIL r=%d WEXITSTATUS=%d\n", (int)r, WEXITSTATUS(st)); fails++; }

    printf(fails ? "ltjob2: FAIL\n" : "ltjob2: PASS\n");
    return fails ? 1 : 0;
}
