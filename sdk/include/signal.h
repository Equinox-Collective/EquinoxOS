#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <stdint.h>
#include <sys/types.h>

typedef int sig_atomic_t;
typedef void (*__sighandler_t)(int);

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

/* Номера сигналов (как в Linux x86_64). */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGIOT   6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20

#define NSIG 32

/* sigset_t — простая 64-битная маска (бит N = сигнал N). */
typedef uint64_t sigset_t;

/* sigprocmask how */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

struct sigaction {
    void   (*sa_handler)(int);
    sigset_t sa_mask;        /* пока не применяется ядром (зарезервировано) */
    int      sa_flags;       /* зарезервировано */
    void   (*sa_restorer)(void);
};

/* --- API --- */
void (*signal(int sig, void (*func)(int)))(int);
int  raise(int sig);
int  kill(pid_t pid, int sig);
int  sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);

int  sigemptyset(sigset_t *set);
int  sigfillset(sigset_t *set);
int  sigaddset(sigset_t *set, int sig);
int  sigdelset(sigset_t *set, int sig);
int  sigismember(const sigset_t *set, int sig);
int  sigprocmask(int how, const sigset_t *set, sigset_t *oldset);

#endif
