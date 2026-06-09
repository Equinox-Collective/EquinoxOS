/*
 * lxtest — проверка Linux-ABI шлюза (int 0x81), Этап 6a.
 *
 * НЕ использует SDK-сисколлы. Делает «линуксовые» вызовы напрямую через
 * `int $0x81` с линуксовыми номерами (write=1, getpid=39, exit_group=231),
 * ровно как будет делать musl после патча syscall_arch.h. Если на экране
 * (serial-консоль) видно строки — шлюз и трансляция номеров работают.
 *
 * crt0 всё равно вызовет наш родной SYS_EXIT при возврате из main, но мы
 * выходим сами через exit_group, чтобы протестировать и его трансляцию.
 */

typedef long ssize_t;

static inline long lsys1(long n, long a1) {
  long r;
  __asm__ volatile("int $0x81" : "=a"(r) : "a"(n), "D"(a1)
                   : "rcx", "r11", "memory");
  return r;
}
static inline long lsys3(long n, long a1, long a2, long a3) {
  long r;
  __asm__ volatile("int $0x81" : "=a"(r)
                   : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                   : "rcx", "r11", "memory");
  return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }

static void lwrite(const char *s) { lsys3(1 /*write*/, 1 /*stdout*/, (long)s, slen(s)); }

/* крошечный itoa для печати pid */
static void lwrite_num(long v) {
  char buf[24];
  int i = 22; buf[23] = '\0';
  if (v == 0) buf[i--] = '0';
  while (v > 0 && i >= 0) { buf[i--] = (char)('0' + (v % 10)); v /= 10; }
  lwrite(&buf[i + 1]);
}

int main(void) {
  lwrite("lxtest: int 0x81 Linux-ABI gate OK\n");

  long pid = lsys1(39 /*getpid*/, 0);
  lwrite("lxtest: getpid -> ");
  lwrite_num(pid);
  lwrite("\n");

  lwrite("lxtest: calling exit_group(7)...\n");
  lsys1(231 /*exit_group*/, 7);

  /* не должно выполниться */
  lwrite("lxtest: ERROR exit_group returned!\n");
  return 0;
}
