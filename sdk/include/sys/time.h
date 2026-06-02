#ifndef _SYS_TIME_H
#define _SYS_TIME_H

/* Minimal <sys/time.h> for QuickJS.
 *
 * QuickJS' cutils.h calls gettimeofday() to seed Math.random and to
 * implement Date.now() (until a more precise host hook is wired up).
 * We back this with SYS_GET_WALL_TIME (whole seconds from the RTC) +
 * SYS_GET_TIME (millisecond uptime) so that the microsecond field is
 * at least monotonic-ish.
 *
 * struct timezone is left as a no-op type — gettimeofday() ignores the
 * second argument on every modern Unix; QuickJS passes NULL.
 */

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _STRUCT_TIMEVAL_DEFINED
#define _STRUCT_TIMEVAL_DEFINED
struct timeval {
  long tv_sec;
  long tv_usec;
};
#endif

struct timezone {
  int tz_minuteswest;
  int tz_dsttime;
};

int gettimeofday(struct timeval *tv, void *tz);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TIME_H */
