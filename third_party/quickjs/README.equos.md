# QuickJS — vendored

Upstream: <https://github.com/quickjs-ng/quickjs> — version **v0.15.0**
(quickjs-ng fork of Fabrice Bellard's QuickJS; MIT licensed; the most
active 2025/2026 maintained line).

This is a verbatim copy of the upstream source files needed to build
the JS engine as a static library. **No source files have been
patched.** All EquinoxOS-side adaptation lives outside this directory:

* `sdk/include/pthread.h` — minimal `pthread_*` type/function decls
  (EquinoxOS is single-threaded — see stubs below).
* `sdk/include/alloca.h` — `#define alloca __builtin_alloca`.
* `sdk/include/sys/time.h` — `struct timeval` + `gettimeofday()` decl.
* `sdk/include/time.h` — extended with `clockid_t`, `CLOCK_*`,
  `struct timespec`, `clock_gettime`, `gmtime_r`, `localtime_r`.
* `sdk/include/math.h` — added `isnan`/`isinf`/`isfinite`/`signbit`/
  `copysign` as GCC built-in macros + decls for `sinh`/`cosh`/`cbrt`/
  `log2`/`round`/... (the C99 surface QuickJS uses).
* `sdk/include/errno.h` — added `ETIMEDOUT`.
* `sdk/lib/qjs_time.c` — `gettimeofday()` and `clock_gettime()` on top
  of `SYS_GET_WALL_TIME` (RTC seconds) + `SYS_GET_TIME` (PIT ms);
  UTC-only `gmtime_r`/`localtime_r` (no timezone DB on EquinoxOS).
* `sdk/lib/qjs_math.c` — simple but spec-correct implementations of
  the C99 math functions QuickJS uses.
* `sdk/lib/qjs_pthread_stubs.c` — no-op pthread implementations
  (single-threaded process model).
* `Makefile` — builds the four .c files below into
  `third_party/quickjs/libquickjs.a` with the `-D` flags described
  there.

## Build flags

      -DNO_TM_GMTOFF      # our struct tm has no tm_gmtoff field;
                          # fall back to the mktime(gmtime_r) -
                          # mktime(localtime_r) path (both return UTC
                          # → offset = 0 = correct on EquinoxOS).
      -D_GNU_SOURCE       # enables a few GNU-isms cutils.h expects.

`CONFIG_ATOMICS` is auto-defined by quickjs.c when the toolchain
supports `<stdatomic.h>` (it does — GCC ships it as part of its
freestanding header set).

`JS_HAVE_THREADS=1` is auto-defined too. We keep it on because
QuickJS' typedefs unconditionally reference `pthread_*` types, and
disabling threads outright requires touching upstream sources. All
actual thread/lock calls go through our no-op stubs.

## Sources we vendor

    quickjs.c                       # ~63k LOC — the engine
    quickjs.h
    quickjs-atom.h
    quickjs-opcode.h
    quickjs-c-atomics.h
    cutils.h                        # C helpers (inline)
    list.h                          # intrusive doubly-linked list
    libregexp.c / .h / -opcode.h    # JS regex engine
    libunicode.c / .h / -table.h    # Unicode 16 tables
    dtoa.c / .h                     # IEEE-754 ↔ string
    builtin-array-fromasync.h       # pre-compiled built-ins
    builtin-iterator-zip.h
    builtin-iterator-zip-keyed.h
    LICENSE

## What we deliberately leave out

* `quickjs-libc.{c,h}` — std module bindings, pulls in `<unistd.h>`,
  `<sys/wait.h>`, signals, fork, etc. We will build our own minimal
  host instead (phase J1).
* `qjs.c`, `qjsc.c`, `qjs-wasi-reactor.c` — REPL / compiler /
  WASI entry points.
* `gen/` — example precompiled JS bundles.
* `api-test.c`, `ctest.c`, `cxxtest.cc`, `fuzz.c`, `lre-test.c`,
  `run-test262.c`, `unicode_gen*` — upstream test/fuzz/codegen
  utilities.
* `CMakeLists.txt`, `Makefile`, `meson.build` — upstream build
  infrastructure we replace with our Makefile rules.

## How to refresh to a newer upstream

    rm /work/repos/EquinoxOS/third_party/quickjs/*.{c,h}
    curl -L https://github.com/quickjs-ng/quickjs/archive/refs/tags/vX.Y.Z.tar.gz | tar xz
    cd quickjs-X.Y.Z
    cp quickjs.{c,h} quickjs-atom.h quickjs-opcode.h quickjs-c-atomics.h \
       cutils.h list.h \
       libregexp.{c,h} libregexp-opcode.h \
       libunicode.{c,h} libunicode-table.h \
       dtoa.{c,h} \
       builtin-array-fromasync.h builtin-iterator-zip.h builtin-iterator-zip-keyed.h \
       LICENSE \
       ../third_party/quickjs/
    make libquickjs

If a new file pulls in `<sys/wait.h>` / `<unistd.h>` / similar, add
the corresponding -D override or a new SDK header — do not patch the
vendored sources.
