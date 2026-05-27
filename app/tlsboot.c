/*
 * tlsboot — phase 3b sanity check.
 *
 * Boots a BearSSL client context end-to-end *without* opening a network
 * connection. Confirms that:
 *
 *   1. libbearssl.a links cleanly into a userspace ELF (phase 3a integration).
 *   2. <bearssl_io.h> compiles and exposes the read/write/seed helpers.
 *   3. SYS_GETRANDOM seeds the BearSSL PRNG without faulting.
 *   4. br_ssl_client_init_full + engine_set_buffer + client_reset all run
 *      to completion — i.e. nothing in BearSSL touches libc symbols we
 *      don't have, and the static state fits in the loader's BSS budget.
 *
 * No socket, no handshake, no certs. Phase 3c (tlstest.elf) plugs the
 * shim into a real https://10.0.2.2/ connection.
 *
 * Run inside EquinoxOS:
 *     run bin/tlsboot.elf
 *
 * Expected output (one screen):
 *
 *     [tlsboot] bearssl client init ... ok
 *     [tlsboot] set buffer (16 KiB bidi) ... ok
 *     [tlsboot] seed engine 32B from sys_getrandom ... ok
 *     [tlsboot] client_reset ("example.com") ... state=0x0042 (handshake)
 *     [tlsboot] sslio_init bound to fake fd ... ok
 *     [tlsboot] DONE
 *
 * State value after client_reset is BR_SSL_SENDREC|BR_SSL_RECVAPP_etc —
 * we don't decode it, the point is just "non-zero, non-error".
 */

#include <equos.h>
#include <bearssl.h>
#include <bearssl_io.h>
#include <stdio.h>
#include <string.h>

/* All large state goes in BSS — these structs are ~12 KB combined and the
 * iobuf is another ~17 KB, which would blow a small kernel-managed user
 * stack. The loader zero-fills .bss for us (see crt0.asm). */
static br_ssl_client_context     g_sc;
static br_x509_minimal_context   g_xc;
static unsigned char             g_iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context          g_ioc;

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* 1) init_full with zero trust anchors. We're not going to actually
     *    validate a peer here — phase 3c will plug in a real anchor list.
     *    BearSSL accepts NULL,0 and simply rejects any chain at handshake
     *    time, which is exactly what we want for a no-network smoke test. */
    br_ssl_client_init_full(&g_sc, &g_xc, NULL, 0);
    printf("[tlsboot] bearssl client init ... ok\n");

    /* 2) Engine buffer. The "1" means full-duplex (BIDI) — sized via the
     *    BR_SSL_BUFSIZE_BIDI macro from bearssl_ssl.h (~17 KiB). */
    br_ssl_engine_set_buffer(&g_sc.eng, g_iobuf, sizeof g_iobuf, 1);
    printf("[tlsboot] set buffer (%u bytes bidi) ... ok\n",
           (unsigned)sizeof g_iobuf);

    /* 3) Seed PRNG via our shim. */
    int seeded = eq_bearssl_seed_engine(&g_sc.eng, 32);
    printf("[tlsboot] seed engine 32B from sys_getrandom ... %s\n",
           seeded == 0 ? "ok" : "FAIL");
    if (seeded != 0) {
        printf("[tlsboot] ABORT — seeding failed\n");
        return 1;
    }

    /* 4) client_reset arms the state machine; with no actual transport
     *    nothing leaves the box, but the engine should transition out of
     *    INIT and into the BR_SSL_SENDREC region (waiting to push the
     *    ClientHello). */
    br_ssl_client_reset(&g_sc, "example.com", 0);
    unsigned state = br_ssl_engine_current_state(&g_sc.eng);
    printf("[tlsboot] client_reset (\"example.com\") ... state=0x%x\n",
           state);
    if (state == 0 || state == BR_SSL_CLOSED) {
        printf("[tlsboot] ABORT — engine not in handshake state (err=%d)\n",
               br_ssl_engine_last_error(&g_sc.eng));
        return 1;
    }

    /* 5) Bind the sslio facade to a dummy fd. We don't *call* read/write
     *    so the fd is never touched — this just verifies the function-pointer
     *    plumbing compiles and links. */
    eq_bearssl_sock_io_t io = { .fd = -1 };
    br_sslio_init(&g_ioc, &g_sc.eng,
                  eq_bearssl_sock_read,  &io,
                  eq_bearssl_sock_write, &io);
    printf("[tlsboot] sslio_init bound to fake fd ... ok\n");

    printf("[tlsboot] DONE\n");
    return 0;
}
