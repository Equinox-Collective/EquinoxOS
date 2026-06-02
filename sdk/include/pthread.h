#ifndef _PTHREAD_H
#define _PTHREAD_H

/* EquinoxOS stub <pthread.h>.
 *
 * EquinoxOS is single-threaded per process today, so QuickJS' threading
 * helpers can all degenerate into no-ops. We still need the *types* to
 * exist because cutils.h `typedef`s them unconditionally on non-Windows
 * builds, and we still need a few function declarations because some
 * QuickJS code paths reference them (most are static-inline so the
 * compiler accepts a never-called declaration; the linker will only
 * complain if those code paths actually execute, which they won't on
 * our single-threaded port).
 *
 * If/when EquinoxOS grows real threads, replace this with a proper
 * implementation. Until then everything here is intentionally trivial.
 */

#include <stddef.h>
#include <stdint.h>
#include <time.h>  /* for struct timespec, used by pthread_cond_timedwait */

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque-ish types. Sized to be at least as wide as glibc's, so that
 * any caller that does `pthread_mutex_t m = {0};` still compiles. */
typedef struct { unsigned long __opaque[8]; } pthread_mutex_t;
typedef struct { unsigned long __opaque[8]; } pthread_cond_t;
typedef struct { int           __opaque;    } pthread_mutexattr_t;
typedef struct { int           __opaque;    } pthread_condattr_t;
typedef struct { int           __opaque;    } pthread_attr_t;
typedef unsigned long          pthread_t;
typedef int                    pthread_once_t;
typedef int                    pthread_key_t;

#define PTHREAD_MUTEX_INITIALIZER { {0,0,0,0,0,0,0,0} }
#define PTHREAD_COND_INITIALIZER  { {0,0,0,0,0,0,0,0} }
#define PTHREAD_ONCE_INIT         0
#define PTHREAD_CREATE_JOINABLE   0
#define PTHREAD_CREATE_DETACHED   1

/* All of these are declared but not defined. QuickJS is built with
 * JS_HAVE_THREADS=1 but in practice only invokes them through helpers
 * that we'll never call from the EquinoxOS host. The linker only
 * complains if a call survives; in that case add a stub in
 * `sdk/lib/qjs_pthread_stubs.c`. */
extern int pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
extern int pthread_mutex_destroy(pthread_mutex_t *);
extern int pthread_mutex_lock(pthread_mutex_t *);
extern int pthread_mutex_unlock(pthread_mutex_t *);
extern int pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
extern int pthread_cond_destroy(pthread_cond_t *);
extern int pthread_cond_signal(pthread_cond_t *);
extern int pthread_cond_broadcast(pthread_cond_t *);
extern int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
extern int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *,
                                  const struct timespec *);
extern int pthread_condattr_init(pthread_condattr_t *);
extern int pthread_condattr_destroy(pthread_condattr_t *);
extern int pthread_condattr_setclock(pthread_condattr_t *, int);
extern int pthread_once(pthread_once_t *, void (*)(void));
extern int pthread_create(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);
extern int pthread_join(pthread_t, void **);
extern int pthread_detach(pthread_t);
extern pthread_t pthread_self(void);
extern int pthread_attr_init(pthread_attr_t *);
extern int pthread_attr_destroy(pthread_attr_t *);
extern int pthread_attr_setstacksize(pthread_attr_t *, size_t);
extern int pthread_attr_setdetachstate(pthread_attr_t *, int);

#ifdef __cplusplus
}
#endif

#endif /* _PTHREAD_H */
