/*
 * stktest — проверка System V initial stack (Этап 6b-1), БЕЗ musl.
 *
 * Ядро (task_load_image) теперь дополнительно строит на вершине стека
 * полноценный SysV-кадр: argc / argv[] / NULL / envp[] / NULL / auxv / AT_NULL.
 * Родной crt0 его игнорирует (читает регистры), а musl будет читать из rsp.
 *
 * Этот тест НЕ линкуется с SDK-crt0: он определяет СВОЙ _start, который сразу
 * захватывает rsp (указатель на argc) и разбирает кадр ровно так, как это
 * делает musl. Вывод — через родной SYS_PRINT (int 0x80, rax=1), выход —
 * SYS_EXIT (rax=10). Если argc/argv/envp/auxv печатаются корректно — значит
 * фундамент под musl готов.
 *
 * Запуск в шелле ОС:  run stktest.elf
 */

/* ---- сырые родные сисколлы (int 0x80) ----------------------------------- */
static inline long sys1(long n, long a1) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a1)
                   : "rcx", "r11", "memory");
  return r;
}

static void kprint(const char *s) { sys1(1 /*SYS_PRINT*/, (long)s); }
static void kexit(long code)      { sys1(10 /*SYS_EXIT*/, code); }

static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

/* печать беззнакового числа в десятичном */
static void kprint_u(unsigned long v) {
  char b[24]; int i = 22; b[23] = 0;
  if (v == 0) b[i--] = '0';
  while (v && i >= 0) { b[i--] = (char)('0' + v % 10); v /= 10; }
  kprint(&b[i + 1]);
}
/* печать числа в hex (0x...) */
static void kprint_x(unsigned long v) {
  char b[19]; int i = 17; b[18] = 0;
  const char *h = "0123456789abcdef";
  if (v == 0) b[i--] = '0';
  while (v && i >= 0) { b[i--] = h[v & 0xF]; v >>= 4; }
  kprint("0x"); kprint(&b[i + 1]);
}

/* разбор SysV-кадра: sp указывает на argc.
 * ВАЖНО: все обходы массивов ОГРАНИЧЕНЫ верхней страницей стека. stack_top
 * выровнен по 4 KiB и НЕ отображён, поэтому первый адрес за ним = (sp|0xFFF)+1.
 * Любое чтение по этому адресу и выше вызвало бы #PF — мы такого не делаем,
 * так что даже при кривом кадре тест печатает FAIL, а не роняет ядро. */
static void stk_main(unsigned long *sp) {
  unsigned long top = ((unsigned long)sp | 0xFFFUL) + 1; /* = stack_top */
  #define INRANGE(p) ((unsigned long)(p) + 8 <= top)

  long argc = (long)sp[0];
  if (argc < 0 || argc > 4096) argc = 0;  /* защита от мусорного argc */
  char **argv = (char **)&sp[1];
  char **envp = argv + argc + 1;          /* после NULL-терминатора argv */

  kprint("stktest: SysV stack OK\n");
  kprint("  argc = "); kprint_u((unsigned long)argc); kprint("\n");
  for (long i = 0; i < argc && INRANGE(&argv[i]); i++) {
    kprint("  argv["); kprint_u((unsigned long)i); kprint("] = ");
    kprint(argv[i] ? argv[i] : "(null)");
    kprint(" (len "); kprint_u((unsigned long)slen(argv[i])); kprint(")\n");
  }
  if (INRANGE(&argv[argc]) && argv[argc] != 0)
    kprint("  !! argv not NULL-terminated\n");

  /* envp (обход ограничен страницей) */
  int envc = 0;
  while (INRANGE(&envp[envc]) && envp[envc]) envc++;
  kprint("  envc = "); kprint_u((unsigned long)envc); kprint("\n");
  for (int i = 0; i < envc && i < 8; i++) {
    kprint("  envp["); kprint_u((unsigned long)i); kprint("] = ");
    kprint(envp[i]); kprint("\n");
  }

  /* auxv: пары (key,val) после NULL-терминатора envp */
  unsigned long *aux = (unsigned long *)(envp + envc + 1);
  int auxn = 0;
  unsigned long at_random = 0, at_pagesz = 0, at_entry = 0, at_phdr = 0, at_phnum = 0;
  for (; INRANGE(&aux[1]) && aux[0] != 0; aux += 2) {
    auxn++;
    switch (aux[0]) {
      case 3:  at_phdr   = aux[1]; break;
      case 5:  at_phnum  = aux[1]; break;
      case 6:  at_pagesz = aux[1]; break;
      case 9:  at_entry  = aux[1]; break;
      case 25: at_random = aux[1]; break;
      default: break;
    }
  }
  kprint("  auxv entries = "); kprint_u((unsigned long)auxn); kprint("\n");
  kprint("  AT_PAGESZ = "); kprint_u(at_pagesz); kprint("\n");
  kprint("  AT_PHDR   = "); kprint_x(at_phdr); kprint("\n");
  kprint("  AT_PHNUM  = "); kprint_u(at_phnum); kprint("\n");
  kprint("  AT_ENTRY  = "); kprint_x(at_entry); kprint("\n");
  kprint("  AT_RANDOM = "); kprint_x(at_random);
  if (at_random && INRANGE((unsigned long *)at_random)) {
    unsigned char *r = (unsigned char *)at_random;
    kprint("  bytes="); for (int i = 0; i < 4; i++) { kprint_x(r[i]); kprint(" "); }
  }
  kprint("\n");

  /* базовые проверки */
  int pass = (argc >= 1) && (at_pagesz == 4096) && (at_random != 0) &&
             INRANGE(&argv[argc]) && (argv[argc] == 0);
  kprint(pass ? "stktest: PASS\n" : "stktest: FAIL\n");
  kexit(pass ? 0 : 1);
  #undef INRANGE
}

/* Свой _start: захватываем rsp (= указатель на argc) и идём в stk_main. */
__attribute__((naked, used)) void _start(void) {
  __asm__ volatile(
      "mov %rsp, %rdi\n"
      "and $-16, %rsp\n"
      "call stk_main\n"
      "mov $10, %rax\n"   /* SYS_EXIT на случай возврата */
      "xor %rdi, %rdi\n"
      "int $0x80\n");
}
