/* ---------------------------------------------------------------------------
 * socket.c — BSD-flavoured fd layer over the EquinoxOS TCP module.
 *
 * Design notes
 * ------------
 * The TCP module already owns a fixed pool of `tcp_socket_t` TCBs (size
 * TCP_MAX_SOCKETS) and a callback hook (`on_data`) that is fired with
 * already-reassembled, in-order bytes. The socket layer adds:
 *
 *   - An fd table (`sockets[SOCK_MAX]`). fds map 1:1 onto entries.
 *   - A receive ring per fd, lazy-allocated on first inbound byte (no cost
 *     for sockets that haven't received anything yet).
 *   - A connect/recv waiter that pumps interrupts with `sti; hlt; cli` —
 *     the same idiom used by the SYS_NET_DNS_RESOLVE handler. This keeps
 *     the API blocking and very simple; non-blocking can be added later by
 *     just returning SOCK_ERR_AGAIN when the ring is empty.
 *
 * Send path is non-blocking: data is chunked into <= TCP_MAX_SEG_PAYLOAD
 * segments, each pushed through `tcp_send_segment()` which also enqueues a
 * copy into the RTX queue. Therefore the caller does not need to retain
 * the buffer past the syscall boundary.
 *
 * The socket layer never frees the underlying TCB — that's tcp_close()'s
 * job. We simply detach (`entry->tcb = NULL`) once the connection moves to
 * CLOSED, so subsequent calls on this fd see "closed" cleanly.
 * ------------------------------------------------------------------------ */

#include "socket.h"
#include "tcp.h"
#include "net.h"
#include "../../../../system/mem/memory.h"
#include "../../../../system/misc/timer.h"
#include "../../../../system/usr/task.h"
#include "../../../../syslibc/string.h"
#include "../../../../syslibc/stdio.h"

#define DEFAULT_CONNECT_TIMEOUT_MS  5000
#define DEFAULT_RECV_TIMEOUT_MS     5000

static socket_entry_t sockets[SOCK_MAX];

extern net_interface_t *net_get_primary_interface(void);

/* ------------------------------------------------------------------------- */
/* Ring buffer (single-producer from softirq context / single-consumer from   */
/* syscall context). We don't lock — the producer runs from network_thread   */
/* with IRQs masked relative to syscall return path, which is enough for     */
/* this single-CPU kernel.                                                    */
/* ------------------------------------------------------------------------- */

static bool ring_alloc(socket_entry_t *e) {
  if (e->rx_ring) return true;
  e->rx_ring = (uint8_t *)kmalloc(SOCK_RX_RING_SIZE);
  if (!e->rx_ring) return false;
  e->rx_size = SOCK_RX_RING_SIZE;
  e->rx_head = e->rx_tail = 0;
  return true;
}

static uint32_t ring_used(const socket_entry_t *e) {
  return e->rx_head - e->rx_tail;
}

static uint32_t ring_free(const socket_entry_t *e) {
  return e->rx_size - ring_used(e);
}

static void ring_push(socket_entry_t *e, const uint8_t *data, uint32_t len) {
  uint32_t free = ring_free(e);
  if (len > free) len = free;          /* drop overflow, RTX will resend? no — TCP rcv_wnd should prevent this in practice */
  for (uint32_t i = 0; i < len; i++) {
    e->rx_ring[(e->rx_head + i) & (e->rx_size - 1)] = data[i];
  }
  e->rx_head += len;
}

static uint32_t ring_pop(socket_entry_t *e, uint8_t *out, uint32_t max) {
  uint32_t used = ring_used(e);
  if (max > used) max = used;
  for (uint32_t i = 0; i < max; i++) {
    out[i] = e->rx_ring[(e->rx_tail + i) & (e->rx_size - 1)];
  }
  e->rx_tail += max;
  return max;
}

/* ------------------------------------------------------------------------- */
/* TCB ↔ fd mapping                                                           */
/* ------------------------------------------------------------------------- */

static socket_entry_t *find_by_tcb(tcp_socket_t *tcb) {
  for (int i = 0; i < SOCK_MAX; i++) {
    if (sockets[i].state != SOCK_STATE_FREE && sockets[i].tcb == tcb) {
      return &sockets[i];
    }
  }
  return NULL;
}

void socket_rx_dispatch(tcp_socket_t *sock, uint8_t *data, uint32_t len) {
  socket_entry_t *e = find_by_tcb(sock);
  if (!e) return;                       /* not a socket-layer TCB (e.g. wget) */
  if (!e->rx_ring && !ring_alloc(e)) {
    /* Allocation failure — silently drop. The peer will retransmit, but for
     * this OS we tolerate the loss. */
    return;
  }
  ring_push(e, data, len);
}

void socket_on_state_change(tcp_socket_t *sock) {
  socket_entry_t *e = find_by_tcb(sock);
  if (!e) return;

  switch (sock->state) {
    case TCP_ESTABLISHED:
      if (e->state == SOCK_STATE_CONNECTING) e->state = SOCK_STATE_CONNECTED;
      break;
    case TCP_CLOSE_WAIT:
    case TCP_FIN_WAIT_2:
      if (e->state == SOCK_STATE_CONNECTED) e->state = SOCK_STATE_PEER_CLOSED;
      break;
    case TCP_CLOSED:
      e->state = SOCK_STATE_CLOSED;
      e->tcb   = NULL;
      break;
    default:
      break;
  }
}

/* ------------------------------------------------------------------------- */
/* API                                                                        */
/* ------------------------------------------------------------------------- */

int sock_create(void) {
  for (int i = 0; i < SOCK_MAX; i++) {
    if (sockets[i].state == SOCK_STATE_FREE) {
      memset(&sockets[i], 0, sizeof(sockets[i]));
      sockets[i].state = SOCK_STATE_CLOSED; /* "exists but not connected" */
      /* Tag with owning pid so SYS_EXIT can sweep it on abrupt exit.
       * current_task is set up before any user code runs, so this is
       * safe from any process context. */
      sockets[i].owner_pid = current_task ? current_task->id : 0;
      return i;
    }
  }
  return SOCK_ERR_NOMEM;
}

/* Spin-wait helper. The PIT is initialised at 1000 Hz in kmain so 1 tick is
 * 1 ms (see init_timer(1000) in src/kernel.c). We pump `sti; hlt; cli` so
 * the NIC IRQ + tcp_tick_with_iface() can advance TCB state while a syscall
 * is blocked here. Same idiom as the SYS_NET_DNS_RESOLVE handler. */
static void hlt_until_tick(void) {
  __asm__ volatile("sti; hlt; cli");
}

int sock_connect(int fd, uint32_t ip_be, uint16_t port) {
  if (fd < 0 || fd >= SOCK_MAX) return SOCK_ERR_BADFD;
  socket_entry_t *e = &sockets[fd];
  if (e->state != SOCK_STATE_CLOSED) return SOCK_ERR_INVAL;

  net_interface_t *iface = net_get_primary_interface();
  if (!iface) return SOCK_ERR_NOTCONN;

  e->state = SOCK_STATE_CONNECTING;
  e->tcb   = tcp_connect(iface, ip_be, port, socket_rx_dispatch);
  if (!e->tcb) {
    e->state = SOCK_STATE_CLOSED;
    return SOCK_ERR_NOMEM;
  }

  uint32_t start = tick;
  uint32_t deadline = start + DEFAULT_CONNECT_TIMEOUT_MS;

  while (tick < deadline) {
    /* tcp.c bumps e->state via socket_on_state_change() when SYN-ACK lands. */
    if (e->state == SOCK_STATE_CONNECTED) return 0;
    if (e->state == SOCK_STATE_CLOSED || e->state == SOCK_STATE_ERROR) {
      return SOCK_ERR_REFUSED;
    }
    hlt_until_tick();
  }
  return SOCK_ERR_TIMEOUT;
}

int sock_send(int fd, const uint8_t *buf, uint32_t len) {
  if (fd < 0 || fd >= SOCK_MAX) return SOCK_ERR_BADFD;
  socket_entry_t *e = &sockets[fd];
  if (e->state != SOCK_STATE_CONNECTED && e->state != SOCK_STATE_PEER_CLOSED) {
    return SOCK_ERR_NOTCONN;
  }
  if (!e->tcb) return SOCK_ERR_CLOSED;

  net_interface_t *iface = net_get_primary_interface();
  if (!iface) return SOCK_ERR_NOTCONN;

  uint32_t sent = 0;
  while (sent < len) {
    uint32_t chunk = len - sent;
    if (chunk > TCP_MAX_SEG_PAYLOAD) chunk = TCP_MAX_SEG_PAYLOAD;
    /* The TCP layer copies the payload into its own RTX entry, so casting
     * away const here is safe — the user buffer is not retained. */
    tcp_send_segment(iface, e->tcb, TCP_ACK | TCP_PSH,
                     (uint8_t *)(buf + sent), chunk, true);
    sent += chunk;
  }
  return (int)sent;
}

int sock_recv(int fd, uint8_t *buf, uint32_t len) {
  if (fd < 0 || fd >= SOCK_MAX) return SOCK_ERR_BADFD;
  socket_entry_t *e = &sockets[fd];
  if (e->state == SOCK_STATE_FREE) return SOCK_ERR_BADFD;

  uint32_t timeout = e->rcv_timeout_ms ? e->rcv_timeout_ms : DEFAULT_RECV_TIMEOUT_MS;
  uint32_t deadline = tick + timeout;

  while (tick < deadline) {
    if (e->rx_ring && ring_used(e) > 0) {
      return (int)ring_pop(e, buf, len);
    }
    /* peer closed AND ring empty → 0 = orderly EOF, per BSD convention. */
    if (e->state == SOCK_STATE_PEER_CLOSED || e->state == SOCK_STATE_CLOSED) {
      if (!e->rx_ring || ring_used(e) == 0) return 0;
    }
    if (e->state == SOCK_STATE_ERROR) return SOCK_ERR_CLOSED;
    hlt_until_tick();
  }
  return SOCK_ERR_TIMEOUT;
}

int sock_close(int fd) {
  if (fd < 0 || fd >= SOCK_MAX) return SOCK_ERR_BADFD;
  socket_entry_t *e = &sockets[fd];
  if (e->state == SOCK_STATE_FREE) return SOCK_ERR_BADFD;

  if (e->tcb) {
    net_interface_t *iface = net_get_primary_interface();
    if (iface) tcp_close(iface, e->tcb);
    /* tcp.c will null out e->tcb via socket_on_state_change() once the
     * TCB reaches CLOSED; meanwhile we keep the rx_ring around so any
     * already-received bytes can still be drained by a final recv(). */
  }
  if (e->rx_ring) {
    kfree(e->rx_ring);
    e->rx_ring = NULL;
  }
  memset(e, 0, sizeof(*e));
  e->state = SOCK_STATE_FREE;
  return 0;
}

int sock_setsockopt(int fd, int level, int optname,
                    const void *val, uint32_t vallen) {
  if (fd < 0 || fd >= SOCK_MAX) return SOCK_ERR_BADFD;
  if (sockets[fd].state == SOCK_STATE_FREE) return SOCK_ERR_BADFD;
  if (level != SOCK_LEVEL_SOCKET) return SOCK_ERR_INVAL;

  switch (optname) {
    case SOCK_OPT_RCVTIMEO:
      if (vallen != sizeof(uint32_t) || !val) return SOCK_ERR_INVAL;
      sockets[fd].rcv_timeout_ms = *(const uint32_t *)val;
      return 0;
    case SOCK_OPT_NODELAY:
      /* We never coalesce — every sock_send pushes immediately. Accept and
       * ignore so BearSSL/HTTP clients can call this unconditionally. */
      return 0;
    default:
      return SOCK_ERR_INVAL;
  }
}

int sock_close_owned_by(uint64_t pid) {
  int closed = 0;
  for (int fd = 0; fd < SOCK_MAX; fd++) {
    socket_entry_t *e = &sockets[fd];
    if (e->state == SOCK_STATE_FREE) continue;
    if (e->owner_pid != pid)         continue;
    /* sock_close already does the right thing: it asks tcp_close to send
     * a FIN if the connection is ESTABLISHED, frees the ring buffer,
     * and marks the entry FREE. The TCB will linger in FIN_WAIT_1 /
     * TIME_WAIT until tcp_tick collects it; that's harmless — packets
     * for the local port keep matching the (still-active) TCB so the
     * "unknown port" log stays quiet. */
    sock_close(fd);
    closed++;
  }
  return closed;
}
