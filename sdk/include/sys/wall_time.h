#ifndef _SYS_WALL_TIME_H
#define _SYS_WALL_TIME_H
/* ---------------------------------------------------------------------------
 * EquinoxOS userspace wall-clock API.
 *
 * Thin wrapper over SYS_GET_WALL_TIME (syscall 87). The kernel reads the
 * CMOS / MC146818 RTC behind ports 0x70/0x71 and returns the result as
 * 64-bit Unix seconds (UTC).
 *
 * This is what every TLS client in EquinoxOS should call to fill in
 * BearSSL's `br_x509_minimal_set_time(ctx, days, seconds)`. The two
 * helpers below take care of the BearSSL-specific day/second split:
 * BearSSL counts days from January 1st, *0 AD* (proleptic Gregorian),
 * which is 719528 days *before* the Unix epoch.
 *
 * Why no nanoseconds? The RTC ticks once per second on real hardware;
 * QEMU follows the same contract. Anything sub-second has to come from
 * SYS_GET_TIME (PIT ms-since-boot) and is intentionally out of scope
 * for X.509 validation, which is itself only second-granular.
 *
 * Usage:
 *
 *     #include <sys/wall_time.h>
 *
 *     uint64_t now;
 *     if (sys_get_wall_time(&now) != 0) { /\* should not happen *\/ }
 *
 *     uint32_t br_days, br_secs;
 *     unix_to_bearssl_time(now, &br_days, &br_secs);
 *     br_x509_minimal_set_time(&xc, br_days, br_secs);
 *
 * Returns 0 on success, -1 if `out` is NULL.
 * ------------------------------------------------------------------------ */

#include <stdint.h>
#include "../equos.h"

/* Days from Jan 1, 0 AD (BearSSL epoch) to Jan 1, 1970 (Unix epoch),
 * proleptic Gregorian. Verified against `datetime.date(1970,1,1)
 * .toordinal() + 365` in Python — toordinal() counts from year 1, we
 * add 366 for the proleptic year 0 (a leap year) then subtract 1 to
 * convert from 1-based ordinal to a 0-based day count. */
#define BEARSSL_UNIX_EPOCH_DAYS 719528u

static inline int sys_get_wall_time(uint64_t *out_unix_secs) {
  return (int)(int64_t)_syscall(SYS_GET_WALL_TIME,
                                (uint64_t)(uintptr_t)out_unix_secs,
                                0, 0, 0, 0);
}

/* Convert Unix seconds → the (days, seconds) pair BearSSL expects.
 * Both outputs are required (no NULL fallthrough — keep the caller
 * honest). */
static inline void unix_to_bearssl_time(uint64_t unix_secs,
                                        uint32_t *out_days,
                                        uint32_t *out_secs_of_day) {
  uint64_t d = unix_secs / 86400ull;
  uint64_t s = unix_secs % 86400ull;
  *out_days        = (uint32_t)(d + BEARSSL_UNIX_EPOCH_DAYS);
  *out_secs_of_day = (uint32_t)s;
}

#endif /* _SYS_WALL_TIME_H */
