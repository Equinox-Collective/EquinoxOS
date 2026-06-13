#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "uregs.h"

struct task; /* fwd (task.h) */

/* Кол-во поддерживаемых сигналов (1..31). 0 не используется (kill(pid,0)=проба). */
#define KSIG_MAX 32

/* Номера сигналов — совпадают с Linux x86_64 (см. sdk/include/signal.h). */
#define KSIGHUP   1
#define KSIGINT   2
#define KSIGQUIT  3
#define KSIGILL   4
#define KSIGTRAP  5
#define KSIGABRT  6
#define KSIGBUS   7
#define KSIGFPE   8
#define KSIGKILL  9
#define KSIGUSR1  10
#define KSIGSEGV  11
#define KSIGUSR2  12
#define KSIGPIPE  13
#define KSIGALRM  14
#define KSIGTERM  15
#define KSIGCHLD  17
#define KSIGCONT  18
#define KSIGSTOP  19

#define KSIG_DFL 0ULL
#define KSIG_IGN 1ULL

/* Кадр, сохраняемый на пользовательском стеке перед вызовом обработчика;
 * читается обратно в SYS_SIGRETURN для восстановления контекста. Только ядро
 * пишет/читает его, поэтому важна лишь внутренняя согласованность. */
#define SIGFRAME_MAGIC 0x5347465254524553ULL /* "SESRTRGS" little-endian-ish */
typedef struct {
    uint64_t magic;
    uint64_t signum;
    uint64_t saved_blocked;
    /* Полный снимок syscall_regs_t (GP + iretq-кадр) прерванного контекста. */
    syscall_regs_t saved;
} sigframe_t;

/* --- Жизненный цикл сигнального состояния задачи --- */
void task_signal_init(struct task *t);                 /* всё в SIG_DFL, маски=0 */
void task_signal_fork(struct task *child, struct task *parent); /* копия обработчиков/масок, pending=0 */
void task_signal_exec(struct task *t);                 /* обработчики != SIG_IGN -> SIG_DFL; маска/pending сохраняются */

/* Послать сигнал процессу pid (>0). sig==0 — только проверка существования.
 * Возвращает 0 при успехе, -1 если процесс не найден или sig некорректен. */
int  signal_send(uint64_t pid, int sig);

/* Установить обработчик. handler: 0=SIG_DFL,1=SIG_IGN,иначе адрес ring3.
 * restorer — адрес трамплина sigreturn (libc), запоминается в задаче.
 * old_out (может быть NULL) получает прежний обработчик. SIGKILL/SIGSTOP
 * нельзя перехватить/игнорировать. Возвращает 0/-1. */
int  signal_setaction(int sig, uint64_t handler, uint64_t restorer, uint64_t *old_out);

/* Изменить маску блокировки текущей задачи. how: 0=BLOCK,1=UNBLOCK,2=SETMASK.
 * set — новая маска (битовая). old_out (может быть NULL) — прежняя. */
void signal_procmask(int how, uint64_t set, uint64_t *old_out);

/* Есть ли у задачи доставляемый (pending & ~blocked) сигнал? */
bool signal_has_pending(struct task *t);

/* Вызывается в конце syscall_handler перед возвратом в ring3. Если у текущей
 * задачи есть доставляемый сигнал — выполняет действие по умолчанию
 * (завершение процесса — не возвращается) или строит кадр обработчика на
 * пользовательском стеке и перенаправляет regs в обработчик. */
void signal_deliver(syscall_regs_t *regs);

/* SYS_SIGRETURN: восстанавливает контекст из sigframe на пользовательском
 * стеке (regs->rsp) обратно в regs и возвращает маску блокировки. */
void signal_sigreturn(syscall_regs_t *regs);

#endif
