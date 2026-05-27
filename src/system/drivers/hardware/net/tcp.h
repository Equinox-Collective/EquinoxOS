#ifndef TCP_H
#define TCP_H

#include "net.h"
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * EquinoxOS TCP — Phase 0 hardening
 * ---------------------------------------------------------------------------
 * Goal: bring the stack from "happy path" (basic SYN→ACK→FIN flow, no
 * retransmission, no reorder) to "good enough for a TLS handshake on a
 * realistic link (loss + reordering)".
 *
 * The TCB now carries:
 *   - a small retransmission queue (RTX) of unacked outbound segments,
 *     with a per-segment RTO timer (start 500 ms, exponential backoff,
 *     max 5 retries before the connection is reset locally);
 *   - a small reorder buffer for incoming out-of-order segments;
 *   - a contiguous receive buffer that the application is fed in-order via
 *     `on_data()` once the missing segments arrive;
 *   - the full state machine: SYN_SENT, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2,
 *     CLOSE_WAIT, LAST_ACK, CLOSING, TIME_WAIT, CLOSED.
 *
 * Everything is sized to fit comfortably in the existing 64 MB kernel heap.
 * The TCB buffers are lazy-allocated in tcp_connect() and freed on close, so
 * idle sockets are cheap.
 * ------------------------------------------------------------------------ */

#define TCP_MAX_SOCKETS        32
#define TCP_RTX_QUEUE_SIZE     8     /* outbound unacked segments */
#define TCP_REORDER_BUF_SIZE   8     /* incoming OOO segments */
#define TCP_RX_BUF_SIZE        16384 /* per-socket reassembled stream */
#define TCP_MAX_SEG_PAYLOAD    1400  /* room for IP+TCP headers within MTU */

/* Initial RTO and backoff caps. PIT runs at 1000 Hz, so 1 tick == 1 ms. */
#define TCP_INITIAL_RTO_MS     500
#define TCP_MAX_RTO_MS         8000
#define TCP_MAX_RETRIES        5
/* TIME_WAIT duration — RFC says 2*MSL (~ 60 s). We use a much shorter value
 * to free socket slots quickly on this single-user OS. */
#define TCP_TIME_WAIT_MS       2000

typedef enum {
  TCP_CLOSED,
  TCP_LISTEN,
  TCP_SYN_SENT,
  TCP_SYN_RECEIVED,
  TCP_ESTABLISHED,
  TCP_FIN_WAIT_1,
  TCP_FIN_WAIT_2,
  TCP_CLOSE_WAIT,
  TCP_CLOSING,
  TCP_LAST_ACK,
  TCP_TIME_WAIT
} tcp_state_t;

typedef void (*tcp_callback_t)(uint8_t *data, uint32_t len);

/* One outstanding outbound segment held in the retransmission queue. We keep
 * a kmalloc'd copy of the original TCP header + payload so that, on timeout,
 * we can resend exactly the same bytes (RFC 793 §3.7). */
typedef struct {
  bool     in_use;
  uint32_t seq;            /* first byte sequence number */
  uint32_t payload_len;    /* payload only — header is computed at send time */
  uint8_t  flags;          /* TCP flags used for this segment              */
  uint32_t send_time_ms;   /* tick value when this segment was last sent   */
  uint32_t rto_ms;         /* current RTO (doubles on every retry)          */
  uint8_t  retries;        /* number of times we've already retransmitted   */
  uint8_t *payload;        /* kmalloc'd copy of the payload (or NULL)       */
} tcp_rtx_entry_t;

/* One out-of-order incoming segment we are holding until earlier bytes
 * arrive. We copy the payload so we can free the receive ring slot later. */
typedef struct {
  bool     in_use;
  uint32_t seq;
  uint32_t len;
  uint8_t *data;
} tcp_reorder_entry_t;

typedef struct {
  /* Identity ----------------------------------------------------------- */
  uint32_t remote_ip;
  uint16_t local_port;
  uint16_t remote_port;

  /* State machine ------------------------------------------------------ */
  tcp_state_t state;

  /* Sequence numbers --------------------------------------------------- */
  uint32_t snd_nxt;        /* next byte we'll send (SND.NXT)                */
  uint32_t snd_una;        /* oldest unacknowledged byte (SND.UNA)          */
  uint32_t rcv_nxt;        /* next byte we expect (RCV.NXT)                 */
  uint16_t rcv_wnd;        /* window we advertise to the peer               */

  /* Application callback (called only with in-order, contiguous bytes) - */
  tcp_callback_t on_data;

  /* Outbound retransmission queue ------------------------------------- */
  tcp_rtx_entry_t rtx[TCP_RTX_QUEUE_SIZE];

  /* Inbound reorder buffer -------------------------------------------- */
  tcp_reorder_entry_t reorder[TCP_REORDER_BUF_SIZE];

  /* Timers ------------------------------------------------------------- */
  uint32_t time_wait_deadline_ms; /* used only in TIME_WAIT                 */

  bool active;
} tcp_socket_t;

/* --- Public API ---------------------------------------------------------- */

/* Packet ingress from ipv4.c. */
void handle_tcp(net_interface_t *iface, uint8_t *packet, uint32_t ip_hdr_len);

/* Low-level send that wraps the header + payload in IP and emits it.
 * If `enqueue_for_retx` is true and there is payload (or SYN/FIN flag),
 * the segment is also pushed into the RTX queue so it can be retransmitted
 * on RTO. */
void tcp_send_segment(net_interface_t *iface, tcp_socket_t *sock,
                      uint8_t flags, uint8_t *payload, uint32_t payload_len,
                      bool enqueue_for_retx);

/* Wrapper that always retransmits when needed. Kept for back-compat with
 * older callers that used the original tcp_send_packet name. */
void tcp_send_packet(net_interface_t *iface, tcp_socket_t *sock, uint8_t flags,
                     uint8_t *payload, uint32_t payload_len);

/* Called by the PIT (1 kHz) — walks every TCB, checks RTOs, drains the
 * TIME_WAIT timer. Safe from IRQ context. */
void tcp_tick(uint32_t now_ms);

/* Initiate active close. Will move the socket through FIN_WAIT_1 ->
 * FIN_WAIT_2 -> TIME_WAIT. Returns immediately. */
void tcp_close(net_interface_t *iface, tcp_socket_t *sock);

/* Application-level shortcuts ---------------------------------------------- */
void net_wget(net_interface_t *iface, uint32_t dest_ip);
tcp_socket_t *tcp_connect(net_interface_t *iface, uint32_t dest_ip,
                          uint16_t port, tcp_callback_t callback);

#endif
