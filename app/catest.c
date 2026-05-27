/*
 * catest — phase 4a build/link smoke test for the Mozilla CA bundle.
 *
 * Pulls in <ca_bundle.h>, initialises a BearSSL client context against
 * `TAs_MOZ`, and prints the anchor count. No socket, no handshake — the
 * only thing being verified is that
 *
 *   (a) the bundle header compiles inside a userspace ELF (i.e. nothing
 *       in it accidentally relies on libc),
 *   (b) the linker resolves all of the per-anchor DN / RSA / EC byte
 *       arrays the table refers to,
 *   (c) the resulting binary fits in the loader's segment budget.
 *
 * If 4a is wired correctly:
 *
 *     [catest] TAs_MOZ_NUM = 121
 *     [catest] bearssl init_full bound bundle ... ok
 *     [catest] DONE
 *
 * The real internet smoke against e.g. example.com lands in phase 4c
 * once we have wall-clock time and DNS resolution lined up.
 */

#include <equos.h>
#include <bearssl.h>
#include <bearssl_io.h>
#include <stdio.h>

#include "../third_party/ca_bundle/ca_bundle.h"

/* BSS — same reasoning as tlsboot / tlstest. */
static br_ssl_client_context     g_sc;
static br_x509_minimal_context   g_xc;

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("[catest] TAs_MOZ_NUM = %u\n", (unsigned)TAs_MOZ_NUM);

    /* Pure linkage check: this call walks `TAs_MOZ` only enough to wire
     * up the x509_minimal trust-anchor table — no certificate is
     * actually parsed here, so we don't need set_time / seeding for
     * this test. */
    br_ssl_client_init_full(&g_sc, &g_xc, TAs_MOZ, TAs_MOZ_NUM);
    printf("[catest] bearssl init_full bound bundle ... ok\n");

    printf("[catest] DONE\n");
    return 0;
}
