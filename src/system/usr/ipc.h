#ifndef IPC_H
#define IPC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Inter-Process Communication.
 *
 *   pipe_t       : byte-stream FIFO ring buffer. read() blocks while empty,
 *                  write() blocks while full. Multiple readers/writers are
 *                  serialized by the pipe's internal mutex.
 *
 *   mqueue_t     : datagram queue (fixed-size messages with priority). Up to
 *                  MQUEUE_MAX_MSGS messages, each up to MQUEUE_MAX_MSG_SIZE
 *                  bytes. send() blocks if full, recv() blocks if empty.
 *
 * Both are kernel-allocated. Userspace gets to them through new syscalls:
 *
 *   SYS_PIPE_CREATE   (60) -> rax = pipe id    (>=0) or -1
 *   SYS_PIPE_READ     (61)    rdi=id rsi=buf rdx=size  -> rax = bytes read
 *   SYS_PIPE_WRITE    (62)    rdi=id rsi=buf rdx=size  -> rax = bytes written
 *   SYS_PIPE_CLOSE    (63)    rdi=id
 *
 *   SYS_MQ_CREATE     (64)    rdi=msg_size                -> rax = id or -1
 *   SYS_MQ_SEND       (65)    rdi=id rsi=buf rdx=prio
 *   SYS_MQ_RECV       (66)    rdi=id rsi=buf             -> rax = bytes read
 *   SYS_MQ_CLOSE      (67)    rdi=id
 */

#define PIPE_BUF_SIZE       4096
#define MAX_PIPES           64

#define MQUEUE_MAX_MSGS     32
#define MQUEUE_MAX_MSG_SIZE 256
#define MAX_MQUEUES         32

/* Kernel-side init (call once from kernel.c). */
void ipc_init(void);

/* Kernel-callable helpers (also used by syscall layer). */
int     pipe_create(void);
int32_t pipe_read (int id, void *buf, uint32_t size);
int32_t pipe_write(int id, const void *buf, uint32_t size);
void    pipe_close(int id);

int     mq_create(uint32_t msg_size);
int32_t mq_send(int id, const void *buf, uint32_t prio);
int32_t mq_recv(int id, void *buf); /* returns bytes received, or -1 */
void    mq_close(int id);

#endif /* IPC_H */
