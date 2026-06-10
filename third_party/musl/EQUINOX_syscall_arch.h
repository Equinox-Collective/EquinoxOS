#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

/*
 * EquinoxOS (Этап 6b-2): musl на x86_64 обычно использует инструкцию `syscall`.
 * В EquinoxOS пользовательские программы попадают в Linux-ABI-шлюз через
 * программное прерывание `int $0x81` (см. src/system/usr/syscall.c,
 * linux_syscall_handler из Этапа 6a). Соглашение об аргументах ИДЕНТИЧНО
 * Linux/SysV: номер в rax, аргументы rdi, rsi, rdx, r10, r8, r9; результат в
 * rax. Поэтому достаточно заменить мнемонику `syscall` на `int $0x81` — всё
 * остальное (включая 4-й аргумент в r10, а не rcx) совпадает.
 *
 * Клоберы rcx/r11 оставлены для совместимости: настоящий `syscall` их затирает,
 * `int` — нет, но «лишний» клобер безопасен (просто запрещает компилятору
 * держать там живые значения через границу вызова).
 */

static __inline long __syscall0(long n)
{
	unsigned long ret;
	__asm__ __volatile__ ("int $0x81" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
	return ret;
}

static __inline long __syscall1(long n, long a1)
{
	unsigned long ret;
	__asm__ __volatile__ ("int $0x81" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
	return ret;
}

static __inline long __syscall2(long n, long a1, long a2)
{
	unsigned long ret;
	__asm__ __volatile__ ("int $0x81" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2)
						  : "rcx", "r11", "memory");
	return ret;
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	unsigned long ret;
	__asm__ __volatile__ ("int $0x81" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2),
						  "d"(a3) : "rcx", "r11", "memory");
	return ret;
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	unsigned long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ __volatile__ ("int $0x81" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2),
						  "d"(a3), "r"(r10): "rcx", "r11", "memory");
	return ret;
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	unsigned long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	__asm__ __volatile__ ("int $0x81" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2),
						  "d"(a3), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
	return ret;
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	unsigned long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	__asm__ __volatile__ ("int $0x81" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2),
						  "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
	return ret;
}

/*
 * Без vDSO: EquinoxOS не отдаёт AT_SYSINFO_EHDR в auxv, поэтому НЕ объявляем
 * VDSO_*. musl тогда не пытается резолвить __vdso_clock_gettime и идёт прямым
 * сисколлом clock_gettime (его мы добавляем в шлюз на Этапе 6b-2).
 */

#define IPC_64 0
