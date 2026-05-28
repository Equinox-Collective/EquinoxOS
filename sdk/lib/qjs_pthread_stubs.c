/* sdk/lib/qjs_pthread_stubs.c — no-op pthread implementations.
 *
 * EquinoxOS is single-threaded per process, so QuickJS' mutex / cond /
 * once helpers can all collapse to no-ops. Each function returns 0
 * (success) and performs no work.
 *
 * If/when EquinoxOS grows real threads, replace these with a proper
 * implementation. For now this keeps the linker happy so userspace
 * apps can pull in libquickjs.a without dragging in a real pthread
 * library.
 *
 * pthread_create / pthread_join are deliberately NOT stubbed — if
 * someone actually calls them we want a link error so we notice the
 * threaded code path got reached.
 */

#include <pthread.h>
#include <stddef.h>
#include <time.h>

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
  (void)m; (void)a; return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m)  { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m)     { (void)m; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m)   { (void)m; return 0; }

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
  (void)c; (void)a; return 0;
}
int pthread_cond_destroy(pthread_cond_t *c)    { (void)c; return 0; }
int pthread_cond_signal(pthread_cond_t *c)     { (void)c; return 0; }
int pthread_cond_broadcast(pthread_cond_t *c)  { (void)c; return 0; }

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  /* Single-threaded: nobody can ever wake us, so "wait" is a no-op.
   * QuickJS only calls this from Atomics.wait, which our port doesn't
   * exercise. */
  (void)c; (void)m; return 0;
}
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *t) {
  (void)c; (void)m; (void)t; return 0;
}

int pthread_condattr_init(pthread_condattr_t *a)    { (void)a; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a) { (void)a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a, int c) {
  (void)a; (void)c; return 0;
}

int pthread_once(pthread_once_t *guard, void (*callback)(void)) {
  /* Single-threaded: just run-once via the guard word. */
  if (guard && *guard == 0 && callback) {
    *guard = 1;
    callback();
  }
  return 0;
}

int pthread_attr_init(pthread_attr_t *a)              { (void)a; return 0; }
int pthread_attr_destroy(pthread_attr_t *a)           { (void)a; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t s) {
  (void)a; (void)s; return 0;
}
int pthread_attr_setdetachstate(pthread_attr_t *a, int s) {
  (void)a; (void)s; return 0;
}

int pthread_detach(pthread_t t)        { (void)t; return 0; }
pthread_t pthread_self(void)           { return (pthread_t)1; }
