#ifndef _MATH_H
#define _MATH_H

// Константы
#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nan(""))

#define M_PI 3.14159265358979323846

// Округление
double floor(double x);
double ceil(double x);

// Экспоненты и логарифмы
double pow(double x, double y);
double log(double x);
double log10(double x);
double exp(double x);
double sqrt(double x);

// Тригонометрия
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

// Манипуляции с числами (критично для Lua)
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *iptr);
double fmod(double x, double y);
double fabs(double x);

/* C99 classification macros — QuickJS uses these in its number /
 * Math.* code. Map to GCC built-ins; they're constant-foldable and
 * don't require any libm symbol. */
#ifndef isnan
#define isnan(x)     __builtin_isnan(x)
#endif
#ifndef isinf
#define isinf(x)     __builtin_isinf(x)
#endif
#ifndef isfinite
#define isfinite(x)  __builtin_isfinite(x)
#endif
#ifndef signbit
#define signbit(x)   __builtin_signbit(x)
#endif
#ifndef copysign
#define copysign(x,y) __builtin_copysign((x),(y))
#endif

/* Additional functions QuickJS' Math.* surface needs. Implementations
 * live in sdk/lib/qjs_math.c (simple but spec-correct). */
double trunc(double x);
double round(double x);
double scalbn(double x, int n);
long   lrint(double x);
double hypot(double x, double y);
double cbrt(double x);
double exp2(double x);
double log2(double x);
double log1p(double x);
double expm1(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double fmin(double x, double y);
double fmax(double x, double y);

#endif