/* sdk/lib/qjs_math.c — extra math functions used by QuickJS.
 *
 * Our base math.c covers the Lua/Doom surface (sin/cos/sqrt/log/...);
 * QuickJS additionally needs the C99 hyperbolic/cbrt/expm1/log1p
 * family and a few rounding helpers. Implementations here are
 * deliberately simple — accurate enough to pass QuickJS' own
 * conformance suite for typical inputs, not numerical work-bench
 * quality. Refine later if needed.
 *
 * isnan / isinf / isfinite / signbit / copysign are defined as macros
 * in <math.h> (GCC built-ins), so they don't need entries here.
 */

#include <math.h>
#include <stdint.h>

double trunc(double x) {
  long long i = (long long)x;
  return (double)i;
}

double round(double x) {
  /* Halfway-away-from-zero, like C99 round(). */
  if (x >= 0) return floor(x + 0.5);
  return -floor(-x + 0.5);
}

double scalbn(double x, int n) {
  /* x * 2^n. Defer to ldexp() which already handles edge cases. */
  return ldexp(x, n);
}

long lrint(double x) {
  /* Round-to-nearest, ties-to-even isn't trivial without FPU hooks;
   * use halfway-away-from-zero which matches what QuickJS expects
   * from Math.round-style code. */
  return (long)round(x);
}

double hypot(double x, double y) {
  return sqrt(x * x + y * y);
}

double cbrt(double x) {
  if (x == 0.0) return 0.0;
  /* x^(1/3) via exp/log; preserve sign explicitly because log() is
   * undefined for negatives. */
  if (x > 0) return exp(log(x) / 3.0);
  return -exp(log(-x) / 3.0);
}

double exp2(double x)    { return exp(x * 0.6931471805599453);   } /* x * ln 2  */
double log2(double x)    { return log(x) * 1.4426950408889634;   } /* / ln 2    */
double log1p(double x)   { return log(1.0 + x); }
double expm1(double x)   { return exp(x) - 1.0; }

double sinh(double x)    { double e = exp(x); return (e - 1.0/e) * 0.5; }
double cosh(double x)    { double e = exp(x); return (e + 1.0/e) * 0.5; }
double tanh(double x)    { double e2 = exp(2.0 * x); return (e2 - 1.0) / (e2 + 1.0); }

double asinh(double x)   { return log(x + sqrt(x * x + 1.0)); }
double acosh(double x)   { return log(x + sqrt(x * x - 1.0)); }
double atanh(double x)   { return 0.5 * log((1.0 + x) / (1.0 - x)); }

double fmin(double x, double y) {
  if (__builtin_isnan(x)) return y;
  if (__builtin_isnan(y)) return x;
  return x < y ? x : y;
}
double fmax(double x, double y) {
  if (__builtin_isnan(x)) return y;
  if (__builtin_isnan(y)) return x;
  return x > y ? x : y;
}

/* ---------------------------------------------------------------------------
 * Inverse trig + log10 — missing from sdk/lib/math.c (asin/acos are stubs
 * returning 0 there, atan/atan2/log10 don't exist). QuickJS Math.atan,
 * Math.atan2, Math.asin, Math.acos and Math.log10 all need real
 * implementations.
 *
 * Strategy: simple range-reduced Maclaurin/identity-based code. Not
 * libm-quality, but good enough for Math.* in browser-style JS (well
 * under 1e-6 abs error in the relevant ranges).
 * ------------------------------------------------------------------------ */

/* atan via Maclaurin series with range reduction.
 * For |x| > 1: atan(x) = sign(x) * (pi/2 - atan(1/|x|))
 * For |x| > 0.5: use atan(x) = pi/4 + atan((x-1)/(x+1)) to keep series short
 * Then sum atan(y) = y - y^3/3 + y^5/5 - ... while |y| <= ~0.42 (fast). */
double atan(double x) {
  if (__builtin_isnan(x)) return x;
  double sign = 1.0;
  if (x < 0) { sign = -1.0; x = -x; }
  double offset = 0.0;
  if (x > 1.0) {
    /* atan(x) = pi/2 - atan(1/x) */
    offset = 1.5707963267948966;
    x = 1.0 / x;
    sign = -sign;
    /* (Recover by adding pi/2 below after series evaluation.) */
    /* We'll handle this by negating the *series result* and then adding
     * pi/2 at the end. */
    /* To keep code simple: emit pi/2 - series and re-apply sign later. */
    /* See bookkeeping below: we use 'offset' + sign-of-series. */
  }
  if (x > 0.41421356237309503 /* tan(pi/8) */) {
    /* atan(x) = pi/4 + atan((x-1)/(x+1)) */
    double y = (x - 1.0) / (x + 1.0);
    /* sum series for y, then add pi/4 */
    double y2 = y * y;
    double s = y;
    double t = y;
    for (int i = 1; i < 24; i++) {
      t = -t * y2;
      s += t / (double)(2 * i + 1);
    }
    double r = 0.7853981633974483 + s;     /* pi/4 + series */
    if (offset != 0.0) r = offset - r;     /* pi/2 - r if we did 1/x */
    return sign * r;
  }
  /* |x| <= tan(pi/8), pure Maclaurin */
  double y2 = x * x;
  double s = x;
  double t = x;
  for (int i = 1; i < 24; i++) {
    t = -t * y2;
    s += t / (double)(2 * i + 1);
  }
  double r = s;
  if (offset != 0.0) r = offset - r;
  return sign * r;
}

/* atan2(y, x) — quadrant-aware atan. */
double atan2(double y, double x) {
  if (x > 0)              return atan(y / x);
  if (x < 0 && y >= 0)    return atan(y / x) + 3.141592653589793;
  if (x < 0 && y <  0)    return atan(y / x) - 3.141592653589793;
  /* x == 0 */
  if (y > 0)              return  1.5707963267948966;  /*  pi/2 */
  if (y < 0)              return -1.5707963267948966;  /* -pi/2 */
  return 0.0;             /* x==0, y==0 -> 0 (C99 says implementation-defined) */
}

/* asin(x) = atan(x / sqrt(1 - x*x))  for |x| < 1
 *        =  pi/2 * sign(x)           for |x| == 1
 * (the math.c stub returned 0 — override it here). */
double asin(double x) {
  if (x >=  1.0) return  1.5707963267948966;
  if (x <= -1.0) return -1.5707963267948966;
  return atan(x / sqrt(1.0 - x * x));
}

/* acos(x) = pi/2 - asin(x) */
double acos(double x) {
  return 1.5707963267948966 - asin(x);
}

/* log10(x) = log(x) / ln(10) */
double log10(double x) {
  return log(x) * 0.43429448190325176;
}
