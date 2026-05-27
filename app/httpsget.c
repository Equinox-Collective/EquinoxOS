/* ---------------------------------------------------------------------------
 * app/httpsget.c — end-to-end HTTPS fetch against a real CA-signed host.
 *
 * Phase 4c of the browser stack. Builds on:
 *
 *   - phase 0/1 TCP + socket fd layer            (sys_socket / connect / ...)
 *   - phase 2   SYS_GETRANDOM                    (engine seeding)
 *   - phase 3   vendored BearSSL + bearssl_io.h
 *   - phase 4a  Mozilla CCADB bundle (TAs_MOZ)
 *   - phase 4b  CMOS RTC → SYS_GET_WALL_TIME
 *
 * What's *new* in this app vs tlstest.elf:
 *
 *   1. DNS instead of hardcoded 10.0.2.2 — calls SYS_NET_DNS_RESOLVE so we
 *      can talk to the real internet through the QEMU SLIRP NAT.
 *   2. Trust anchors come from `TAs_MOZ` (121 CAs as of CCADB May 2026),
 *      not the single self-signed dev anchor in app/ca_anchors.h.
 *   3. *Hostname verification is ON*: br_ssl_client_reset() is called with
 *      the actual hostname string, so a chain with the wrong SAN will be
 *      rejected with BR_ERR_X509_BAD_SERVER_NAME (62). Until this commit
 *      we always passed NULL there.
 *   4. The HTTP request carries a `Host:` header — mandatory for almost
 *      every real-internet host (HTTP/1.0 + Host: works fine for what we
 *      need; saves us from juggling HTTP/1.1 chunked teardown).
 *
 * Usage from the EquinoxOS shell:
 *
 *     run bin/httpsget.elf                       # defaults to example.com
 *     run bin/httpsget.elf wttr.in
 *     run bin/httpsget.elf www.google.com
 *
 * The host string is fed verbatim to DNS *and* to BearSSL as the SNI /
 * hostname-verification target, so they're guaranteed to match.
 *
 * What this app explicitly does NOT do (yet):
 *
 *   - No redirect following. 301/302 prints and exits.
 *   - No keep-alive / connection reuse. One handshake per fetch.
 *   - No proxy / CONNECT support.
 *   - No streaming render — body is printed straight to the serial term.
 *
 * Memory footprint: ~33 KiB of TLS I/O buffer + the static BearSSL client
 * + minimal context. The Mozilla TA array lives in .rodata of the ELF.
 * ------------------------------------------------------------------------ */

#include <equos.h>
#include <bearssl.h>
#include <bearssl_io.h>
#include <sys/socket.h>
#include <sys/wall_time.h>
#include <stdio.h>
#include <string.h>

#include "../third_party/ca_bundle/ca_bundle.h"

/* TLS state lives in .bss so we don't blow the stack — same convention as
 * tlstest.elf. The 33 KiB bidirectional buffer is sized off BearSSL's
 * BR_SSL_BUFSIZE_BIDI macro (= 16920 bytes per direction). */
static br_ssl_client_context   g_sc;
static br_x509_minimal_context g_xc;
static unsigned char           g_iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context        g_ioc;

/* HTTP request scratch — generous enough for the hostname + a couple of
 * headers; 512 bytes is overkill but reads better than counting. */
static char     g_req[512];
/* Single-pass response buffer. 8 KiB covers example.com's ~1.3 KiB body
 * and a typical wttr.in plain-text response. Anything bigger is fine —
 * we stream-print and overwrite. */
static unsigned char g_resp[8192];

/* Pretty-print an IPv4 stored in network byte order (high byte = a.b.c.d
 * leftmost). */
static void print_ipv4_be(uint32_t ip_be) {
    printf("%u.%u.%u.%u",
           (unsigned)((ip_be >> 24) & 0xFF),
           (unsigned)((ip_be >> 16) & 0xFF),
           (unsigned)((ip_be >>  8) & 0xFF),
           (unsigned)( ip_be        & 0xFF));
}

int main(int argc, char **argv) {
    const char *host = (argc >= 2) ? argv[1] : "example.com";

    printf("[httpsget] host = %s\n", host);

    /* --- DNS ------------------------------------------------------------ */
    uint32_t ip_be = net_dns_resolve(host);
    if (ip_be == 0) {
        printf("[httpsget] DNS                            ... FAIL\n");
        return 1;
    }
    printf("[httpsget] DNS                            ... ");
    print_ipv4_be(ip_be);
    printf("\n");

    /* --- TCP ------------------------------------------------------------ */
    int s = sys_socket();
    if (s < 0) {
        printf("[httpsget] socket()                       ... FAIL (%d)\n", s);
        return 1;
    }
    printf("[httpsget] socket()                       ... fd=%d\n", s);

    /* 20 s receive timeout — generous for the round-trip through SLIRP +
     * the public internet; will let us notice a stalled response without
     * hanging the shell forever. */
    uint32_t rcvto_ms = 20000;
    sys_setsockopt(s, SOCK_LEVEL_SOCKET, SOCK_OPT_RCVTIMEO,
                   &rcvto_ms, sizeof rcvto_ms);

    if (sys_connect(s, ip_be, 443) < 0) {
        printf("[httpsget] connect :443                   ... FAIL\n");
        sys_close_sock(s);
        return 1;
    }
    printf("[httpsget] connect :443                   ... ok\n");

    /* --- TLS ------------------------------------------------------------ */
    br_ssl_client_init_full(&g_sc, &g_xc, TAs_MOZ, TAs_MOZ_NUM);
    printf("[httpsget] bearssl init_full (TAs=%u)     ... ok\n",
           (unsigned)TAs_MOZ_NUM);

    br_ssl_engine_set_buffer(&g_sc.eng, g_iobuf, sizeof g_iobuf, 1);
    printf("[httpsget] set buffer (%u bytes bidi)     ... ok\n",
           (unsigned)sizeof g_iobuf);

    /* Pull real UTC from the CMOS RTC and feed it to the X.509 validator.
     * Without this every Mozilla-signed cert is rejected as expired/
     * not-yet-valid. */
    uint64_t now_unix = 0;
    if (sys_get_wall_time(&now_unix) != 0) {
        printf("[httpsget] sys_get_wall_time              ... FAIL\n");
        sys_close_sock(s);
        return 1;
    }
    uint32_t br_days, br_secs;
    unix_to_bearssl_time(now_unix, &br_days, &br_secs);
    br_x509_minimal_set_time(&g_xc, br_days, br_secs);
    printf("[httpsget] x509 set_time (unix=%llu, d=%u, s=%u) ... ok\n",
           (unsigned long long)now_unix, (unsigned)br_days, (unsigned)br_secs);

    /* Seed the engine PRNG with 32 bytes from RDRAND (phase 2). */
    eq_bearssl_seed_engine(&g_sc.eng, 32);
    printf("[httpsget] seed engine 32B                ... ok\n");

    /* *** SAN / hostname verification ON. *** This is the line that makes
     * 4c meaningful — pass the real hostname instead of NULL so BearSSL
     * cross-checks dNSName/iPAddress entries in the leaf cert. */
    br_ssl_client_reset(&g_sc, host, 0);
    printf("[httpsget] client_reset (SNI=%s)\n", host);

    eq_bearssl_sock_io_t io = { .fd = s };
    br_sslio_init(&g_ioc, &g_sc.eng,
                  eq_bearssl_sock_read,  &io,
                  eq_bearssl_sock_write, &io);
    printf("[httpsget] sslio_init                     ... ok\n");

    /* --- HTTP/1.0 GET --------------------------------------------------- */
    int req_len = snprintf(g_req, sizeof g_req,
                           "GET / HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "User-Agent: EquinoxOS/httpsget\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           host);
    if (req_len <= 0 || (size_t)req_len >= sizeof g_req) {
        printf("[httpsget] request build                  ... FAIL\n");
        sys_close_sock(s);
        return 1;
    }

    if (br_sslio_write_all(&g_ioc, g_req, (size_t)req_len) < 0) {
        int err = br_ssl_engine_last_error(&g_sc.eng);
        printf("[httpsget] write_all (err=%d)             ... FAIL\n", err);
        sys_close_sock(s);
        return 1;
    }
    if (br_sslio_flush(&g_ioc) < 0) {
        int err = br_ssl_engine_last_error(&g_sc.eng);
        printf("[httpsget] flush (err=%d)                 ... FAIL\n", err);
        sys_close_sock(s);
        return 1;
    }
    printf("[httpsget] GET sent (%d bytes)            ... ok\n", req_len);

    /* --- Response ------------------------------------------------------- */
    printf("[httpsget] -- response --\n");
    size_t total = 0;
    for (;;) {
        int n = br_sslio_read(&g_ioc, g_resp, sizeof g_resp);
        if (n < 0) {
            break; /* clean close_notify or transport error */
        }
        if (n == 0) {
            continue;
        }
        /* Stream straight to the terminal. printf would barf on
         * arbitrary bytes, so use the explicit-length path. */
        for (int i = 0; i < n; i++) {
            putchar((int)g_resp[i]);
        }
        total += (size_t)n;
    }
    printf("\n[httpsget] -- end (bytes=%u, last_err=%d) --\n",
           (unsigned)total,
           br_ssl_engine_last_error(&g_sc.eng));

    sys_close_sock(s);
    printf("[httpsget] DONE\n");
    return 0;
}
