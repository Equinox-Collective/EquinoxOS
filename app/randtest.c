/*
 * randtest - smoke test for SYS_GETRANDOM (syscall 86).
 *
 * Pulls a few different chunk sizes, prints them in hex, runs a quick
 * sanity check (no full-zero output, two consecutive 32-byte pulls
 * must differ), and exits.
 *
 * Build: socktest-style — already wired into APP_ELFS_SIMPLE in the
 *        Makefile.
 *
 * Run:   inside EquinoxOS:
 *            run bin/randtest.elf
 *
 *        expected (bytes will differ obviously):
 *            [randtest] sys_getrandom(8)   -> rc=0  bytes=2f a1 ... 9d
 *            [randtest] sys_getrandom(32)  -> rc=0  bytes=...
 *            [randtest] sys_getrandom(13)  -> rc=0  bytes=...   (odd length)
 *            [randtest] two 32-byte pulls differ: yes
 *            [randtest] all-zero check:      yes (not all zeros)
 *            [randtest] bad flag:            rc=-1 (expected)
 *            [randtest] NULL buf:            rc=-1 (expected)
 *            [randtest] DONE
 */

#include <equos.h>
#include <sys/random.h>
#include <stdio.h>
#include <string.h>

static void print_hex(const char *tag, const uint8_t *p, uint32_t n) {
    printf("%s bytes=", tag);
    for (uint32_t i = 0; i < n; i++) {
        printf("%02x", p[i]);
        if (i + 1 < n) printf(" ");
    }
    printf("\n");
}

static int all_zero(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

int main(void) {
    uint8_t a8[8];
    uint8_t a32[32];
    uint8_t b32[32];
    uint8_t a13[13];

    memset(a8, 0, sizeof a8);
    int rc = sys_getrandom(a8, sizeof a8, 0);
    printf("[randtest] sys_getrandom(8)   -> rc=%d ", rc);
    print_hex("", a8, sizeof a8);

    memset(a32, 0, sizeof a32);
    rc = sys_getrandom(a32, sizeof a32, 0);
    printf("[randtest] sys_getrandom(32)  -> rc=%d ", rc);
    print_hex("", a32, sizeof a32);

    memset(a13, 0, sizeof a13);
    rc = sys_getrandom(a13, sizeof a13, 0);
    printf("[randtest] sys_getrandom(13)  -> rc=%d ", rc);
    print_hex("", a13, sizeof a13);

    /* Two consecutive 32-byte pulls must not be identical
     * (probability of collision is 2^-256). */
    memset(b32, 0, sizeof b32);
    sys_getrandom(b32, sizeof b32, 0);
    printf("[randtest] two 32-byte pulls differ: %s\n",
           memcmp(a32, b32, sizeof a32) ? "yes" : "NO (BUG)");

    /* Output of 32 bytes can statistically never be all zeros. */
    printf("[randtest] all-zero check:      %s\n",
           all_zero(a32, sizeof a32) ? "NO (BUG)" : "yes (not all zeros)");

    /* Bad-flag path. */
    rc = sys_getrandom(a8, sizeof a8, 0xDEADBEEF);
    printf("[randtest] bad flag:            rc=%d (expected -1)\n", rc);

    /* NULL-buf path. */
    rc = sys_getrandom((void *)0, 8, 0);
    printf("[randtest] NULL buf:            rc=%d (expected -1)\n", rc);

    printf("[randtest] DONE\n");
    return 0;
}
