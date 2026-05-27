#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H
/* ---------------------------------------------------------------------------
 * EquinoxOS userspace entropy API.
 *
 * Thin wrapper over SYS_GETRANDOM (syscall 86). The kernel pulls bytes from
 * the on-CPU RDRAND DRNG (Intel SecureKey / AMD equivalent) and copies them
 * straight into your buffer. No partial fills.
 *
 * Usage:
 *
 *     #include <sys/random.h>
 *
 *     uint8_t nonce[32];
 *     if (sys_getrandom(nonce, sizeof nonce, 0) != 0) {
 *         // bad pointer or unsupported flag — should not happen at runtime
 *     }
 *
 * `flags` is reserved (must be 0). It exists so we can extend later with
 * e.g. GRND_NONBLOCK semantics if we ever add a userspace-visible CSPRNG
 * pool on top.
 *
 * Returns 0 on success, -1 on bad args.
 * ------------------------------------------------------------------------ */

#include <stdint.h>
#include "../equos.h"

static inline int sys_getrandom(void *buf, uint32_t len, uint32_t flags) {
  return (int)(int64_t)_syscall(SYS_GETRANDOM,
                                (uint64_t)(uintptr_t)buf,
                                (uint64_t)len,
                                (uint64_t)flags, 0, 0);
}

#endif /* _SYS_RANDOM_H */
