// sdk/lib/math.c
#include <math.h>
#include <time.h>

double fabs(double x) { return x < 0 ? -x : x; }

double floor(double x) {
  long long i = (long long)x;
  if (x < 0 && x != (double)i)
    return (double)(i - 1);
  return (double)i;
}

double ceil(double x) {
  long long i = (long long)x;
  if (x > 0 && x != (double)i)
    return (double)(i + 1);
  return (double)i;
}

double fmod(double x, double y) {
  if (y == 0.0)
    return 0.0;
  double q = x / y;
  return x - floor(q) * y;
}

double modf(double x, double *iptr) {
  *iptr = floor(x);
  return x - *iptr;
}

// Расщепление на мантиссу и экспоненту (нужно для Lua парсера)
double frexp(double x, int *exp) {
  *exp = 0;
  if (x == 0.0)
    return 0.0;
  int sign = x < 0 ? -1 : 1;
  x = fabs(x);
  while (x >= 1.0) {
    x /= 2.0;
    (*exp)++;
  }
  while (x < 0.5) {
    x *= 2.0;
    (*exp)--;
  }
  return x * sign;
}

double ldexp(double x, int exp) {
  double res = x;
  if (exp > 0) {
    while (exp--)
      res *= 2.0;
  } else {
    while (exp++)
      res /= 2.0;
  }
  return res;
}

// Честный квадратный корень (метод Ньютона)
double sqrt(double x) {
  if (x <= 0)
    return 0;
  double res = x;
  for (int i = 0; i < 15; i++)
    res = 0.5 * (res + x / res);
  return res;
}

// Возведение в степень
double pow(double x, double y) {
  if (y == 0)
    return 1.0;
  double res = 1.0;
  int iy = (int)y;
  if (iy > 0) {
    for (int i = 0; i < iy; i++)
      res *= x;
  } else {
    for (int i = 0; i < -iy; i++)
      res /= x;
  }
  return res;
}

// Экспонента и логарифм (Ряды Тейлора)
double exp(double x) {
  double sum = 1.0, term = 1.0;
  for (int i = 1; i < 20; i++) {
    term *= x / i;
    sum += term;
  }
  return sum;
}

double log(double x) {
  if (x <= 0)
    return -1.0;
  double res = 0.0;
  for (int i = 0; i < 15; i++)
    res = res + 2.0 * (x - exp(res)) / (x + exp(res));
  return res;
}
double log10(double x) { return log(x) / 2.30258509299; }

// --- Аппаратная тригонометрия через сопроцессор (x87 FPU) ---
double sin(double x) {
  double res;
  __asm__ volatile("fsin" : "=t"(res) : "0"(x));
  return res;
}
double cos(double x) {
  double res;
  __asm__ volatile("fcos" : "=t"(res) : "0"(x));
  return res;
}
double tan(double x) {
  double res;
  __asm__ volatile("fptan; fstp %%st(0)" : "=t"(res) : "0"(x));
  return res;
}
double atan2(double y, double x) {
  double res;
  __asm__ volatile("fpatan" : "=t"(res) : "0"(x), "u"(y));
  return res;
}
/* asin/acos moved to sdk/lib/qjs_math.c (the stubs here returned 0
 * which broke Math.asin / Math.acos for QuickJS — see qjs_math.c). */
double difftime(time_t t1, time_t t0) { return (double)(t1 - t0); }