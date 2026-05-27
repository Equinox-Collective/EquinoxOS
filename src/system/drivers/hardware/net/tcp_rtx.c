/* tcp_rtx.c — retransmission queue + reorder buffer helpers
 *
 * These helpers live in their own translation unit so tcp.c stays readable.
 * They are intentionally lock-free (single-CPU OS) but the public functions
 * are designed to be called both from the network softirq context
 * (`network_thread`) and from the PIT tick handler. Callers that mutate the
 * RTX queue from interrupt context must ensure IRQs are disabled around the
 * mutation — `tcp_tick()` already runs with IRQs off because it is invoked
 * from `timer_callback`.
 */

#include "tcp.h"
#include "ipv4.h"
#include "../../../../syslibc/string.h"
#include "../../../../syslibc/stdio.h"
#include "../../../../system/mem/memory.h"

extern void term_print(const char *str);

/* ------------------------------------------------------------------------- */
/* Retransmission queue                                                       */
/* ------------------------------------------------------------------------- */

/* Push one segment into the RTX queue. The caller has already sent the
 * segment on the wire (or will do so right after) — this only records the
 * bytes for a potential resend. `now_ms` is the current `tick` value, used
 * to seed the RTO. Returns true on success, false if the queue is full. */
bool tcp_rtx_enqueue(tcp_socket_t *sock, uint32_t seq, uint8_t flags,
                     const uint8_t *payload, uint32_t payload_len,
                     uint32_t now_ms) {
  for (int i = 0; i < TCP_RTX_QUEUE_SIZE; i++) {
    tcp_rtx_entry_t *e = &sock->rtx[i];
    if (e->in_use) continue;

    e->in_use       = true;
    e->seq          = seq;
    e->payload_len  = payload_len;
    e->flags        = flags;
    e->send_time_ms = now_ms;
    e->rto_ms       = TCP_INITIAL_RTO_MS;
    e->retries      = 0;
    e->payload      = NULL;
    if (payload_len > 0 && payload) {
      e->payload = kmalloc(payload_len);
      if (!e->payload) {
        e->in_use = false;
        return false;
      }
      memcpy(e->payload, payload, payload_len);
    }
    return true;
  }
  /* queue full — drop. This is a denial-of-service vector in principle, but
   * in practice we only ever have a single GET in flight per socket. */
  return false;
}

/* Remove every queued segment whose last byte is <= `ack_nxt` (i.e. the
 * peer has acknowledged it). `ack_nxt` is in absolute sequence space. */
void tcp_rtx_ack(tcp_socket_t *sock, uint32_t ack_nxt) {
  for (int i = 0; i < TCP_RTX_QUEUE_SIZE; i++) {
    tcp_rtx_entry_t *e = &sock->rtx[i];
    if (!e->in_use) continue;

    /* End of the segment in sequence space. SYN and FIN each consume one
     * byte of sequence space. */
    uint32_t seg_end = e->seq + e->payload_len;
    if (e->flags & TCP_SYN) seg_end += 1;
    if (e->flags & TCP_FIN) seg_end += 1;

    /* Use modular arithmetic-safe comparison: seg_end <= ack_nxt iff
     * (ack_nxt - seg_end) fits in the lower half of a 32-bit window. */
    int32_t diff = (int32_t)(ack_nxt - seg_end);
    if (diff >= 0) {
      if (e->payload) {
        kfree(e->payload);
        e->payload = NULL;
      }
      e->in_use = false;
    }
  }
}

/* Free everything in the queue (used when the connection is torn down). */
void tcp_rtx_flush(tcp_socket_t *sock) {
  for (int i = 0; i < TCP_RTX_QUEUE_SIZE; i++) {
    tcp_rtx_entry_t *e = &sock->rtx[i];
    if (e->in_use && e->payload) kfree(e->payload);
    e->in_use = false;
    e->payload = NULL;
  }
}

/* Walk the queue once and resend any segment whose RTO has elapsed.
 * `now_ms` is the current `tick`. Returns true if the socket should be
 * killed (too many retries on any single segment). */
bool tcp_rtx_tick(net_interface_t *iface, tcp_socket_t *sock,
                  uint32_t now_ms) {
  (void)iface;
  bool kill = false;

  for (int i = 0; i < TCP_RTX_QUEUE_SIZE; i++) {
    tcp_rtx_entry_t *e = &sock->rtx[i];
    if (!e->in_use) continue;

    uint32_t elapsed = now_ms - e->send_time_ms;
    if (elapsed < e->rto_ms) continue;

    /* Pure-FIN segments (no payload, TCP_FIN flag) use a shorter retry
     * budget — see TCP_FIN_MAX_RETRIES in tcp.h. We also suppress their
     * per-retry log lines: in practice the only reason a FIN goes unacked
     * is that the host-side socket has already vanished (QEMU SLIRP after
     * userspace close), and the noise serves no debugging purpose. */
    bool is_fin_only = (e->payload_len == 0) && (e->flags & TCP_FIN);
    uint32_t retry_budget = is_fin_only ? TCP_FIN_MAX_RETRIES
                                        : TCP_MAX_RETRIES;

    if (e->retries >= retry_budget) {
      if (!is_fin_only) {
        char buf[96];
        sprintf(buf, "[TCP] RTX: giving up after %u retries (seq=%u)\n",
                (unsigned)e->retries, (unsigned)e->seq);
        term_print(buf);
      }
      kill = true;
      continue;
    }

    /* Resend. We re-emit the same segment with the same seq and flags. The
     * ACK we piggy-back is the latest rcv_nxt, which is what RFC 793 asks
     * for. */
    if (!is_fin_only) {
      char buf[128];
      sprintf(buf, "[TCP] RTX: retransmit seq=%u len=%u rto=%u retry=%u\n",
              (unsigned)e->seq, (unsigned)e->payload_len,
              (unsigned)e->rto_ms, (unsigned)e->retries + 1);
      term_print(buf);
    }

    /* Build a fresh header but with the original seq/flags/payload. */
    uint32_t old_snd_nxt = sock->snd_nxt;
    sock->snd_nxt = e->seq;
    tcp_send_segment(iface, sock, e->flags, e->payload, e->payload_len,
                     false /* don't double-enqueue */);
    sock->snd_nxt = old_snd_nxt;

    e->retries++;
    e->send_time_ms = now_ms;
    e->rto_ms = (e->rto_ms * 2 > TCP_MAX_RTO_MS) ? TCP_MAX_RTO_MS
                                                  : e->rto_ms * 2;
  }

  return kill;
}

/* ------------------------------------------------------------------------- */
/* Reorder buffer (inbound)                                                   */
/* ------------------------------------------------------------------------- */

/* Insert one segment into the reorder buffer. Duplicates (same seq, same
 * length) are silently dropped. Returns true on success. */
bool tcp_reorder_insert(tcp_socket_t *sock, uint32_t seq, const uint8_t *data,
                        uint32_t len) {
  for (int i = 0; i < TCP_REORDER_BUF_SIZE; i++) {
    tcp_reorder_entry_t *e = &sock->reorder[i];
    if (e->in_use && e->seq == seq && e->len == len) return true; /* dup */
  }

  for (int i = 0; i < TCP_REORDER_BUF_SIZE; i++) {
    tcp_reorder_entry_t *e = &sock->reorder[i];
    if (e->in_use) continue;

    e->data = kmalloc(len);
    if (!e->data) return false;
    memcpy(e->data, data, len);
    e->seq = seq;
    e->len = len;
    e->in_use = true;
    return true;
  }
  /* Buffer is full — drop and rely on the peer to retransmit when our
   * (unchanged) rcv_nxt-based ACK reaches it. */
  return false;
}

/* Drain everything that is now contiguous with `sock->rcv_nxt`, advance
 * rcv_nxt past it and call on_data() for each chunk. Returns the number of
 * bytes delivered. */
uint32_t tcp_reorder_drain(tcp_socket_t *sock) {
  uint32_t delivered = 0;
  bool progress;

  do {
    progress = false;
    for (int i = 0; i < TCP_REORDER_BUF_SIZE; i++) {
      tcp_reorder_entry_t *e = &sock->reorder[i];
      if (!e->in_use) continue;

      if (e->seq == sock->rcv_nxt) {
        if (sock->on_data) sock->on_data(sock, e->data, e->len);
        sock->rcv_nxt += e->len;
        delivered     += e->len;
        kfree(e->data);
        e->data = NULL;
        e->in_use = false;
        progress = true;
      } else {
        /* Drop fully-stale fragments that the peer is needlessly resending. */
        int32_t diff = (int32_t)(sock->rcv_nxt - (e->seq + e->len));
        if (diff >= 0) {
          kfree(e->data);
          e->data = NULL;
          e->in_use = false;
        }
      }
    }
  } while (progress);

  return delivered;
}

void tcp_reorder_flush(tcp_socket_t *sock) {
  for (int i = 0; i < TCP_REORDER_BUF_SIZE; i++) {
    tcp_reorder_entry_t *e = &sock->reorder[i];
    if (e->in_use && e->data) kfree(e->data);
    e->in_use = false;
    e->data = NULL;
  }
}
