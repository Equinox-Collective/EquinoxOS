/* sdk/lib/qjs_time.c — gettimeofday() / clock_gettime() for QuickJS.
 *
 * EquinoxOS doesn't ship a real POSIX time stack; QuickJS only needs
 * two endpoints:
 *
 *   gettimeofday(&tv, NULL)       — seconds + microseconds since the
 *                                   Unix epoch. Used by Date.now and
 *                                   to seed Math.random.
 *   clock_gettime(CLOCK_*, &ts)   — a monotonic-ish clock. QuickJS'
 *                                   hrtime helper just wants something
 *                                   that ticks forward.
 *
 * We combine SYS_GET_WALL_TIME (RTC seconds, UTC) and SYS_GET_TIME
 * (PIT milliseconds since boot) to get plausible values for both.
 * Microseconds get the boot-millisecond's remainder × 1000 so the
 * sub-second field is at least monotonic between two `gettimeofday`
 * calls inside the same second.
 */

#include "../include/equos.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/wall_time.h>
#include <time.h>

int gettimeofday(struct timeval *tv, void *tz) {
  (void)tz;
  if (!tv) return -1;

  uint64_t secs = 0;
  (void)sys_get_wall_time(&secs);

  uint64_t boot_ms =
      (uint64_t)(int64_t)_syscall(SYS_GET_TIME, 0, 0, 0, 0, 0);
  long usec = (long)((boot_ms % 1000ull) * 1000ull);

  tv->tv_sec  = (long)secs;
  tv->tv_usec = usec;
  return 0;
}

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
  if (!tp) return -1;

  if (clk_id == CLOCK_MONOTONIC) {
    uint64_t boot_ms =
        (uint64_t)(int64_t)_syscall(SYS_GET_TIME, 0, 0, 0, 0, 0);
    tp->tv_sec  = (time_t)(boot_ms / 1000ull);
    tp->tv_nsec = (long)((boot_ms % 1000ull) * 1000000ull);
    return 0;
  }

  /* CLOCK_REALTIME and anything else fall back to wall time. */
  uint64_t secs = 0;
  (void)sys_get_wall_time(&secs);
  uint64_t boot_ms =
      (uint64_t)(int64_t)_syscall(SYS_GET_TIME, 0, 0, 0, 0, 0);
  tp->tv_sec  = (time_t)secs;
  tp->tv_nsec = (long)((boot_ms % 1000ull) * 1000000ull);
  return 0;
}

/* Reentrant time decomposition. QuickJS' Date code uses these when
 * NO_TM_GMTOFF is defined (we set it in the Makefile because our
 * struct tm has no tm_gmtoff field). Both write into *result and
 * return it.
 *
 * EquinoxOS has no timezone database, so localtime_r is identical
 * to gmtime_r — we always return UTC. (QuickJS uses the difference
 * between mktime(localtime_r) and mktime(gmtime_r) to derive the
 * offset; returning the same tm in both gives offset=0, i.e. UTC,
 * which is the correct answer for our system.) */

static void unix_to_tm(time_t t, struct tm *out) {
  /* Days since 1970-01-01 (proleptic Gregorian). */
  long days = (long)(t / 86400);
  long secs = (long)(t % 86400);
  if (secs < 0) { secs += 86400; days--; }

  out->tm_sec  = (int)(secs % 60);
  out->tm_min  = (int)((secs / 60) % 60);
  out->tm_hour = (int)(secs / 3600);

  /* Day of week: 1970-01-01 was a Thursday (=4). */
  out->tm_wday = (int)(((days % 7) + 4 + 7) % 7);

  /* Walk years from 1970 until we run out of `days`. Handles both
   * forward and backward time by allowing days to go negative. */
  int year = 1970;
  while (1) {
    int leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
    int year_days = leap ? 366 : 365;
    if (days >= year_days) { days -= year_days; year++; }
    else if (days < 0)     { year--;
                              int prev_leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
                              days += prev_leap ? 366 : 365; }
    else break;
  }
  int leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
  static const int mdays[2][12] = {
    {31,28,31,30,31,30,31,31,30,31,30,31},
    {31,29,31,30,31,30,31,31,30,31,30,31},
  };
  int mon = 0;
  while (mon < 12 && days >= mdays[leap][mon]) {
    days -= mdays[leap][mon];
    mon++;
  }

  out->tm_year  = year - 1900;
  out->tm_mon   = mon;
  out->tm_mday  = (int)days + 1;
  out->tm_yday  = 0; /* not strictly needed by QuickJS */
  out->tm_isdst = 0;
}

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
  if (!timep || !result) return NULL;
  unix_to_tm(*timep, result);
  return result;
}

struct tm *localtime_r(const time_t *timep, struct tm *result) {
  return gmtime_r(timep, result);
}
