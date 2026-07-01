// sdk/lib/math.c
#include <math.h>
#include <time.h>

double fabs(double x) { return x < 0 ? -x : x; }

/* floor/ceil реализованы через битовые операции по стандарту IEEE-754,
 * БЕЗ преобразования double->int. Прежняя версия на (long long)x
 * мискомпилировалась в ring3 и возвращала 0 для дробных чисел
 * (floor(399.7)==0), из-за чего у SDL обнулялся viewport/clip -> чёрный
 * экран. Битовый вариант использует только целочисленные/памятевые
 * операции, которые в ring3 работают корректно. */
double floor(double x) {
  union { double f; unsigned long long i; } u;
  u.f = x;
  int e = (int)((u.i >> 52) & 0x7ff) - 0x3ff; /* несмещённая экспонента */
  if (e >= 52) return x;                       /* |x|>=2^52 либо inf/nan */
  if (e < 0) {                                 /* |x| < 1 */
    if (u.i >> 63)                             /* отрицательное */
      return (u.i << 1) ? -1.0 : x;            /* (-1,0) -> -1 ; -0.0 как есть */
    return 0.0;                                /* [0,1) -> +0 */
  }
  unsigned long long m = 0x000fffffffffffffULL >> e; /* маска дробной части */
  if ((u.i & m) == 0) return x;                /* уже целое */
  if (u.i >> 63) u.i += m;                     /* отрицательное: к -inf */
  u.i &= ~m;
  return u.f;
}

double ceil(double x) {
  union { double f; unsigned long long i; } u;
  u.f = x;
  int e = (int)((u.i >> 52) & 0x7ff) - 0x3ff;
  if (e >= 52) return x;
  if (e < 0) {
    if (u.i >> 63)                             /* (-1,0) -> -0.0 */
      return (u.i << 1) ? -0.0 : x;
    return (u.i << 1) ? 1.0 : x;               /* (0,1) -> 1 ; +0 как есть */
  }
  unsigned long long m = 0x000fffffffffffffULL >> e;
  if ((u.i & m) == 0) return x;
  if (!(u.i >> 63)) u.i += m;                  /* положительное: к +inf */
  u.i &= ~m;
  return u.f;
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
  if (x == 0.0) return 0.0;
  if (y == 0.0) return 1.0;

  // Если показатель степени целый — используем быстрый цикл
  int iy = (int)y;
  if ((double)iy == y) {
    double res = 1.0;
    if (iy > 0) {
      for (int i = 0; i < iy; i++) {
        res *= x;
      }
    } else {
      for (int i = 0; i < -iy; i++) {
        res /= x;
      }
    }
    return res;
  }

  // Для дробных показателей степени используем тождество x^y = exp(y * log(x))
  if (x < 0.0) {
    return 0.0; // Избегаем логарифма отрицательного числа
  }
  return exp(y * log(x));
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

float powf(float x, float y) {
  return (float)pow((double)x, (double)y);
}

float fabsf(float x) {
  return x < 0.0f ? -x : x;
}

float sqrtf(float x) {
  return (float)sqrt((double)x);
}

float sinf(float x) {
  return (float)sin((double)x);
}

float cosf(float x) {
  return (float)cos((double)x);
}

float tanf(float x) {
  return (float)tan((double)x);
}

float floorf(float x) {
  return (float)floor((double)x);
}

float ceilf(float x) {
  return (float)ceil((double)x);
}