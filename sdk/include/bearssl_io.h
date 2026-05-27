#ifndef _EQUOS_BEARSSL_IO_H
#define _EQUOS_BEARSSL_IO_H
/* ---------------------------------------------------------------------------
 * EquinoxOS ↔ BearSSL glue layer (phase 3b).
 *
 * Two responsibilities:
 *
 *   1. Provide the read / write callbacks that BearSSL's br_sslio_* API
 *      expects, layered over our blocking socket syscalls (SYS_RECV /
 *      SYS_SEND from phase 1).
 *
 *   2. Seed a freshly-initialised br_ssl_engine_context with bytes pulled
 *      via SYS_GETRANDOM (RDRAND-backed, phase 2). This is the only way
 *      BearSSL gets entropy on EquinoxOS — we deliberately did NOT enable
 *      BearSSL's own BR_USE_URANDOM / BR_USE_WIN32_RAND / BR_RDRAND
 *      seeders during phase 3a (see third_party/bearssl/README.equos.md
 *      and the BEARSSL_CFLAGS in Makefile).
 *
 * Header-only: implementation lives in static inline functions so there is
 * no separate .c file dragged into every SDK consumer. Apps that need TLS
 * #include <bearssl.h> AND #include <bearssl_io.h>, then link with
 * libbearssl.a (see app/tlsboot rule in the Makefile).
 *
 * Usage sketch (full handshake lands in phase 3c via tlstest.elf):
 *
 *     static br_ssl_client_context     sc;
 *     static br_x509_minimal_context   xc;
 *     static unsigned char             iobuf[BR_SSL_BUFSIZE_BIDI];
 *     static br_sslio_context          ioc;
 *
 *     int fd = sys_socket();
 *     sys_connect(fd, ip, 443);
 *
 *     br_ssl_client_init_full(&sc, &xc, MY_TAS, MY_TAS_NUM);
 *     br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
 *     eq_bearssl_seed_engine(&sc.eng, 32);
 *     br_ssl_client_reset(&sc, "example.com", 0);
 *
 *     eq_bearssl_sock_io_t io = { .fd = fd };
 *     br_sslio_init(&ioc, &sc.eng,
 *                   eq_bearssl_sock_read,  &io,
 *                   eq_bearssl_sock_write, &io);
 *     ...
 *     sys_close_sock(fd);
 * ------------------------------------------------------------------------ */

#include <stdint.h>
#include <stddef.h>
#include <bearssl.h>
#include <sys/socket.h>
#include <sys/random.h>

/* Context passed to the read/write callbacks. One per TLS session.
 * The fd MUST already be connected (sys_connect returned 0) before any
 * br_sslio_* call. We don't carry buffers here — BearSSL does its own
 * record-layer buffering on top of these callbacks. */
typedef struct {
    int fd;
} eq_bearssl_sock_io_t;

/* --- read callback --------------------------------------------------------
 * BearSSL contract (see bearssl_ssl.h, br_sslio_init docs):
 *   > 0  : bytes read into `data` (may be < len; partial read is fine).
 *   < 0  : transport failure; BearSSL aborts the connection.
 *   = 0  : BearSSL takes 0 as "retry" — but our recv() returns 0 only on
 *          clean peer-close, which mid-record means the TCP layer dropped
 *          the connection before TLS close_notify. Map that to -1.
 *
 * `len` is whatever BearSSL feels like asking for, usually one TLS record
 * (~16 KiB max). Our kernel recv handles partial reads internally, so we
 * just pass through whatever it returns.                                   */
static inline int
eq_bearssl_sock_read(void *ctx, unsigned char *data, size_t len)
{
    eq_bearssl_sock_io_t *io = (eq_bearssl_sock_io_t *)ctx;
    if (!io || io->fd < 0 || !data || len == 0) return -1;
    /* Cap to int32 — sys_recv takes uint32_t. BearSSL never asks for more
     * than one TLS record (~16 KiB) so this is purely defensive. */
    if (len > 0x7fffffffu) len = 0x7fffffffu;
    int rc = sys_recv(io->fd, data, (uint32_t)len);
    if (rc <= 0) return -1;   /* 0 = peer EOF mid-record => protocol error */
    return rc;
}

/* --- write callback -------------------------------------------------------
 * Same contract, mirrored. Our sys_send may return a short count if the
 * TCP TX window is small — that's OK, br_sslio loops until BearSSL's
 * record buffer is drained.                                                */
static inline int
eq_bearssl_sock_write(void *ctx, const unsigned char *data, size_t len)
{
    eq_bearssl_sock_io_t *io = (eq_bearssl_sock_io_t *)ctx;
    if (!io || io->fd < 0 || !data || len == 0) return -1;
    if (len > 0x7fffffffu) len = 0x7fffffffu;
    int rc = sys_send(io->fd, data, (uint32_t)len);
    if (rc <= 0) return -1;
    return rc;
}

/* --- seeder ---------------------------------------------------------------
 * Pull `n` fresh bytes from SYS_GETRANDOM and feed them to BearSSL's PRNG
 * via br_ssl_engine_inject_entropy. Call this exactly once between
 * br_ssl_client_init_full() (or _set_buffer) and br_ssl_client_reset() —
 * before the first handshake byte.
 *
 * 32 bytes (256 bits) is plenty for ChaCha20-based PRNG state; BearSSL
 * stretches internally. We cap n at 64 to avoid blowing the stack buffer.
 *
 * Returns 0 on success, -1 if the syscall failed or args are bad.          */
static inline int
eq_bearssl_seed_engine(br_ssl_engine_context *eng, size_t n)
{
    unsigned char seed[64];
    if (!eng || n == 0 || n > sizeof seed) return -1;
    if (sys_getrandom(seed, (uint32_t)n, 0) != 0) return -1;
    br_ssl_engine_inject_entropy(eng, seed, n);
    /* Best-effort wipe — we don't have explicit_bzero on freestanding. */
    for (size_t i = 0; i < sizeof seed; i++) seed[i] = 0;
    return 0;
}

#endif /* _EQUOS_BEARSSL_IO_H */
