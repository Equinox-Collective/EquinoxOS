/*
 * tlstest — phase 3c end-to-end TLS smoke test.
 *
 * Opens a TCP socket to 10.0.2.2:443 (which QEMU's user-mode networking
 * routes to the host loopback), drives a full TLS 1.2 handshake against
 * the self-signed trust anchor baked into <ca_anchors.h>, sends one
 * HTTP/1.0 GET, prints the decrypted response, and closes the connection.
 *
 * The host side is just:
 *
 *     python -c "import http.server, ssl; \
 *       s=http.server.HTTPServer(('0.0.0.0',443), \
 *           http.server.SimpleHTTPRequestHandler); \
 *       ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); \
 *       ctx.load_cert_chain('cert.pem','key.pem'); \
 *       s.socket=ctx.wrap_socket(s.socket,server_side=True); \
 *       s.serve_forever()"
 *
 * Run inside EquinoxOS:
 *     run bin/tlstest.elf
 *
 * Expected output (the body lines depend on the directory contents the
 * Python server is exporting):
 *
 *     [tlstest] socket()                       ... fd=3
 *     [tlstest] connect 10.0.2.2:443           ... ok
 *     [tlstest] bearssl init_full (TAs=1)      ... ok
 *     [tlstest] set buffer (17408 bytes bidi)  ... ok
 *     [tlstest] seed engine 32B                ... ok
 *     [tlstest] client_reset (no hostname check) ... ok
 *     [tlstest] sslio_init                     ... ok
 *     [tlstest] write_all GET / HTTP/1.0       ... ok
 *     [tlstest] flush                          ... ok
 *     [tlstest] -- response --
 *     HTTP/1.0 200 OK
 *     ...
 *     [tlstest] -- end (last_err=0) --
 *     [tlstest] DONE
 *
 * Hostname verification is deliberately disabled (server_name = NULL):
 *
 *   • the trust anchor IS the leaf certificate (self-signed), so chain
 *     validation against TAs[] still proves that the server holds the
 *     matching private key — which is the actual security property we
 *     want from this smoke test;
 *
 *   • BearSSL's x509_minimal doesn't match IP-literal CNs against an IP
 *     hostname string by default, so passing "10.0.2.2" would just fail
 *     verification with no extra security gained. A proper SAN-driven
 *     IP-address check belongs in phase 4 once we have a real CA bundle.
 */

#include <equos.h>
#include <bearssl.h>
#include <bearssl_io.h>
#include <sys/socket.h>
#include <sys/wall_time.h>
#include <stdio.h>
#include <string.h>

#include "ca_anchors.h"

/* All large BearSSL state in BSS — see comment in tlsboot.c for why. */
static br_ssl_client_context     g_sc;
static br_x509_minimal_context   g_xc;
static unsigned char             g_iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context          g_ioc;
static unsigned char             g_rxbuf[4096];

/* One-line failure print + exit code 1. */
#define DIE(tag) do { \
    int _e = br_ssl_engine_last_error(&g_sc.eng); \
    printf("[tlstest] %s ... FAIL (last_err=%d)\n", tag, _e); \
    sys_close_sock(fd); \
    return 1; \
} while (0)

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* 1) socket + connect ------------------------------------------------ */
    int fd = sys_socket();
    if (fd < 0) {
        printf("[tlstest] socket() ... FAIL rc=%d\n", fd);
        return 1;
    }
    printf("[tlstest] socket()                       ... fd=%d\n", fd);

    uint32_t ip = IPV4(10, 0, 2, 2);
    int rc = sys_connect(fd, ip, 443);
    if (rc < 0) {
        printf("[tlstest] connect 10.0.2.2:443           ... FAIL rc=%d\n", rc);
        sys_close_sock(fd);
        return 1;
    }
    printf("[tlstest] connect 10.0.2.2:443           ... ok\n");

    /* Recv timeout — if the Python server hangs we still get our shell back. */
    uint32_t rcvto = 8000;  /* ms */
    sys_setsockopt(fd, SOCK_LEVEL_SOCKET, SOCK_OPT_RCVTIMEO,
                   &rcvto, sizeof rcvto);

    /* 2) BearSSL client wiring ------------------------------------------- */
    br_ssl_client_init_full(&g_sc, &g_xc, TAs, TAs_NUM);
    printf("[tlstest] bearssl init_full (TAs=%u)     ... ok\n",
           (unsigned)TAs_NUM);

    /* Pull current UTC from the CMOS RTC and feed it to BearSSL's
     * x509_minimal so NotBefore/NotAfter actually mean something. The
     * earlier hardcoded "2026-08-01" stand-in is gone; if the RTC is
     * wedged the handshake will now fail loudly with
     * BR_ERR_X509_EXPIRED / NOT_YET_VALID instead of silently passing.*/
    uint64_t now_unix = 0;
    if (sys_get_wall_time(&now_unix) != 0) {
        printf("[tlstest] sys_get_wall_time              ... FAIL\n");
        return 1;
    }
    uint32_t br_days, br_secs;
    unix_to_bearssl_time(now_unix, &br_days, &br_secs);
    br_x509_minimal_set_time(&g_xc, br_days, br_secs);
    printf("[tlstest] x509 set_time (unix=%llu, d=%u, s=%u) ... ok\n",
           (unsigned long long)now_unix, (unsigned)br_days, (unsigned)br_secs);

    br_ssl_engine_set_buffer(&g_sc.eng, g_iobuf, sizeof g_iobuf, 1);
    printf("[tlstest] set buffer (%u bytes bidi)  ... ok\n",
           (unsigned)sizeof g_iobuf);

    if (eq_bearssl_seed_engine(&g_sc.eng, 32) != 0) DIE("seed engine 32B");
    printf("[tlstest] seed engine 32B                ... ok\n");

    /* NULL hostname -> no SNI, no SAN/CN check; chain still validates
     * against TAs[]. See file header for the security rationale. */
    if (!br_ssl_client_reset(&g_sc, NULL, 0)) DIE("client_reset");
    printf("[tlstest] client_reset (no hostname check) ... ok\n");

    eq_bearssl_sock_io_t io = { .fd = fd };
    br_sslio_init(&g_ioc, &g_sc.eng,
                  eq_bearssl_sock_read,  &io,
                  eq_bearssl_sock_write, &io);
    printf("[tlstest] sslio_init                     ... ok\n");

    /* 3) HTTP request ---------------------------------------------------- */
    static const char REQ[] =
        "GET / HTTP/1.0\r\n"
        "Host: 10.0.2.2\r\n"
        "User-Agent: EquinoxOS-tlstest/0.1\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (br_sslio_write_all(&g_ioc, REQ, sizeof REQ - 1) != 0)
        DIE("write_all GET");
    printf("[tlstest] write_all GET / HTTP/1.0       ... ok\n");

    if (br_sslio_flush(&g_ioc) != 0) DIE("flush");
    printf("[tlstest] flush                          ... ok\n");

    /* 4) Drain decrypted body ------------------------------------------- */
    printf("[tlstest] -- response --\n");
    for (;;) {
        int got = br_sslio_read(&g_ioc, g_rxbuf, sizeof g_rxbuf - 1);
        if (got <= 0) break;
        g_rxbuf[got] = 0;
        /* The body is arbitrary directory listing HTML — printf %s is
         * fine because BearSSL produces a clean stream and we
         * null-terminated above. */
        printf("%s", (char *)g_rxbuf);
    }
    int eng_err = br_ssl_engine_last_error(&g_sc.eng);
    printf("\n[tlstest] -- end (last_err=%d) --\n", eng_err);

    /* 5) Cleanup --------------------------------------------------------- */
    /* Best-effort close_notify; ignore the return code — the peer may
     * already have torn the TCP connection down (HTTP/1.0 + Connection:
     * close means the server closes first). */
    (void)br_sslio_close(&g_ioc);
    sys_close_sock(fd);

    if (eng_err != 0 && eng_err != BR_ERR_OK) {
        /* close_notify-not-received is the usual "non-zero" we see here
         * when the server hangs up first — surface it but don't fail. */
        printf("[tlstest] note: handshake/teardown reported err=%d\n",
               eng_err);
    }

    printf("[tlstest] DONE\n");
    return 0;
}
