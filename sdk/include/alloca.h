#ifndef _ALLOCA_H
#define _ALLOCA_H

/* EquinoxOS minimal <alloca.h>.
 *
 * QuickJS' libregexp.c uses `alloca()` for small per-call scratch
 * buffers. GCC provides this as a built-in on every target we care
 * about. Just alias it here so the include works.
 */

#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif

#endif /* _ALLOCA_H */
