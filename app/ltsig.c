/*
 * ltsig.c — Этап 6d: настоящие сигналы через vendored-musl (Linux-ABI).
 *
 * Линкуется с musl (как dirtest/stattest). Проверяет путь
 *   sigaction(2) -> int 0x81 -> linux_syscall_handler case 13 (rt_sigaction)
 *   sigprocmask -> case 14 (rt_sigprocmask)
 *   raise/kill   -> case 62 (kill) + доставка через трамплин __restore_rt.
 *
 * Сценарии:
 *   1) sigaction(SIGUSR1, handler) + raise(SIGUSR1): обработчик срабатывает,
 *      управление возвращается после raise (sigreturn-трамплин работает).
 *   2) SIG_IGN на SIGUSR2 + raise: процесс жив, обработчик не зван.
 *   3) sigprocmask(SIG_BLOCK, SIGUSR1) — raise откладывается (pending),
 *      обработчик НЕ зван; после SIG_UNBLOCK — доставляется.
 *   4) oldact: повторный sigaction с oldact!=NULL возвращает прежний обработчик.
 */
#include <stdio.h>
#include <signal.h>
#include <string.h>

static volatile sig_atomic_t got_usr1 = 0;
static volatile sig_atomic_t got_usr2 = 0;
static volatile int last_sig = 0;

static void on_usr1(int sig) { got_usr1++; last_sig = sig; }
static void on_usr2(int sig) { got_usr2++; last_sig = sig; }

int main(void) {
    int fails = 0;
    printf("ltsig: rt_sigaction/rt_sigprocmask 6d\n");

    /* 1) установить обработчик SIGUSR1 и доставить его себе */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0) { printf("  sigaction(USR1) failed\n"); fails++; }
    raise(SIGUSR1);
    if (got_usr1 == 1 && last_sig == SIGUSR1)
        printf("  [1] handler SIGUSR1 ran, got_usr1=%d OK\n", (int)got_usr1);
    else { printf("  [1] FAIL got_usr1=%d last=%d\n", (int)got_usr1, last_sig); fails++; }

    /* 2) SIG_IGN на SIGUSR2 — raise не должен ничего вызвать и не убить нас */
    struct sigaction ig;
    memset(&ig, 0, sizeof ig);
    ig.sa_handler = SIG_IGN;
    sigaction(SIGUSR2, &ig, NULL);
    raise(SIGUSR2);
    if (got_usr2 == 0) printf("  [2] SIGUSR2 ignored, still alive OK\n");
    else { printf("  [2] FAIL got_usr2=%d\n", (int)got_usr2); fails++; }

    /* 3) заблокировать SIGUSR1, raise — должно отложиться; затем разблокировать */
    sigaction(SIGUSR1, &sa, NULL);   /* вернуть обработчик */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, &old);
    int before = (int)got_usr1;
    raise(SIGUSR1);
    if (got_usr1 == before) printf("  [3a] SIGUSR1 blocked, pending OK\n");
    else { printf("  [3a] FAIL delivered while blocked got_usr1=%d\n", (int)got_usr1); fails++; }
    sigprocmask(SIG_UNBLOCK, &block, NULL);  /* при возврате доставится */
    if (got_usr1 == before + 1) printf("  [3b] SIGUSR1 delivered after unblock OK\n");
    else { printf("  [3b] FAIL got_usr1=%d (want %d)\n", (int)got_usr1, before + 1); fails++; }

    /* 4) oldact: установить новый обработчик, проверить что вернулся прежний */
    struct sigaction prev;
    memset(&prev, 0, sizeof prev);
    sa.sa_handler = on_usr2;
    if (sigaction(SIGUSR1, &sa, &prev) == 0 && prev.sa_handler == on_usr1)
        printf("  [4] oldact returned previous handler OK\n");
    else { printf("  [4] FAIL oldact=%p want=%p\n", (void *)prev.sa_handler, (void *)on_usr1); fails++; }

    printf("ltsig: %s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
