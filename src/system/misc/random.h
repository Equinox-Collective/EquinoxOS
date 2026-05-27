#ifndef RANDOM_H
#define RANDOM_H
/* ---------------------------------------------------------------------------
 * Kernel-side entropy source.
 *
 * Uses the x86 RDRAND instruction (Intel SecureKey, present on Ivy Bridge
 * and later; AMD added it on Excavator). The instruction internally feeds a
 * hardware noise source through an AES-CBC-MAC conditioner — output is
 * suitable as a CSPRNG seed on its own.
 *
 * RDRAND is allowed to fail (CF=0). Intel's manual recommends retrying up
 * to ten times before giving up; we follow that.
 *
 * On CPUs that do not advertise RDRAND in CPUID.01h:ECX bit 30 the helper
 * falls back to a small mix of TSC + tick counter. That fallback is NOT
 * cryptographically strong and is only there so kernel-side bring-up does
 * not hard-fault on ancient QEMU TCG configurations — production builds
 * should be running on whpx/kvm where RDRAND is exposed.
 *
 * Return value of rdrand_bytes:
 *   0  on success (buf fully filled),
 *  -1  if the CPU has no RDRAND AND the caller refuses the soft fallback
 *      (currently never returned — soft fallback is unconditional, see
 *      comment above; reserved for the future hard-mode flag).
 * ------------------------------------------------------------------------ */

#include <stdint.h>
#include <stddef.h>

/* Initialise: cache the CPUID feature bit. Safe to call from arch init
 * after CPUID is otherwise usable. If never called, rdrand_bytes() will
 * lazy-init on first use. */
void rdrand_init(void);

/* Returns 1 if the CPU advertises RDRAND, 0 otherwise. */
int rdrand_supported(void);

/* Fill `buf` with `len` bytes of entropy. Returns 0 on success.
 * Never partial — either the whole buffer is written or rdrand_bytes
 * keeps spinning on RDRAND until it succeeds. */
int rdrand_bytes(void *buf, uint32_t len);

#endif /* RANDOM_H */
