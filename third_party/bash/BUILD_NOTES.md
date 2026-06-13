# third_party/bash — GNU bash 5.2.37 для EquinoxOS (Этап 7)

`bash.o` — предсобранный relocatable-объект (объединение всех объектников
bash через `ld -r`, БЕЗ libc), очищенный от debug-секций
(`objcopy --strip-debug`). Линкуется с vendored-musl (`third_party/musl`)
правилом `$(ISO_ROOT)/bin/bash.elf` в корневом Makefile — пересобирать bash
на Windows не нужно, как и в случае musl.

## Происхождение и лицензия

* Исходники: GNU bash **5.2.37** (официальный tarball
  `https://ftp.gnu.org/gnu/bash/bash-5.2.37.tar.gz`).
* Лицензия: **GPLv3+** (см. COPYING в tarball'е). Объект собран из
  немодифицированных исходников; вся «адаптация» — флаги конфигурации ниже.

## Как воспроизвести bash.o

Нужен Linux/WSL с gcc и vendored-musl из этого репозитория.

1. Обёртка-компилятор `eqcc` (musl headers + crt + libc, статически):

   ```sh
   #!/bin/sh
   exec gcc -specs=musl-equinox.specs -static "$@"
   ```

   где specs-файл подменяет пути на `third_party/musl/include`,
   `third_party/musl/lib/crt1.o|crti.o|crtn.o|libc.a` и добавляет
   `-static -no-pie -nostdlib`.

2. Конфигурация (из каталога `build/` рядом с распакованным tarball'ом):

   ```sh
   CC=eqcc CFLAGS="-O2 -g -mcmodel=small -mno-red-zone -fno-stack-protector \
       -fno-pic -fno-omit-frame-pointer" \
   ../configure --host=x86_64-pc-linux-musl --build=x86_64-pc-linux-gnu \
       --without-bash-malloc --disable-nls --enable-static-link \
       --disable-net-redirections \
       bash_cv_getcwd_malloc=yes bash_cv_job_control_missing=present \
       bash_cv_sys_named_pipes=present bash_cv_unusable_rtsigs=no
   ```

   Пояснения:
   * `--without-bash-malloc` — используем mallocng из musl (bash-malloc
     дёргает sbrk вперемешку с mmap, musl'ный аллокатор уже обкатан на
     Этапе 6b);
   * `--disable-nls` — без локалей/gettext;
   * `--disable-net-redirections` — `/dev/tcp` нам пока не нужен;
   * `bash_cv_*` — ответы на runtime-проверки configure, которые при
     кросс-компиляции невозможны (job control есть — Этап 6e).

3. `make -j8` (собирается `bash` + все `*.o`).

4. Склейка и зачистка:

   ```sh
   ld -r build/*.o build/builtins/*.o build/lib/glob/*.o \
        build/lib/readline/*.o build/lib/sh/*.o build/lib/tilde/*.o \
        -o bash_combined.o     # порядок не важен, ld -r без libc
   objcopy --strip-debug bash_combined.o third_party/bash/bash.o
   ```

   (Точный набор каталогов с .o — все, куда make раскладывает объекты;
   главное НЕ включать libc и не линковать окончательно.)

## Проверка

* В `bash.elf` не должно быть ни одного «сырого» `syscall` (0F 05):
  `objdump -d bash.elf | grep -c '0f 05'` → 0. Все системные вызовы musl
  идут через `int $0x81` (Linux-шлюз ядра).
* Запуск: `run bin/bash.elf` в шелле ОС (ввод/вывод процесса — COM1).
