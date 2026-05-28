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
