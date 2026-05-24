#include "ipc.h"
#include "sync.h"
#include "task.h"
#include "../mem/memory.h"
#include "../mem/vmm.h"
#include "../core/cpu.h"
#include "../../syslibc/string.h"

extern void term_print(const char *str);

/* =========================================================================
 *  Pipes
 * ========================================================================= */

typedef struct {
    bool        used;
    uint8_t     buf[PIPE_BUF_SIZE];
    uint32_t    head;          /* next byte to read  */
    uint32_t    tail;          /* next byte to write */
    uint32_t    count;
    bool        closed;
    kmutex_t    lock;
    waitqueue_t readers;
    waitqueue_t writers;
} pipe_t;

static pipe_t pipes[MAX_PIPES];

int pipe_create(void) {
    for (int i = 0; i < MAX_PIPES; ++i) {
        if (!pipes[i].used) {
            pipe_t *p = &pipes[i];
            memset(p, 0, sizeof(*p));
            p->used = true;
            mutex_init(&p->lock);
            wq_init(&p->readers);
            wq_init(&p->writers);
            return i;
        }
    }
    return -1;
}

static bool pipe_valid(int id) {
    return id >= 0 && id < MAX_PIPES && pipes[id].used;
}

int32_t pipe_read(int id, void *user_buf, uint32_t size) {
    if (!pipe_valid(id) || size == 0) return -1;
    pipe_t *p = &pipes[id];

    uint32_t written = 0;
    uint8_t *dst = (uint8_t *)user_buf;

    while (written < size) {
        mutex_lock(&p->lock);

        /* Drain as much as possible. */
        while (p->count > 0 && written < size) {
            uint8_t b = p->buf[p->head];
            p->head = (p->head + 1) % PIPE_BUF_SIZE;
            p->count--;
            stac();
            dst[written++] = b;
            clac();
        }

        bool got_some = written > 0;
        bool closed   = p->closed;
        mutex_unlock(&p->lock);

        /* Wake any blocked writer — pipe just got drained. */
        wq_wake_all(&p->writers);

        if (got_some || closed) break;

        /* Empty — block. */
        wq_sleep(&p->readers);
    }

    return (int32_t)written;
}

int32_t pipe_write(int id, const void *user_buf, uint32_t size) {
    if (!pipe_valid(id) || size == 0) return -1;
    pipe_t *p = &pipes[id];

    uint32_t written = 0;
    const uint8_t *src = (const uint8_t *)user_buf;

    while (written < size) {
        mutex_lock(&p->lock);

        while (p->count < PIPE_BUF_SIZE && written < size) {
            stac();
            uint8_t b = src[written++];
            clac();
            p->buf[p->tail] = b;
            p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
            p->count++;
        }

        bool was_progress = written > 0;
        bool closed       = p->closed;
        mutex_unlock(&p->lock);

        if (was_progress) wq_wake_all(&p->readers);

        if (closed) break;
        if (written < size) wq_sleep(&p->writers);
    }

    return (int32_t)written;
}

void pipe_close(int id) {
    if (!pipe_valid(id)) return;
    pipe_t *p = &pipes[id];
    mutex_lock(&p->lock);
    p->closed = true;
    mutex_unlock(&p->lock);
    wq_wake_all(&p->readers);
    wq_wake_all(&p->writers);

    /* If the buffer is empty and we're closed, fully free the slot. We
     * keep a closed-but-not-empty pipe around so the last reader can drain. */
    if (p->count == 0) {
        p->used = false;
    }
}

/* =========================================================================
 *  Message queue
 * ========================================================================= */

typedef struct {
    uint32_t prio;
    uint32_t size;
    uint8_t  data[MQUEUE_MAX_MSG_SIZE];
} mq_msg_t;

typedef struct {
    bool        used;
    uint32_t    msg_size;
    mq_msg_t    msgs[MQUEUE_MAX_MSGS];
    uint32_t    count;          /* current valid msgs                       */
    kmutex_t    lock;
    waitqueue_t senders;
    waitqueue_t receivers;
} mqueue_t;

static mqueue_t mqs[MAX_MQUEUES];

int mq_create(uint32_t msg_size) {
    if (msg_size == 0 || msg_size > MQUEUE_MAX_MSG_SIZE) return -1;
    for (int i = 0; i < MAX_MQUEUES; ++i) {
        if (!mqs[i].used) {
            mqueue_t *q = &mqs[i];
            memset(q, 0, sizeof(*q));
            q->used     = true;
            q->msg_size = msg_size;
            mutex_init(&q->lock);
            wq_init(&q->senders);
            wq_init(&q->receivers);
            return i;
        }
    }
    return -1;
}

static bool mq_valid(int id) {
    return id >= 0 && id < MAX_MQUEUES && mqs[id].used;
}

/* Find insertion index — keep array roughly sorted by priority descending. */
static int mq_insert_index(mqueue_t *q, uint32_t prio) {
    /* Linear scan; queue is small. */
    int idx = (int)q->count;
    for (int i = 0; i < (int)q->count; ++i) {
        if (prio > q->msgs[i].prio) { idx = i; break; }
    }
    return idx;
}

int32_t mq_send(int id, const void *user_buf, uint32_t prio) {
    if (!mq_valid(id)) return -1;
    mqueue_t *q = &mqs[id];

    for (;;) {
        mutex_lock(&q->lock);
        if (q->count < MQUEUE_MAX_MSGS) {
            int pos = mq_insert_index(q, prio);
            /* shift right from pos */
            for (int i = (int)q->count; i > pos; --i)
                q->msgs[i] = q->msgs[i - 1];

            mq_msg_t *m = &q->msgs[pos];
            m->prio = prio;
            m->size = q->msg_size;
            stac();
            memcpy(m->data, user_buf, q->msg_size);
            clac();
            q->count++;

            mutex_unlock(&q->lock);
            wq_wake_one(&q->receivers);
            return (int32_t)q->msg_size;
        }
        mutex_unlock(&q->lock);
        wq_sleep(&q->senders);
    }
}

int32_t mq_recv(int id, void *user_buf) {
    if (!mq_valid(id)) return -1;
    mqueue_t *q = &mqs[id];

    for (;;) {
        mutex_lock(&q->lock);
        if (q->count > 0) {
            mq_msg_t m = q->msgs[0];
            /* shift left */
            for (uint32_t i = 0; i + 1 < q->count; ++i)
                q->msgs[i] = q->msgs[i + 1];
            q->count--;

            mutex_unlock(&q->lock);
            stac();
            memcpy(user_buf, m.data, m.size);
            clac();
            wq_wake_one(&q->senders);
            return (int32_t)m.size;
        }
        mutex_unlock(&q->lock);
        wq_sleep(&q->receivers);
    }
}

void mq_close(int id) {
    if (!mq_valid(id)) return;
    mqs[id].used = false;
    wq_wake_all(&mqs[id].senders);
    wq_wake_all(&mqs[id].receivers);
}

/* =========================================================================
 *  Init
 * ========================================================================= */

void ipc_init(void) {
    for (int i = 0; i < MAX_PIPES;   ++i) pipes[i].used = false;
    for (int i = 0; i < MAX_MQUEUES; ++i) mqs  [i].used = false;
    term_print("[IPC] Pipes + message queues ready.\n");
}
