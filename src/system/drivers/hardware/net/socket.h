#ifndef SOCKET_H
#define SOCKET_H

#include "tcp.h"
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * EquinoxOS socket layer — Phase 1
 * ---------------------------------------------------------------------------
 * Wraps the existing TCP machinery in a small, BSD-flavoured fd-based API:
 *
 *   int  sock_create(void);
 *   int  sock_connect(int fd, uint32_t ip_be, uint16_t port);
 *   int  sock_send   (int fd, const uint8_t *buf, uint32_t len);
 *   int  sock_recv   (int fd, uint8_t       *buf, uint32_t len);
 *   int  sock_close  (int fd);
 *   int  sock_setsockopt(int fd, int level, int optname,
 *                        const void *val, uint32_t vallen);
 *
 * Why this exists: the wget shortcut (syscall 41) is a one-shot HTTP GET,
 * hard-wired to net_wget()/wget_on_data(). A TLS handshake on top of TCP
 * requires arbitrary read/write, which means the application needs its own
 * receive ring and a way to push outbound bytes without leaving the kernel
 * world. The socket layer below owns one ring per fd (lazy-allocated) and
 * routes incoming bytes via a per-TCB callback into the matching ring.
 *
 * Errors are negative ints returned in-band. The userspace SDK header
 * (sdk/include/sys/socket.h) defines symbolic constants for the few we care
 * about — most callers just check `rc < 0`.
 * ------------------------------------------------------------------------ */

#define SOCK_MAX           16
#define SOCK_RX_RING_SIZE  32768  /* per-socket reassembled inbound stream */

#define SOCK_ERR_BADFD       -1
#define SOCK_ERR_NOMEM       -2
#define SOCK_ERR_NOTCONN     -3
#define SOCK_ERR_TIMEOUT     -4
#define SOCK_ERR_REFUSED     -5
#define SOCK_ERR_CLOSED      -6
#define SOCK_ERR_AGAIN       -7
#define SOCK_ERR_INVAL       -8

/* setsockopt levels / names. Today we only honour SO_RCVTIMEO; everything
 * else is accepted and ignored so a future BearSSL port can call setsockopt
 * unconditionally without #ifdef gymnastics. */
#define SOCK_LEVEL_SOCKET   1
#define SOCK_OPT_RCVTIMEO   1     /* value = uint32_t ms                */
#define SOCK_OPT_NODELAY    2     /* accepted, ignored (we don't Nagle) */

typedef enum {
  SOCK_STATE_FREE = 0,
  SOCK_STATE_CONNECTING,   /* SYN sent, awaiting ESTABLISHED */
  SOCK_STATE_CONNECTED,
  SOCK_STATE_PEER_CLOSED,  /* peer FIN seen — still drainable */
  SOCK_STATE_CLOSED,
  SOCK_STATE_ERROR
} sock_state_t;

typedef struct {
  sock_state_t   state;
  tcp_socket_t  *tcb;           /* NULL until sock_connect()                 */
  uint8_t       *rx_ring;       /* lazy-allocated on first incoming byte     */
  uint32_t       rx_head;       /* producer (incoming TCP -> ring)           */
  uint32_t       rx_tail;       /* consumer (sock_recv -> userspace)         */
  uint32_t       rx_size;       /* current ring capacity                     */
  uint32_t       rcv_timeout_ms;/* 0 = use default                           */
  int32_t        err;
  uint64_t       owner_pid;     /* current_task->id at sock_create time;
                                   used by sock_close_owned_by(pid) to
                                   reap leaked fds on SYS_EXIT.            */
} socket_entry_t;

/* fd table API ------------------------------------------------------------- */

int sock_create(void);
int sock_connect(int fd, uint32_t ip_be, uint16_t port);
int sock_send(int fd, const uint8_t *buf, uint32_t len);
int sock_recv(int fd, uint8_t *buf, uint32_t len);
int sock_close(int fd);
int sock_setsockopt(int fd, int level, int optname,
                    const void *val, uint32_t vallen);

/* Reap every fd whose owner_pid matches `pid`. Called from SYS_EXIT so
 * a crashing or exiting process doesn't leave dangling TCBs behind that
 * would later log "[TCP] segment for unknown port …" on every
 * retransmit from the peer. Safe to call with no sockets owned —
 * returns the count actually closed. */
int sock_close_owned_by(uint64_t pid);

/* Internal: callback wired into tcp_connect() so the TCP layer can deliver
 * in-order, reassembled bytes for a particular TCB. Looked up by tcb ptr. */
void socket_rx_dispatch(tcp_socket_t *sock, uint8_t *data, uint32_t len);

/* Internal: invoked from tcp.c whenever a TCB state transition happens that
 * a socket cares about. Today: ESTABLISHED, CLOSE_WAIT, CLOSED, refused. */
void socket_on_state_change(tcp_socket_t *sock);

#endif
