# BearSSL — vendored

Upstream: <https://www.bearssl.org/> — version **0.6** (2018-08-14, the
latest stable release at vendor time).

This is a verbatim copy of the upstream `src/` and `inc/` trees plus
`LICENSE.txt`. **No source files have been patched.** All EquinoxOS-side
adaptation lives outside this directory:

* `sdk/include/limits.h` — minimal `limits.h` so `inner.h`'s 32/64-bit
  probe works.
* `Makefile` — compiles every `.c` in `third_party/bearssl/src/**` into
  `third_party/bearssl/libbearssl.a` and adds the include path. The
  following defines disable platform-specific seeders we cannot use:

      -DBR_USE_URANDOM=0      # no Unix /dev/urandom — we have SYS_GETRANDOM
      -DBR_USE_WIN32_RAND=0   # not Windows
      -DBR_64=1               # force the 64-bit codepath (we're x86_64)

  We do NOT enable `BR_RDRAND` from inside BearSSL: BearSSL would emit
  an `__attribute__((target("rdrnd")))` function which our freestanding
  toolchain build flags do not always accept, and we get cleaner
  separation by seeding from userspace via `sys_getrandom()` (phase 3b
  shim) and feeding bytes through BearSSL's public `br_prng_class` API.

## What we deliberately leave out

* `bearssl-0.6/tools/` (the `brssl` CLI) — pulls in `<stdio.h>` /
  `<unistd.h>` heavily.
* `bearssl-0.6/test/` and `bearssl-0.6/samples/` — same reason.
* `bearssl-0.6/T0`, `bearssl-0.6/T0Comp.exe`, `bearssl-0.6/Doxyfile`,
  `bearssl-0.6/Makefile`, `bearssl-0.6/conf/`, `bearssl-0.6/mk/` —
  upstream build infrastructure we replace with our Makefile rules.

## How to refresh to a newer upstream

    rm -rf third_party/bearssl/src third_party/bearssl/inc
    curl -L https://www.bearssl.org/bearssl-X.Y.tar.gz | tar xz
    cp -r bearssl-X.Y/src third_party/bearssl/src
    cp -r bearssl-X.Y/inc third_party/bearssl/inc
    cp    bearssl-X.Y/LICENSE.txt third_party/bearssl/
    make libbearssl

If a new file shows up that pulls in `<sys/types.h>` or similar, add
the corresponding BR_USE_* override in the Makefile to disable the
code path — do not patch the vendored sources.
