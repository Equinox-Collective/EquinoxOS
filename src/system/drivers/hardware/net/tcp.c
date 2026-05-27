/* tcp.c — full TCP/IP transport for EquinoxOS
 *
 * Compared to the original "happy-path" implementation this version:
 *   - tracks SND.NXT/SND.UNA/RCV.NXT properly (RFC 793 §3.3);
 *   - retransmits unacked segments via a per-socket RTX queue + RTO timer
 *     (see tcp_rtx.c);
 *   - reorders incoming OOO segments into a small per-socket reorder buffer
 *     before delivering them to the user callback;
 *   - implements the complete FIN exchange (FIN_WAIT_1, FIN_WAIT_2,
 *     CLOSE_WAIT, LAST_ACK, CLOSING, TIME_WAIT) so closes are graceful
 *     in either direction;
 *   - advertises a real receive window (currently a static TCP_RX_BUF_SIZE
 *     since data is consumed synchronously by on_data()).
 *
 * The original wget shortcut (`net_wget` / `wget_on_data` / the
 * `http_response_*` globals consumed by syscall 41) is preserved verbatim so
 * the existing `htmlview.elf` and `SYS_NET_HTTP_GET` callers keep working
 * without changes.
 */

#include "tcp.h"
#include "../../../../syslibc/stdio.h"
#include "../../../../syslibc/string.h"
#include "../../../../system/mem/memory.h"
#include "../../../misc/random.h"
#include "ipv4.h"

extern void term_print(const char *str);

/* Tick counter from src/system/misc/timer.c — 1 kHz, free-running. */
extern volatile uint32_t tick;

/* --- Forward decls from tcp_rtx.c ---------------------------------------- */
bool tcp_rtx_enqueue(tcp_socket_t *sock, uint32_t seq, uint8_t flags,
                     const uint8_t *payload, uint32_t payload_len,
                     uint32_t now_ms);
void tcp_rtx_ack(tcp_socket_t *sock, uint32_t ack_nxt);
void tcp_rtx_flush(tcp_socket_t *sock);
bool tcp_rtx_tick(net_interface_t *iface, tcp_socket_t *sock,
                  uint32_t now_ms);
bool tcp_reorder_insert(tcp_socket_t *sock, uint32_t seq, const uint8_t *data,
                        uint32_t len);
uint32_t tcp_reorder_drain(tcp_socket_t *sock);
void tcp_reorder_flush(tcp_socket_t *sock);

/* ------------------------------------------------------------------------- */
/* Socket table                                                                */
/* ------------------------------------------------------------------------- */

static tcp_socket_t tcp_sockets[TCP_MAX_SOCKETS];

static void tcp_socket_free(tcp_socket_t *sock) {
  tcp_rtx_flush(sock);
  tcp_reorder_flush(sock);
  sock->state  = TCP_CLOSED;
  sock->active = false;
  sock->on_data = NULL;
}

/* ------------------------------------------------------------------------- */
/* Checksum + low-level send                                                   */
/* ------------------------------------------------------------------------- */

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dest_ip,
                             tcp_header_t *tcp, uint8_t *payload,
                             uint32_t payload_len) {
  uint32_t sum = 0;
  uint16_t tcp_len = sizeof(tcp_header_t) + payload_len;

  uint16_t s_ip[2] = {(uint16_t)(src_ip >> 16),  (uint16_t)(src_ip & 0xFFFF)};
  uint16_t d_ip[2] = {(uint16_t)(dest_ip >> 16), (uint16_t)(dest_ip & 0xFFFF)};

  sum += HTONS(s_ip[0]);
  sum += HTONS(s_ip[1]);
  sum += HTONS(d_ip[0]);
  sum += HTONS(d_ip[1]);
  sum += HTONS(6); /* Protocol = TCP */
  sum += HTONS(tcp_len);

  uint16_t *ptr = (uint16_t *)tcp;
  for (int i = 0; i < (int)sizeof(tcp_header_t) / 2; i++) sum += ptr[i];

  uint16_t *p_ptr = (uint16_t *)payload;
  for (int i = 0; i < (int)payload_len / 2; i++) sum += p_ptr[i];
  if (payload_len % 2) sum += (uint16_t)((uint8_t *)payload)[payload_len - 1];

  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

void tcp_send_segment(net_interface_t *iface, tcp_socket_t *sock,
                      uint8_t flags, uint8_t *payload, uint32_t payload_len,
                      bool enqueue_for_retx) {
  if (!iface || !sock) return;

  uint32_t total_len = sizeof(tcp_header_t) + payload_len;
  uint8_t *buffer = kmalloc(total_len);
  if (!buffer) return;
  memset(buffer, 0, total_len);

  tcp_header_t *tcp = (tcp_header_t *)buffer;
  tcp->src_port    = HTONS(sock->local_port);
  tcp->dest_port   = HTONS(sock->remote_port);
  tcp->seq         = HTONL(sock->snd_nxt);
  tcp->ack         = HTONL(sock->rcv_nxt);
  tcp->data_offset = 0x50; /* 5 * 4 = 20 bytes (no options) */
  tcp->flags       = flags;
  tcp->window_size = HTONS(sock->rcv_wnd ? sock->rcv_wnd : 8192);

  if (payload && payload_len > 0)
    memcpy(buffer + sizeof(tcp_header_t), payload, payload_len);

  tcp->checksum = 0;
  tcp->checksum = tcp_checksum(iface->ip, sock->remote_ip, tcp, payload,
                               payload_len);

  ipv4_send_packet(iface, sock->remote_ip, 6, buffer, total_len);

  /* Enqueue *before* freeing the local buffer — the queue keeps its own copy
   * of the payload so kmalloc lifetimes are independent. */
  if (enqueue_for_retx) {
    uint32_t consumes = payload_len;
    if (flags & TCP_SYN) consumes += 1;
    if (flags & TCP_FIN) consumes += 1;

    if (consumes > 0) {
      tcp_rtx_enqueue(sock, sock->snd_nxt, flags, payload, payload_len, tick);
      sock->snd_nxt += consumes;
    }
  }

  kfree(buffer);
}

/* Back-compat wrapper. */
void tcp_send_packet(net_interface_t *iface, tcp_socket_t *sock, uint8_t flags,
                     uint8_t *payload, uint32_t payload_len) {
  tcp_send_segment(iface, sock, flags, payload, payload_len, true);
}

/* Send a pure ACK without touching snd_nxt or the RTX queue. */
static void tcp_send_pure_ack(net_interface_t *iface, tcp_socket_t *sock) {
  tcp_send_segment(iface, sock, TCP_ACK, NULL, 0, false);
}

/* ------------------------------------------------------------------------- */
/* Incoming segment handler                                                    */
/* ------------------------------------------------------------------------- */

/* Globals consumed by syscall 41 (`SYS_NET_HTTP_GET`). */
uint8_t *http_response_buf = NULL;
uint32_t http_response_len = 0;
bool     http_finished     = false;

static void wget_on_data(tcp_socket_t *sock, uint8_t *data, uint32_t len) {
  (void)sock;
  if (!http_response_buf) {
    http_response_buf = kmalloc(65536);
    if (!http_response_buf) return;
    memset(http_response_buf, 0, 65536);
  }
  if (http_response_len + len < 65536) {
    memcpy(http_response_buf + http_response_len, data, len);
    http_response_len += len;
  }
}

/* The original code peeked at last_queried_name from dns.c to fill in the
 * Host header. We preserve that behaviour. */
extern char last_queried_name[];

static void send_http_get(net_interface_t *iface, tcp_socket_t *sock) {
  char get[512];
  sprintf(get,
          "GET / HTTP/1.1\r\n"
          "Host: %s\r\n"
          "User-Agent: EquinoxBrowser/1.0\r\n"
          "Accept: text/html\r\n"
          "Connection: close\r\n\r\n",
          last_queried_name[0] ? last_queried_name : "10.0.2.2");
  tcp_send_packet(iface, sock, TCP_PSH | TCP_ACK,
                  (uint8_t *)get, (uint32_t)strlen(get));
}

void handle_tcp(net_interface_t *iface, uint8_t *packet, uint32_t ip_hdr_len) {
  ipv4_header_t *ip = (ipv4_header_t *)(packet + sizeof(ethernet_header_t));
  tcp_header_t  *th =
      (tcp_header_t *)(packet + sizeof(ethernet_header_t) + ip_hdr_len);

  uint16_t dest_port = HTONS(th->dest_port);
  uint32_t src_ip    = HTONL(ip->src_ip);

  tcp_socket_t *sock = NULL;
  for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
    if (tcp_sockets[i].active && tcp_sockets[i].local_port == dest_port &&
        tcp_sockets[i].remote_ip == src_ip) {
      sock = &tcp_sockets[i];
      break;
    }
  }

  if (!sock) {
    /* Don't spam — only log if it's not just a stray ACK to a closed port. */
    if ((th->flags & TCP_RST) == 0) {
      uint16_t src_port = HTONS(th->src_port);
      char mbuf[160];
      sprintf(mbuf,
              "[TCP] segment for unknown port %u (src %u.%u.%u.%u:%u flags=0x%x ihl=%u)\n",
              (unsigned)dest_port,
              (unsigned)((src_ip >> 24) & 0xFF),
              (unsigned)((src_ip >> 16) & 0xFF),
              (unsigned)((src_ip >> 8)  & 0xFF),
              (unsigned)( src_ip        & 0xFF),
              (unsigned)src_port,
              (unsigned)th->flags,
              (unsigned)ip_hdr_len);
      term_print(mbuf);
    }
    return;
  }

  uint32_t hdr_len    = (th->data_offset >> 4) * 4;
  uint32_t payload_len = HTONS(ip->len) - ip_hdr_len - hdr_len;
  uint32_t seq        = HTONL(th->seq);
  uint32_t ack        = HTONL(th->ack);
  uint8_t  flags      = th->flags;
  uint8_t *payload    = (uint8_t *)th + hdr_len;

  /* --- RST: tear the socket down immediately --------------------------- */
  if (flags & TCP_RST) {
    term_print("[TCP] RST received — socket closed\n");
    tcp_socket_free(sock);
    if (sock->on_data == wget_on_data) http_finished = true;
    return;
  }

  /* --- Acknowledgement processing for every state ---------------------- */
  if (flags & TCP_ACK) {
    /* Trim our retransmission queue: anything < ack is acknowledged. */
    tcp_rtx_ack(sock, ack);
    if ((int32_t)(ack - sock->snd_una) > 0) sock->snd_una = ack;
  }

  switch (sock->state) {

  /* ====================================================================
   * SYN_SENT: we sent SYN, expecting SYN+ACK.
   * ==================================================================== */
  case TCP_SYN_SENT: {
    if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
      sock->rcv_nxt = seq + 1;
      sock->state   = TCP_ESTABLISHED;
      tcp_send_pure_ack(iface, sock);
      term_print("[TCP] handshake complete -> ESTABLISHED\n");

      /* Compatibility: the original net_wget code emitted GET immediately
       * after the handshake. Preserve that path so syscall 41 still works. */
      if (sock->on_data == wget_on_data &&
          (sock->remote_port == 80 || sock->remote_port == 8080)) {
        send_http_get(iface, sock);
      }
    }
    break;
  }

  /* ====================================================================
   * ESTABLISHED — main data exchange state.
   * ==================================================================== */
  case TCP_ESTABLISHED: {
    if (payload_len > 0) {
      /* Three cases: in-order (consume + drain), past (dup, ignore), future
       * (stash in reorder buffer). */
      if (seq == sock->rcv_nxt) {
        if (sock->on_data) sock->on_data(sock, payload, payload_len);
        sock->rcv_nxt += payload_len;
        tcp_reorder_drain(sock);
        tcp_send_pure_ack(iface, sock);
      } else if ((int32_t)(seq - sock->rcv_nxt) > 0) {
        /* Future: stash and ACK with rcv_nxt unchanged (duplicate-ACK
         * semantics that nudge the peer to retransmit the gap). */
        tcp_reorder_insert(sock, seq, payload, payload_len);
        tcp_send_pure_ack(iface, sock);
      } else {
        /* Past: peer retransmitted bytes we've already accepted. Just ACK. */
        tcp_send_pure_ack(iface, sock);
      }
    }

    if (flags & TCP_FIN) {
      sock->rcv_nxt = seq + payload_len + 1;
      sock->state   = TCP_CLOSE_WAIT;
      tcp_send_pure_ack(iface, sock);
      term_print("[TCP] peer FIN -> CLOSE_WAIT\n");

      /* For the simple "request/response then done" pattern used by
       * net_wget the application is happy to close immediately. */
      sock->state = TCP_LAST_ACK;
      tcp_send_packet(iface, sock, TCP_FIN | TCP_ACK, NULL, 0);

      if (sock->on_data == wget_on_data) http_finished = true;
    }
    break;
  }

  /* ====================================================================
   * FIN_WAIT_1: we sent FIN. Wait for ACK of FIN (-> FIN_WAIT_2) or for
   * the peer's FIN to cross ours (-> CLOSING).
   * ==================================================================== */
  case TCP_FIN_WAIT_1: {
    bool our_fin_acked = ((int32_t)(ack - sock->snd_nxt) >= 0);

    if (payload_len > 0 && seq == sock->rcv_nxt) {
      if (sock->on_data) sock->on_data(sock, payload, payload_len);
      sock->rcv_nxt += payload_len;
      tcp_reorder_drain(sock);
      tcp_send_pure_ack(iface, sock);
    }

    if (flags & TCP_FIN) {
      sock->rcv_nxt = seq + payload_len + 1;
      tcp_send_pure_ack(iface, sock);

      if (our_fin_acked) {
        sock->state = TCP_TIME_WAIT;
        sock->time_wait_deadline_ms = tick + TCP_TIME_WAIT_MS;
        term_print("[TCP] FIN_WAIT_1 + FIN+ACK -> TIME_WAIT\n");
      } else {
        sock->state = TCP_CLOSING;
        term_print("[TCP] simultaneous close -> CLOSING\n");
      }
    } else if (our_fin_acked) {
      sock->state = TCP_FIN_WAIT_2;
      term_print("[TCP] our FIN acked -> FIN_WAIT_2\n");
    }
    break;
  }

  /* ====================================================================
   * FIN_WAIT_2: peer has ack'd our FIN. Wait for their FIN.
   * ==================================================================== */
  case TCP_FIN_WAIT_2: {
    if (payload_len > 0 && seq == sock->rcv_nxt) {
      if (sock->on_data) sock->on_data(sock, payload, payload_len);
      sock->rcv_nxt += payload_len;
      tcp_reorder_drain(sock);
      tcp_send_pure_ack(iface, sock);
    }
    if (flags & TCP_FIN) {
      sock->rcv_nxt = seq + payload_len + 1;
      tcp_send_pure_ack(iface, sock);
      sock->state = TCP_TIME_WAIT;
      sock->time_wait_deadline_ms = tick + TCP_TIME_WAIT_MS;
      term_print("[TCP] FIN_WAIT_2 -> TIME_WAIT\n");
    }
    break;
  }

  /* ====================================================================
   * CLOSING: both sides FIN'd simultaneously, waiting for ACK of our FIN.
   * ==================================================================== */
  case TCP_CLOSING: {
    if ((int32_t)(ack - sock->snd_nxt) >= 0) {
      sock->state = TCP_TIME_WAIT;
      sock->time_wait_deadline_ms = tick + TCP_TIME_WAIT_MS;
      term_print("[TCP] CLOSING -> TIME_WAIT\n");
    }
    break;
  }

  /* ====================================================================
   * LAST_ACK: we sent FIN in response to peer's FIN, waiting for ACK.
   * ==================================================================== */
  case TCP_LAST_ACK: {
    if ((int32_t)(ack - sock->snd_nxt) >= 0) {
      term_print("[TCP] LAST_ACK -> CLOSED\n");
      tcp_socket_free(sock);
    }
    break;
  }

  /* ====================================================================
   * TIME_WAIT: just absorb whatever comes in until the timer expires.
   * ==================================================================== */
  case TCP_TIME_WAIT:
    /* Re-ACK any retransmitted FIN from the peer. */
    if (flags & TCP_FIN) tcp_send_pure_ack(iface, sock);
    break;

  default:
    break;
  }
}

/* ------------------------------------------------------------------------- */
/* Active close + connect                                                       */
/* ------------------------------------------------------------------------- */

void tcp_close(net_interface_t *iface, tcp_socket_t *sock) {
  if (!sock || !sock->active) return;

  switch (sock->state) {
  case TCP_ESTABLISHED:
  case TCP_SYN_RECEIVED:
    sock->state = TCP_FIN_WAIT_1;
    tcp_send_packet(iface, sock, TCP_FIN | TCP_ACK, NULL, 0);
    break;
  case TCP_CLOSE_WAIT:
    sock->state = TCP_LAST_ACK;
    tcp_send_packet(iface, sock, TCP_FIN | TCP_ACK, NULL, 0);
    break;
  default:
    /* Already closing or closed — nothing to do. */
    break;
  }
}

tcp_socket_t *tcp_connect(net_interface_t *iface, uint32_t dest_ip,
                          uint16_t port, tcp_callback_t callback) {
  if (!iface) return NULL;

  for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
    tcp_socket_t *s = &tcp_sockets[i];
    if (s->active) continue;

    memset(s, 0, sizeof(*s));
    s->remote_ip   = dest_ip;
    s->remote_port = port;
    s->local_port  = 50000 + i; /* ephemeral */
    /* Random Initial Send Sequence number per RFC 6528 — burns one RDRAND
     * call (or soft fallback on TCG). Keeping the high bit clear avoids
     * an early wrap during a long-running connection: 2^31 bytes is still
     * 2 GiB before SEQ overflow, which is far past anything urlget /
     * browser pushes today. */
    uint64_t isn_seed = 0;
    rdrand_bytes(&isn_seed, sizeof isn_seed);
    s->snd_nxt     = (uint32_t)(isn_seed & 0x7FFFFFFFu);
    s->snd_una     = s->snd_nxt;
    s->rcv_nxt     = 0;
    s->rcv_wnd     = TCP_RX_BUF_SIZE;
    s->state       = TCP_SYN_SENT;
    s->on_data     = callback;
    s->active      = true;

    {
      char cbuf[128];
      sprintf(cbuf,
              "[TCP] connect: local=%u remote=%u.%u.%u.%u:%u (slot %d)\n",
              (unsigned)s->local_port,
              (unsigned)((dest_ip >> 24) & 0xFF),
              (unsigned)((dest_ip >> 16) & 0xFF),
              (unsigned)((dest_ip >> 8)  & 0xFF),
              (unsigned)( dest_ip        & 0xFF),
              (unsigned)port, i);
      term_print(cbuf);
    }
    tcp_send_packet(iface, s, TCP_SYN, NULL, 0);
    return s;
  }
  return NULL;
}

/* ------------------------------------------------------------------------- */
/* Periodic tick — called from timer_callback()                                */
/* ------------------------------------------------------------------------- */

void tcp_tick(uint32_t now_ms) {
  for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
    tcp_socket_t *s = &tcp_sockets[i];
    if (!s->active) continue;

    if (s->state == TCP_TIME_WAIT) {
      if ((int32_t)(now_ms - s->time_wait_deadline_ms) >= 0) {
        tcp_socket_free(s);
      }
      continue;
    }

    if (tcp_rtx_tick(NULL /* iface set below */, s, now_ms)) {
      /* Too many retries — locally reset the socket so the application
       * sees the failure quickly. */
      tcp_socket_free(s);
      if (s->on_data == wget_on_data) http_finished = true;
      continue;
    }
  }
}

/* The rtx_tick callback above needs the interface but we can only get it
 * lazily, so wrap. */
extern net_interface_t *net_get_primary_interface(void);

/* Weak hook into the socket layer. Defined in socket.c. tcp.c can't include
 * socket.h directly (layering — socket sits on top of tcp), so we forward-
 * declare and call. If you build a stripped kernel without socket.c, link
 * with a `void socket_on_state_change(tcp_socket_t *){}` stub. */
void socket_on_state_change(tcp_socket_t *sock);

void tcp_tick_with_iface(uint32_t now_ms) {
  net_interface_t *iface = net_get_primary_interface();
  if (!iface) return;
  for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
    tcp_socket_t *s = &tcp_sockets[i];
    if (!s->active) {
      /* Still notify on the active -> CLOSED edge so the socket layer can
       * detach its fd. After the first notification, last_notified_state is
       * TCP_CLOSED and we stop firing. */
      if (s->last_notified_state != TCP_CLOSED) {
        s->state = TCP_CLOSED;
        socket_on_state_change(s);
        s->last_notified_state = TCP_CLOSED;
      }
      continue;
    }
    if (s->state == TCP_TIME_WAIT) {
      if ((int32_t)(now_ms - s->time_wait_deadline_ms) >= 0)
        tcp_socket_free(s);
    } else if (tcp_rtx_tick(iface, s, now_ms)) {
      tcp_socket_free(s);
      if (s->on_data == wget_on_data) http_finished = true;
    }
    if (s->state != s->last_notified_state) {
      socket_on_state_change(s);
      s->last_notified_state = s->state;
    }
  }
}

/* ------------------------------------------------------------------------- */
/* Compatibility shortcut: HTTP GET (kept identical to the original API)        */
/* ------------------------------------------------------------------------- */

void net_wget(net_interface_t *iface, uint32_t dest_ip) {
  term_print("[TCP] Initiating HTTP GET (Port 80)...\n");
  /* Reset the wget response buffer state — syscall 41 also does this but it
   * cannot hurt to be defensive. */
  if (http_response_buf) {
    kfree(http_response_buf);
    http_response_buf = NULL;
  }
  http_response_len = 0;
  http_finished     = false;

  tcp_connect(iface, dest_ip, 80, wget_on_data);
}
