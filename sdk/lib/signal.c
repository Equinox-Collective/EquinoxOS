/* sdk/lib/signal.c — Этап 4: пользовательская обёртка над сигнальными
 * сисколлами (kill/signal/sigaction/raise/sigprocmask).
 *
 * Обработчики регистрируются вместе с адресом трамплина возврата
 * (__equos_sigreturn_trampoline, sdk/lib/sigtramp.asm): ядро кладёт его как
 * адрес возврата на стек обработчика, и `ret` обработчика уходит в трамплин,
 * который вызывает SYS_SIGRETURN для восстановления контекста. */

#include <signal.h>
#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <equos.h>

/* Трамплин из sigtramp.asm. Его адрес передаём ядру как sa_restorer. */
extern void __equos_sigreturn_trampoline(void);

int kill(pid_t pid, int sig) {
    int rc = sys_kill((uint64_t)pid, sig);
    if (rc < 0) { errno = 3 /* ESRCH */; return -1; }
    return 0;
}

int raise(int sig) {
    return kill(getpid(), sig);
}

void (*signal(int sig, void (*func)(int)))(int) {
    uint64_t old = 0;
    int rc = sys_sigaction(sig, (uint64_t)func,
                           (uint64_t)__equos_sigreturn_trampoline, &old);
    if (rc < 0) { errno = 22 /* EINVAL */; return SIG_ERR; }
    return (void (*)(int))old;
}

#define __SIG_QUERY 0xFFFFFFFFFFFFFFFFULL  /* «только прочитать old» (см. ядро) */

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    uint64_t old = 0;
    /* act==NULL -> только запрос (не меняем обработчик). */
    uint64_t handler = act ? (uint64_t)act->sa_handler : __SIG_QUERY;
    int rc = sys_sigaction(sig, handler,
                           (uint64_t)__equos_sigreturn_trampoline, &old);
    if (rc < 0) { errno = 22 /* EINVAL */; return -1; }
    if (oldact) {
        oldact->sa_handler  = (void (*)(int))old;
        oldact->sa_mask     = 0;
        oldact->sa_flags    = 0;
        oldact->sa_restorer = __equos_sigreturn_trampoline;
    }
    return 0;
}

/* --- sigset helpers (маска = битовое поле) --- */
int sigemptyset(sigset_t *set) { if (set) *set = 0; return 0; }
int sigfillset(sigset_t *set)  { if (set) *set = ~0ULL; return 0; }

int sigaddset(sigset_t *set, int sig) {
    if (!set || sig <= 0 || sig >= NSIG) { errno = 22; return -1; }
    *set |= (1ULL << sig);
    return 0;
}
int sigdelset(sigset_t *set, int sig) {
    if (!set || sig <= 0 || sig >= NSIG) { errno = 22; return -1; }
    *set &= ~(1ULL << sig);
    return 0;
}
int sigismember(const sigset_t *set, int sig) {
    if (!set || sig <= 0 || sig >= NSIG) { errno = 22; return -1; }
    return (*set & (1ULL << sig)) ? 1 : 0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    uint64_t old = 0;
    uint64_t s = set ? *set : 0;
    /* Если set==NULL — не менять, только прочитать: ставим SETMASK с текущим.
     * Чтобы прочитать текущее, делаем UNBLOCK 0 (no-op), ядро вернёт old. */
    int how_k = set ? how : 1 /* UNBLOCK */;
    sys_sigprocmask(how_k, s, &old);
    if (oldset) *oldset = old;
    return 0;
}
