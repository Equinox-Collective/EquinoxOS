#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>
#include <stdbool.h>
#include "task.h"

/*
 * Synchronization primitives for the kernel.
 *
 *   spinlock_t          : busy-wait, ALSO disables IRQs while held. Use in
 *                         interrupt-context or for tiny critical sections.
 *
 *   kmutex_t            : sleeping mutex. Waiters are parked
 *                         (task->running = false). Released task is woken on
 *                         unlock.
 *
 *   ksem_t              : counting semaphore. wait() blocks while count == 0,
 *                         post() releases one waiter (or increments count).
 *
 *   waitqueue_t         : low-level building block used by mutex/sem/pipe.
 */

/* ----- Spinlock -------------------------------------------------------- */

typedef struct {
    volatile uint32_t locked;   /* 0 = free, 1 = held */
    uint64_t          saved_rflags;
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

void spin_lock(spinlock_t *s);
void spin_unlock(spinlock_t *s);

/* ----- Wait queue ------------------------------------------------------ */

typedef struct waitnode {
    task_t          *task;
    struct waitnode *next;
} waitnode_t;

typedef struct {
    spinlock_t  lock;
    waitnode_t *head;
    waitnode_t *tail;
} waitqueue_t;

void wq_init(waitqueue_t *q);

/* park current task on `q`. Caller MUST `cli` before and recheck condition. */
void wq_sleep(waitqueue_t *q);

/* wake one (FIFO) — returns true if someone was woken. */
bool wq_wake_one(waitqueue_t *q);

/* wake every parked task. */
void wq_wake_all(waitqueue_t *q);

/* ----- Mutex ----------------------------------------------------------- */

typedef struct {
    volatile uint32_t owner_pid;   /* 0 = free                              */
    spinlock_t        guard;
    waitqueue_t       waiters;
} kmutex_t;

void mutex_init(kmutex_t *m);
void mutex_lock(kmutex_t *m);
bool mutex_trylock(kmutex_t *m);
void mutex_unlock(kmutex_t *m);

/* ----- Semaphore ------------------------------------------------------- */

typedef struct {
    volatile int32_t count;
    spinlock_t       guard;
    waitqueue_t      waiters;
} ksem_t;

void sem_init(ksem_t *s, int32_t initial);
void sem_wait(ksem_t *s);          /* P() */
bool sem_trywait(ksem_t *s);
void sem_post(ksem_t *s);          /* V() */

#endif /* SYNC_H */
