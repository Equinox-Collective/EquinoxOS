# busybox 1.36.1 для EquinoxOS (Этап 8)

`busybox.o` — relocatable-объект (`ld -r`) busybox 1.36.1, собранного против
vendored-musl (`third_party/musl`), без libc внутри. Линкуется правилом
`bin/busybox.elf` в корневом Makefile точно так же, как bash.elf.
Лицензия busybox: GPLv2.

## Как воспроизвести
1. Скачать https://busybox.net/downloads/busybox-1.36.1.tar.bz2, распаковать.
2. Обёртка компилятора `eqcc`: `gcc -specs=musl-equinox.specs -static "$@"`
   (specs подменяет include/lib на third_party/musl — см. BUILD_NOTES bash).
3. `make allnoconfig`, затем в .config включить:
   `CONFIG_STATIC=y CONFIG_LFS=y CONFIG_BUSYBOX=y CONFIG_SH_IS_NONE=y`
   и аплеты: echo cat ls grep head tail tr wc sort uniq cut pwd uname mkdir
   rmdir rm cp mv touch basename dirname env printf seq sleep true false yes
   tee rev date whoami hostname du df sync clear xxd hexdump md5sum sha256sum
   expr test which find sed awk cal free uptime
   (+ FEATURE_LS_TIMESTAMPS/SORTFILES/FILETYPES, FEATURE_FANCY_HEAD_TAIL),
   `yes "" | make oldconfig`.
4. `make -j8 CC=eqcc HOSTCC=gcc busybox_unstripped`.
5. Собрать relocatable-объект из всех built-in.o + объектов из */lib.a:
   распаковать каждый lib.a (`ar x`), затем
   `ld -r -o busybox_all.o $(find . -name built-in.o) <объекты lib.a>`
   `objcopy --strip-debug busybox_all.o busybox.o`
6. Проверка: в слинкованном busybox.elf не должно быть ни одной сырой
   syscall-инструкции: `objdump -d busybox.elf | grep -cw syscall` → 0.
