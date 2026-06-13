/*
 * fstest — проверка, что FS_BASE (TLS-указатель) ВЫЖИВАЕТ переключение
 * контекста (Этап 6b, перед вендорингом musl). БЕЗ musl, свой _start.
 *
 * Зачем: musl ставит свой TLS через arch_prctl(ARCH_SET_FS). Если ядро не
 * сохраняет адрес в current_task->fs_base, то schedule() на следующем тике
 * перезапишет MSR старым значением и затрёт TLS — errno/__libc развалятся.
 * Этот тест:
 *   1) кладёт сигнатуру в tls[0];
 *   2) arch_prctl(ARCH_SET_FS, &tls) через int 0x81 (Linux syscall 158);
 *   3) читает %fs:0 — должно быть = сигнатуре;
 *   4) крутит длинный цикл, чтобы планировщик нас несколько раз вытеснил;
 *   5) снова читает %fs:0 — должно ОСТАТЬСЯ = сигнатуре.
 * PASS => FS_BASE доезжает до ring3 и переживает переключения => musl-ready.
 *
 * Запуск в шелле ОС:  run fstest.elf
 */

/* ---- родной SYS_PRINT/SYS_EXIT (int 0x80) ------------------------------- */
static inline long sys1(long n, long a1) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a1)
                   : "rcx", "r11", "memory");
  return r;
}
static void kprint(const char *s) { sys1(1 /*SYS_PRINT*/, (long)s); }
static void kexit(long code)      { sys1(10 /*SYS_EXIT*/, code); }

/* печать hex (0x...) */
static void kprint_x(unsigned long v) {
  char b[19]; int i = 17; b[18] = 0;
  const char *h = "0123456789abcdef";
  if (v == 0) b[i--] = '0';
  while (v && i >= 0) { b[i--] = h[v & 0xF]; v >>= 4; }
  kprint("0x"); kprint(&b[i + 1]);
}

/* ---- Linux arch_prctl через шлюз int 0x81 ------------------------------- */
static inline long larch_prctl(long code, unsigned long addr) {
  long r;
  __asm__ volatile("int $0x81" : "=a"(r)
                   : "a"(158L), "D"(code), "S"(addr)
                   : "rcx", "r11", "memory");
  return r;
}

/* чтение машинного слова по %fs:0 (TCB self-slot) */
static inline unsigned long read_fs0(void) {
  unsigned long v;
  __asm__ volatile("mov %%fs:0, %0" : "=r"(v));
  return v;
}

/* TLS-блок в BSS (в PT_LOAD, доступен ring3). Слот 0 — то, что читает %fs:0. */
static unsigned long tls_block[8];

#define SIG 0xCAFEBABEDEADBEEFUL

void stk_main(void) {
  tls_block[0] = SIG;

  long rc = larch_prctl(0x1002 /*ARCH_SET_FS*/, (unsigned long)&tls_block[0]);
  kprint("fstest: arch_prctl(SET_FS) rc="); kprint_x((unsigned long)rc); kprint("\n");

  unsigned long v1 = read_fs0();
  kprint("  fs:0 (before switches) = "); kprint_x(v1); kprint("\n");

  /* Долгий цикл -> несколько таймерных тиков -> переключения контекста. */
  volatile unsigned long spin = 0;
  for (unsigned long i = 0; i < 400000000UL; i++) spin += i;

  unsigned long v2 = read_fs0();
  kprint("  fs:0 (after  switches) = "); kprint_x(v2); kprint("\n");

  int pass = (v1 == SIG) && (v2 == SIG);
  if (!pass && v2 != SIG)
    kprint("  !! FS_BASE затёрт планировщиком — musl сломался бы здесь\n");
  kprint(pass ? "fstest: PASS\n" : "fstest: FAIL\n");
  kexit(pass ? 0 : 1);
}

/* Свой _start: без аргументов, сразу выравниваем стек и в stk_main. */
__attribute__((naked, used)) void _start(void) {
  __asm__ volatile(
      "and $-16, %rsp\n"
      "call stk_main\n"
      "mov $10, %rax\n"   /* SYS_EXIT на случай возврата */
      "xor %rdi, %rdi\n"
      "int $0x80\n");
}
