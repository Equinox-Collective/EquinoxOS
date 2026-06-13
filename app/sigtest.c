/*
 * sigtest — приёмочный тест Этапа 4 (сигналы).
 *
 * Сценарии:
 *   1) signal()+raise(): синхронная доставка собственного сигнала и возврат
 *      через трамплин sigreturn (выполнение продолжается после raise).
 *   2) SIG_IGN: сигнал игнорируется, процесс продолжает работу.
 *   3) fork + kill с кастомным обработчиком в ребёнке (SIGTERM -> exit 42);
 *      родитель ловит SIGCHLD; синхронизация через pipe.
 *   4) fork + действие по умолчанию (нет обработчика) -> процесс завершается
 *      с кодом 128+SIGTERM = 143.
 *
 * Ожидаемый вывод (PID'ы динамические):
 *   [sigtest] start
 *   [main] before raise SIGUSR1
 *   [handler] caught SIGUSR1 (sig=10)
 *   [main] after raise, got_usr1=1
 *   [main] after raise SIGUSR2 (ignored), still alive
 *   [main] child ready, sending SIGTERM to <pid>
 *   [child] caught SIGTERM, exiting 42
 *   [handler] caught SIGCHLD (sig=17)
 *   [main] child <pid> exited, status=42 (expect 42)
 *   [handler] caught SIGCHLD (sig=17)
 *   [main] child2 <pid2> default-terminated, status=143 (expect 143)
 *   [sigtest] all done
 *
 * Примечание: обработчик SIGCHLD печатается ДО строки "...exited", т.к. сигнал
 * доставляется при возврате из waitpid (до выхода в пользовательский код).
 */
#include <equos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static volatile int got_usr1 = 0;

static void on_usr1(int s) {
    printf("[handler] caught SIGUSR1 (sig=%d)\n", s);
    got_usr1 = 1;
}
static void on_usr2(int s) {
    printf("[handler] SHOULD NOT RUN (SIGUSR2 sig=%d)\n", s);
}
static void on_chld(int s) {
    printf("[handler] caught SIGCHLD (sig=%d)\n", s);
}
static void child_term(int s) {
    printf("[child] caught SIGTERM, exiting 42\n");
    (void)s;
    exit(42);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[sigtest] start\n");

    /* 1. signal + raise (синхронная доставка + sigreturn) */
    signal(SIGUSR1, on_usr1);
    printf("[main] before raise SIGUSR1\n");
    raise(SIGUSR1);
    printf("[main] after raise, got_usr1=%d\n", got_usr1);

    /* 2. SIG_IGN */
    signal(SIGUSR2, on_usr2);   /* сначала ставим обработчик... */
    signal(SIGUSR2, SIG_IGN);   /* ...затем игнор — он и должен победить */
    raise(SIGUSR2);
    printf("[main] after raise SIGUSR2 (ignored), still alive\n");

    /* 3. fork + kill (кастомный обработчик у ребёнка) + SIGCHLD у родителя */
    signal(SIGCHLD, on_chld);
    int sync[2];
    pipe(sync);
    pid_t pid = fork();
    if (pid == 0) {
        close(sync[0]);
        signal(SIGTERM, child_term);
        char c = 'r';
        write(sync[1], &c, 1);          /* сигнализируем готовность */
        while (1) { getpid(); }         /* крутимся, делая сисколлы */
    }
    close(sync[1]);
    char c;
    read(sync[0], &c, 1);               /* ждём, пока ребёнок поставит обработчик */
    printf("[main] child ready, sending SIGTERM to %d\n", (int)pid);
    kill(pid, SIGTERM);
    int st = 0;
    pid_t r = waitpid(pid, &st, 0);
    printf("[main] child %d exited, status=%d (expect 42)\n", (int)r, st);

    /* 4. fork + действие по умолчанию (нет обработчика SIGTERM -> terminate) */
    pid_t pid2 = fork();
    if (pid2 == 0) {
        while (1) { getpid(); }         /* без обработчика */
    }
    for (int i = 0; i < 2000; i++) getpid();   /* дать ребёнку стартовать */
    kill(pid2, SIGTERM);
    int st2 = 0;
    pid_t r2 = waitpid(pid2, &st2, 0);
    printf("[main] child2 %d default-terminated, status=%d (expect %d)\n",
           (int)r2, st2, 128 + SIGTERM);

    printf("[sigtest] all done\n");
    return 0;
}
