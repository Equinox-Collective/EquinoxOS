#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 1000

/* clockid_t + clock_gettime — QuickJS' cutils.h uses these to drive
 * its monotonic-clock fallback. CLOCK_REALTIME maps to RTC wall time
 * (SYS_GET_WALL_TIME); CLOCK_MONOTONIC maps to PIT ms-since-boot
 * (SYS_GET_TIME). Both end up second/millisecond-granular, which is
 * fine for QuickJS' use (Math.random seed, Date.now). */
typedef int clockid_t;
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1

#ifndef _STRUCT_TIMESPEC_DEFINED
#define _STRUCT_TIMESPEC_DEFINED
struct timespec {
  time_t tv_sec;
  long   tv_nsec;
};
#endif

struct tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
  int tm_wday;
  int tm_yday;
  int tm_isdst;
};

time_t time(time_t *t);
clock_t clock(void);
int clock_gettime(clockid_t clk_id, struct timespec *tp);
struct tm *gmtime(const time_t *timep);
struct tm *localtime(const time_t *timep);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
struct tm *localtime_r(const time_t *timep, struct tm *result);
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);
time_t mktime(struct tm *tm);
double difftime(time_t time1, time_t time0);

#endif