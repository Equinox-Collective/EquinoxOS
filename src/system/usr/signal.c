/* src/system/usr/signal.c — Этап 4: сигналы (POSIX-подмножество).
 *
 * Модель доставки: сигналы доставляются в безопасной точке — при ВОЗВРАТЕ из
 * системного вызова в ring3 (signal_deliver() вызывается в конце
 * syscall_handler). Это покрывает основной сценарий шелла: процесс почти
 * всегда либо делает сисколлы, либо заблокирован в прерываемом ожидании
 * (waitpid/read), откуда kill() его будит.
 *
 * Кастомный обработчик запускается так: на пользовательском стеке строится
 * sigframe_t (снимок прерванного контекста + маска), ниже кладётся адрес
 * возврата = трамплин sigreturn (libc). regs перенаправляются в обработчик
 * (rdi=signum). Когда обработчик делает `ret`, управление уходит в трамплин,
 * который вызывает SYS_SIGRETURN; ядро восстанавливает контекст из sigframe.
 *
 * Ограничения этапа: нет доставки в чисто CPU-bound цикле без сисколлов
 * (придёт при следующем сисколле/таймер-хуке); SIGSTOP/SIGCONT трактуются как
 * IGNORE (job control — Этап 8); групповые kill (pid<=0) не поддержаны. */

#include <stdint.h>
#include <stdbool.h>
#include "uregs.h"
#include "signal.h"
#include "task.h"
#include "../core/cpu.h"
#include "../../syslibc/string.h"

extern void term_print(const char *str);

#define SIGBIT(s) (1ULL << (s))

/* Сигналы, которые НЕЛЬЗЯ перехватить/заблокировать. */
static inline bool sig_uncatchable(int s) {
    return s == KSIGKILL || s == KSIGSTOP;
}

/* Действие по умолчанию = IGNORE (иначе — завершить процесс). */
static inline bool default_is_ignore(int s) {
    return s == KSIGCHLD || s == KSIGCONT || s == KSIGSTOP;
}

/* ------------------------------------------------------------------ */
/* Жизненный цикл                                                      */
/* ------------------------------------------------------------------ */
void task_signal_init(struct task *t) {
    t->sig_pending = 0;
    t->sig_blocked = 0;
    for (int i = 0; i < KSIG_MAX; i++) t->sig_handlers[i] = KSIG_DFL;
    t->sig_restorer = 0;
}

void task_signal_fork(struct task *child, struct task *parent) {
    /* Дочерний наследует обработчики и маску блокировки; pending обнуляется. */
    child->sig_blocked  = parent->sig_blocked;
    child->sig_restorer = parent->sig_restorer;
    for (int i = 0; i < KSIG_MAX; i++) child->sig_handlers[i] = parent->sig_handlers[i];
    child->sig_pending  = 0;
}

void task_signal_exec(struct task *t) {
    /* POSIX: при exec перехватываемые обработчики сбрасываются в SIG_DFL,
     * SIG_IGN сохраняется; маска блокировки и pending сохраняются.
     * restorer сбросим — новый образ переустановит его через sigaction. */
    for (int i = 0; i < KSIG_MAX; i++)
        if (t->sig_handlers[i] != KSIG_IGN)
            t->sig_handlers[i] = KSIG_DFL;
    t->sig_restorer = 0;
}

/* ------------------------------------------------------------------ */
/* Отправка                                                            */
/* ------------------------------------------------------------------ */
static struct task *find_task(uint64_t pid) {
    task_t *head = task_get_list_head();
    if (!head) return NULL;
    task_t *t = head;
    do {
        if (t->id == pid) return t;
        t = t->next;
    } while (t && t != head);
    return NULL;
}

int signal_send(uint64_t pid, int sig) {
    if (sig < 0 || sig >= KSIG_MAX) return -1;
    if (pid == 0) pid = current_task ? current_task->id : 0;

    task_t *t = find_task(pid);
    if (!t) return -1;           /* ESRCH */
    if (sig == 0) return 0;      /* только проба существования */

    t->sig_pending |= SIGBIT(sig);

    /* Если цель заблокирована в прерываемом ожидании (waitpid) и сигнал не
     * заблокирован — разбудим её, чтобы ожидание прервалось и сигнал
     * доставился при возврате из сисколла. */
    if (t->waiting && !(t->sig_blocked & SIGBIT(sig)) && !t->running) {
        t->running = true;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Установка обработчика / маска                                       */
/* ------------------------------------------------------------------ */
int signal_setaction(int sig, uint64_t handler, uint64_t restorer, uint64_t *old_out) {
    if (sig <= 0 || sig >= KSIG_MAX) return -1;
    if (!current_task) return -1;
    if (old_out) *old_out = current_task->sig_handlers[sig];
    /* SIG_QUERY: только прочитать old, ничего не менять. */
    if (handler == 0xFFFFFFFFFFFFFFFFULL) return 0;
    if (sig_uncatchable(sig)) return -1;   /* SIGKILL/SIGSTOP не меняются */
    current_task->sig_handlers[sig] = handler;
    if (restorer) current_task->sig_restorer = restorer;
    return 0;
}

void signal_procmask(int how, uint64_t set, uint64_t *old_out) {
    if (!current_task) return;
    if (old_out) *old_out = current_task->sig_blocked;
    /* SIGKILL/SIGSTOP нельзя заблокировать. */
    uint64_t cant = SIGBIT(KSIGKILL) | SIGBIT(KSIGSTOP);
    switch (how) {
        case 0: current_task->sig_blocked |=  set; break;   /* BLOCK   */
        case 1: current_task->sig_blocked &= ~set; break;   /* UNBLOCK */
        case 2: current_task->sig_blocked  =  set; break;   /* SETMASK */
        default: break;
    }
    current_task->sig_blocked &= ~cant;
}

bool signal_has_pending(struct task *t) {
    if (!t) return false;
    return (t->sig_pending & ~t->sig_blocked) != 0;
}

/* Выбрать младший доставляемый сигнал (или 0). */
static int pick_signal(struct task *t) {
    uint64_t d = t->sig_pending & ~t->sig_blocked;
    if (!d) return 0;
    for (int s = 1; s < KSIG_MAX; s++)
        if (d & SIGBIT(s)) return s;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Доставка                                                            */
/* ------------------------------------------------------------------ */
static void setup_handler_frame(syscall_regs_t *regs, int sig, uint64_t handler) {
    task_t *t = current_task;

    uint64_t sp = regs->rsp;
    sp -= 128;                 /* red zone (SysV) */
    sp &= ~15ULL;
    sp -= sizeof(sigframe_t);
    sp &= ~15ULL;              /* база кадра 16-выровнена */
    uint64_t frame_addr = sp;
    sp -= 8;                   /* слот адреса возврата -> rsp%16==8 на входе */

    sigframe_t fr;
    fr.magic        = SIGFRAME_MAGIC;
    fr.signum       = (uint64_t)sig;
    fr.saved_blocked = t->sig_blocked;
    fr.saved        = *regs;   /* снимок прерванного контекста (вкл. rax-результат) */

    stac();
    memcpy((void *)frame_addr, &fr, sizeof(fr));
    *(uint64_t *)sp = t->sig_restorer;   /* адрес возврата для `ret` обработчика */
    clac();

    /* Блокируем сам сигнал на время работы обработчика (нет рекурсии). */
    t->sig_blocked |= SIGBIT(sig);

    /* Перенаправляем возврат из сисколла в обработчик. */
    regs->rip = handler;
    regs->rsp = sp;
    regs->rdi = (uint64_t)sig;  /* void handler(int signum) */
    regs->rax = (uint64_t)sig;  /* безвредно; rax-результат сохранён во фрейме */
}

void signal_deliver(syscall_regs_t *regs) {
    task_t *t = current_task;
    if (!t) return;
    /* Доставляем только при возврате в ring3 (CPL=3). */
    if ((regs->cs & 3) != 3) return;

    for (;;) {
        int sig = pick_signal(t);
        if (sig == 0) return;

        uint64_t h = t->sig_handlers[sig];

        /* Снимаем pending для выбранного сигнала. */
        t->sig_pending &= ~SIGBIT(sig);

        if (h == KSIG_IGN) continue;                 /* явно игнорируется */
        if (h == KSIG_DFL) {
            if (default_is_ignore(sig)) continue;    /* действие по умолч. = ignore */
            /* Действие по умолчанию = завершить процесс (код 128+sig). */
            term_print("[SIG] default action: terminate\n");
            task_exit_current(128 + sig);            /* не возвращается */
            return;
        }
        /* Кастомный обработчик: нужен трамплин sigreturn. Если его нет
         * (libc не инициализировала) — завершаем процесс, чтобы не уйти в
         * неопределённое поведение. */
        if (!t->sig_restorer) {
            term_print("[SIG] no restorer; terminate\n");
            task_exit_current(128 + sig);
            return;
        }
        setup_handler_frame(regs, sig, h);
        return;                                      /* один обработчик за раз */
    }
}

void signal_sigreturn(syscall_regs_t *regs) {
    task_t *t = current_task;
    if (!t) return;

    sigframe_t fr;
    stac();
    memcpy(&fr, (void *)regs->rsp, sizeof(fr));
    clac();

    if (fr.magic != SIGFRAME_MAGIC) {
        term_print("[SIG] bad sigframe; terminate\n");
        task_exit_current(139);                      /* как SIGSEGV-ish */
        return;
    }

    /* Восстанавливаем прерванный контекст и маску блокировки. */
    *regs = fr.saved;
    t->sig_blocked = fr.saved_blocked;
}
