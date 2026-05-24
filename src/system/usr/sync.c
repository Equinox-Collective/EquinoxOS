#include "sync.h"
#include "task.h"
#include "../mem/memory.h"
#include "../../syslibc/string.h"

extern void term_print(const char *str);

/* ---------- IRQ helpers ------------------------------------------------ */

static inline uint64_t save_irq(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void restore_irq(uint64_t flags) {
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

/* ---------- Spinlock --------------------------------------------------- */
/*
 * Single-CPU kernel — a true CAS spinlock is overkill, but we still use one
 * (a) to be SMP-friendly the day APs come online, (b) because we WANT IRQ
 * disabling around the critical section.
 */

void spin_lock(spinlock_t *s) {
    uint64_t flags = save_irq();
    while (__atomic_test_and_set(&s->locked, __ATOMIC_ACQUIRE)) {
        /* In a true UP kernel with IRQs off, the holder cannot be us, so this
         * branch shouldn't be hit. Keep it for the SMP future. */
        __asm__ volatile("pause");
    }
    s->saved_rflags = flags;
}

void spin_unlock(spinlock_t *s) {
    uint64_t flags = s->saved_rflags;
    __atomic_clear(&s->locked, __ATOMIC_RELEASE);
    restore_irq(flags);
}

/* ---------- Wait queue ------------------------------------------------- */

void wq_init(waitqueue_t *q) {
    q->lock.locked       = 0;
    q->lock.saved_rflags = 0;
    q->head = q->tail = (waitnode_t *)0;
}

void wq_sleep(waitqueue_t *q) {
    /* Allocate node BEFORE locking — kmalloc may want IRQs on inside. */
    waitnode_t *node = (waitnode_t *)kmalloc(sizeof(*node));
    if (!node) {
        term_print("[SYNC] wq_sleep: OOM, spinning instead\n");
        while (1) __asm__ volatile("pause");
    }
    node->task = current_task;
    node->next = (waitnode_t *)0;

    spin_lock(&q->lock);
    if (q->tail) q->tail->next = node;
    else         q->head       = node;
    q->tail = node;
    current_task->running = false;
    spin_unlock(&q->lock);

    /* Drop into the scheduler — `running = false` means we won't be picked
     * until someone toggles it back on. */
    yield();
}

bool wq_wake_one(waitqueue_t *q) {
    spin_lock(&q->lock);
    waitnode_t *n = q->head;
    if (!n) { spin_unlock(&q->lock); return false; }
    q->head = n->next;
    if (!q->head) q->tail = (waitnode_t *)0;
    spin_unlock(&q->lock);

    n->task->running = true;
    kfree(n);
    return true;
}

void wq_wake_all(waitqueue_t *q) {
    while (wq_wake_one(q)) { /* nothing */ }
}

/* ---------- Mutex ------------------------------------------------------ */

void mutex_init(kmutex_t *m) {
    m->owner_pid          = 0;
    m->guard.locked       = 0;
    m->guard.saved_rflags = 0;
    wq_init(&m->waiters);
}

bool mutex_trylock(kmutex_t *m) {
    bool got = false;
    spin_lock(&m->guard);
    if (m->owner_pid == 0) {
        m->owner_pid = (uint32_t)current_task->id;
        got = true;
    }
    spin_unlock(&m->guard);
    return got;
}

void mutex_lock(kmutex_t *m) {
    for (;;) {
        if (mutex_trylock(m)) return;
        /* Park ourselves */
        wq_sleep(&m->waiters);
    }
}

void mutex_unlock(kmutex_t *m) {
    spin_lock(&m->guard);
    m->owner_pid = 0;
    spin_unlock(&m->guard);
    wq_wake_one(&m->waiters);
}

/* ---------- Semaphore -------------------------------------------------- */

void sem_init(ksem_t *s, int32_t initial) {
    s->count              = initial;
    s->guard.locked       = 0;
    s->guard.saved_rflags = 0;
    wq_init(&s->waiters);
}

bool sem_trywait(ksem_t *s) {
    bool ok = false;
    spin_lock(&s->guard);
    if (s->count > 0) { s->count--; ok = true; }
    spin_unlock(&s->guard);
    return ok;
}

void sem_wait(ksem_t *s) {
    for (;;) {
        if (sem_trywait(s)) return;
        wq_sleep(&s->waiters);
    }
}

void sem_post(ksem_t *s) {
    spin_lock(&s->guard);
    s->count++;
    spin_unlock(&s->guard);
    wq_wake_one(&s->waiters);
}
