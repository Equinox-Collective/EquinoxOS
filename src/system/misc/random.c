/* ---------------------------------------------------------------------------
 * src/system/misc/random.c — RDRAND-backed entropy.
 *
 * See random.h for the rationale; this file is just the implementation.
 *
 * Performance: RDRAND on modern silicon retires in ~100–300 cycles per
 * 64-bit word, so filling a 32-byte TLS-handshake nonce costs O(1µs).
 * We pull 8 bytes at a time and memcpy the tail; no per-byte fallback
 * loop needed.
 * ------------------------------------------------------------------------ */

#include "random.h"
#include "../../syslibc/string.h"
#include <stdint.h>

/* Intel SDM: CPUID.01h ECX bit 30 = RDRAND available. */
#define RDRAND_CPUID_BIT (1u << 30)

/* Intel recommends retrying RDRAND up to 10 times before treating a CF=0
 * result as a hardware failure. */
#define RDRAND_RETRY 10

static int   g_rdrand_inited   = 0;
static int   g_rdrand_supported = 0;

static inline void cpuid(uint32_t leaf,
                         uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(0));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

/* Try one RDRAND; returns 1 on success (CF=1) and writes into *out. */
static inline int rdrand64_step(uint64_t *out) {
    uint64_t val;
    uint8_t  ok;
    /* `rdrand %rax` sets CF on success. setc collects CF into a byte. */
    __asm__ volatile("rdrand %0; setc %1"
                     : "=r"(val), "=qm"(ok)
                     :
                     : "cc");
    *out = val;
    return ok;
}

/* Soft fallback: TSC + monotonic tick xor-mixed. NOT crypto. */
extern volatile uint32_t tick;
static inline uint64_t soft_fallback_u64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = ((uint64_t)hi << 32) | lo;
    /* Splittable64 step (Vigna) — purely to spread bits. */
    uint64_t z = tsc + (uint64_t)tick * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z =  z ^ (z >> 31);
    return z;
}

void rdrand_init(void) {
    uint32_t ecx = 0;
    cpuid(1, NULL, NULL, &ecx, NULL);
    g_rdrand_supported = (ecx & RDRAND_CPUID_BIT) ? 1 : 0;
    g_rdrand_inited    = 1;
}

int rdrand_supported(void) {
    if (!g_rdrand_inited) rdrand_init();
    return g_rdrand_supported;
}

/* Get one 64-bit chunk. Always succeeds (falls back if needed). */
static uint64_t next_u64(void) {
    if (!g_rdrand_inited) rdrand_init();

    if (g_rdrand_supported) {
        uint64_t v;
        for (int i = 0; i < RDRAND_RETRY; i++) {
            if (rdrand64_step(&v)) return v;
        }
        /* RDRAND advertised but failed 10× in a row — treat as a transient
         * power-supply glitch and fall through to the soft mixer. */
    }
    return soft_fallback_u64();
}

int rdrand_bytes(void *buf, uint32_t len) {
    if (!buf || len == 0) return 0;

    uint8_t *p = (uint8_t *)buf;
    uint32_t whole = len / 8;
    uint32_t tail  = len % 8;

    for (uint32_t i = 0; i < whole; i++) {
        uint64_t v = next_u64();
        /* memcpy instead of *(uint64_t*)p = v: p may not be 8-aligned, and
         * userspace pointers are routinely byte-aligned (sys_getrandom is
         * usually called with a char[] on the stack). */
        memcpy(p, &v, 8);
        p += 8;
    }
    if (tail) {
        uint64_t v = next_u64();
        memcpy(p, &v, tail);
    }
    return 0;
}
