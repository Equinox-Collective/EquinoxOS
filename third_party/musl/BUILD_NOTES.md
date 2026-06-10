# Вендоринг musl 1.2.5 для EquinoxOS (Этап 6b-2)

Здесь лежит **предсобранная** статическая musl libc для EquinoxOS. Собирать
musl на самой Windows НЕ нужно — `make` просто линкуется с готовым `lib/libc.a`.

## Что внутри
- `lib/libc.a` — статическая musl libc (~2.5 МБ, 1345 объектов).
- `lib/crt1.o`, `lib/crti.o`, `lib/crtn.o` — стартовый код musl (`_start` →
  `__libc_start_main` → `main`). Читают SysV-кадр со стека (наш Этап 6b-1),
  ставят TLS через `arch_prctl(ARCH_SET_FS)` (наш Этап 6b).
- `lib/*.a` остальные (`libm`, `libpthread`, `librt`, …) — пустые архивы musl
  (вся функциональность уже в `libc.a`), нужны только чтобы `-lm`/`-lpthread`
  линковались без ошибок.
- `include/` — публичные заголовки musl (218 шт.), включая сгенерированные
  `bits/alltypes.h` и `bits/syscall.h` для x86_64.
- `EQUINOX_syscall_arch.h` — наш патч `arch/x86_64/syscall_arch.h`: `__syscallN`
  использует `int $0x81` (Linux-ABI-шлюз, Этап 6a) вместо инструкции `syscall`.
  Соглашение об аргументах идентично Linux (rdi/rsi/rdx/r10/r8/r9, номер в rax).

## Как это было собрано (для воспроизводимости, на POSIX-хосте)
```
# musl-1.2.5, в arch/x86_64/syscall_arch.h заменён `syscall` на `int $0x81`
./configure --target=x86_64 CC=gcc \
    CFLAGS="-mno-red-zone -fno-stack-protector -fno-pic -fno-PIE" \
    --disable-shared --prefix=$PWD/_install
make AR=ar RANLIB=ranlib lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o install
```
Объекты — x86_64 ELF (REL), флаги совпадают с `USER_CFLAGS` ядра
(`-mno-red-zone -fno-pic`), поэтому линкуются с `x86_64-elf-ld`.

## Как линковать программу против musl (см. правило musltest.elf в Makefile)
```
x86_64-elf-gcc -ffreestanding -mcmodel=small -mno-red-zone \
    -fno-stack-protector -fno-pic -nostdinc \
    -isystem third_party/musl/include -c app/foo.c -o app/foo.o

x86_64-elf-gcc -nostdlib -static -Wl,-Ttext=0x1000000 \
    third_party/musl/lib/crt1.o third_party/musl/lib/crti.o app/foo.o \
    third_party/musl/lib/libc.a -lgcc third_party/musl/lib/crtn.o -o foo.elf
```

## Лицензия
musl распространяется под MIT (см. `COPYRIGHT`).
