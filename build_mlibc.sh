#!/bin/bash
set -e

# Configuration
MLIBC_REPO="https://github.com/managarm/mlibc.git"
MLIBC_DIR="mlibc"
SYSDEPS_SRC="sdk/mlibc/sysdeps/equinox"
BUILD_DIR="mlibc_build"
CROSS_FILE="equinox-cross.txt"
INSTALL_PREFIX="$(pwd)/sdk"

echo "=== EquinoxOS mlibc Build Script ==="

# 1. Clone mlibc if missing
if [ ! -d "$MLIBC_DIR" ]; then
    echo "[*] Cloning mlibc repository..."
    git clone "$MLIBC_REPO" "$MLIBC_DIR"
else
    echo "[*] mlibc repository already exists."
fi

# 2. Inject sysdeps
echo "[*] Injecting Equinox sysdeps..."

# Create sysdeps source directory if missing (injector)
# Always regenerate to ensure correct meson.build
if [ -d "$SYSDEPS_SRC" ]; then
    rm -rf "$SYSDEPS_SRC"
fi
echo "[*] Creating sysdeps stub files..."
mkdir -p "$SYSDEPS_SRC/include"
mkdir -p "$SYSDEPS_SRC/src"

# Create minimal meson.build for equinox sysdeps
cat > "$SYSDEPS_SRC/meson.build" <<'SYSDEPS_MESON'
# EquinoxOS sysdeps meson.build

sysdep_supported_options = {
    'posix': false,
    'linux': false,
    'glibc': false,
    'bsd': false,
}

rtld_sources += files(
    'src/sysdeps.cpp',
)
rtld_include_dirs += include_directories('include')

libc_sources += files(
    'src/sysdeps.cpp',
)
libc_include_dirs += include_directories('include')
SYSDEPS_MESON

# Create sysdeps.cpp with EquinoxOS syscall implementations
echo "[*] Creating sysdeps.cpp with syscall implementations..."
cat > "$SYSDEPS_SRC/src/sysdeps.cpp" <<'SYSDEPS_CPP'
#include <mlibc/tcb.hpp>
#include <abi-bits/errno.h>
#include <bits/ensure.h>
#include <mlibc/all-sysdeps.hpp>
#include <string.h>
#include <stdint.h>

// Syscall numbers for EquinoxOS
#define SYS_PRINT       1
#define SYS_READ_FILE   2
#define SYS_WRITE_FILE  3
#define SYS_DRAW_BUFFER 5
#define SYS_GET_TIME    6
#define SYS_GET_SCANCODE 9
#define SYS_EXIT        10
#define SYS_YIELD       11
#define SYS_GET_FONT    12
#define SYS_SLEEP       13
#define SYS_MMAP        14
#define SYS_WRITE       16
#define SYS_GETPID      17
#define SYS_GETPPID     18
#define SYS_GETTID      19

// x86_64 syscall via int 0x80
// EquinoxOS uses: rax=syscall_num, rdi=arg1, rsi=arg2, rdx=arg3, r8=arg4, r9=arg5
static inline long syscall0(long n) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n) : "memory");
    return ret;
}

static inline long syscall1(long n, long a1) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1) : "memory");
    return ret;
}

static inline long syscall2(long n, long a1, long a2) {
    long ret;
    register long rsi __asm__("rsi") = a2;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1), "r"(rsi) : "memory");
    return ret;
}

static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    register long rsi __asm__("rsi") = a2;
    register long rdx __asm__("rdx") = a3;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1), "r"(rsi), "r"(rdx) : "memory");
    return ret;
}

static inline long syscall4(long n, long a1, long a2, long a3, long a4) {
    long ret;
    register long rsi __asm__("rsi") = a2;
    register long rdx __asm__("rdx") = a3;
    register long r8 __asm__("r8") = a4;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1), "r"(rsi), "r"(rdx), "r"(r8) : "memory");
    return ret;
}

namespace mlibc {

void Sysdeps<LibcPanic>::operator()() {
    sysdep<LibcLog>("!!! mlibc panic !!!");
    sysdep<Exit>(-1);
    __builtin_trap();
}

void Sysdeps<LibcLog>::operator()(const char *message) {
    syscall1(SYS_PRINT, (long)message);
}

int Sysdeps<Isatty>::operator()(int fd) {
    (void)fd;
    return 0;
}

int Sysdeps<Write>::operator()(int fd, const void *buf, size_t count, ssize_t *bytes_written) {
    if (fd == 1 || fd == 2) {
        long ret = syscall3(SYS_WRITE, fd, (long)buf, count);
        *bytes_written = ret;
        return 0;
    }
    *bytes_written = -1;
    return EBADF;
}

int Sysdeps<TcbSet>::operator()(void *pointer) {
    uintptr_t thread_data = reinterpret_cast<uintptr_t>(pointer) + sizeof(Tcb);
    uint32_t low = thread_data & 0xFFFFFFFF;
    uint32_t high = thread_data >> 32;
    __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"(low), "d"(high));
    return 0;
}

int Sysdeps<AnonAllocate>::operator()(size_t size, void **pointer) {
    long ret = syscall1(SYS_MMAP, size);
    if (ret == 0) return ENOMEM;
    *pointer = (void *)ret;
    return 0;
}

int Sysdeps<AnonFree>::operator()(void *pointer, size_t size) {
    (void)pointer; (void)size;
    return 0;
}

int Sysdeps<Seek>::operator()(int fd, off_t offset, int whence, off_t *new_offset) {
    (void)fd; (void)offset; (void)whence; (void)new_offset;
    return ESPIPE;
}

void Sysdeps<Exit>::operator()(int status) {
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

int Sysdeps<Close>::operator()(int fd) {
    (void)fd;
    return 0;
}

int Sysdeps<FutexWake>::operator()(int *pointer, bool all) {
    (void)pointer; (void)all;
    return 0;
}

int Sysdeps<FutexWait>::operator()(int *pointer, int expected, const timespec *time) {
    (void)pointer; (void)expected; (void)time;
    return 0;
}

int Sysdeps<Read>::operator()(int fd, void *buf, size_t count, ssize_t *bytes_read) {
    (void)fd; (void)buf; (void)count;
    *bytes_read = 0;
    return 0;
}

int Sysdeps<Open>::operator()(const char *pathname, int flags, mode_t mode, int *fd) {
    (void)pathname; (void)flags; (void)mode; (void)fd;
    return ENOSYS;
}

int Sysdeps<VmMap>::operator()(void *hint, size_t size, int prot, int flags, int fd, off_t offset, void **window) {
    (void)hint; (void)prot; (void)flags; (void)fd; (void)offset;
    long ret = syscall1(SYS_MMAP, size);
    if (ret == 0) return ENOMEM;
    *window = (void *)ret;
    return 0;
}

int Sysdeps<VmUnmap>::operator()(void *pointer, size_t size) {
    (void)pointer; (void)size;
    return 0;
}

int Sysdeps<ClockGet>::operator()(int clock, time_t *secs, long *nanos) {
    (void)clock;
    long time_ms = syscall0(SYS_GET_TIME);
    if (secs) *secs = time_ms / 1000;
    if (nanos) *nanos = (time_ms % 1000) * 1000000;
    return 0;
}

} // namespace mlibc
SYSDEPS_CPP

# Copy all abi-bits headers from demo sysdeps as a starting point
mkdir -p "$SYSDEPS_SRC/include/abi-bits"
if [ -d "$MLIBC_DIR/sysdeps/demo/include/abi-bits" ]; then
    cp -r "$MLIBC_DIR/sysdeps/demo/include/abi-bits/"* "$SYSDEPS_SRC/include/abi-bits/"
else
    # Fallback: create minimal abi-bits headers if demo not available
    cat > "$SYSDEPS_SRC/include/abi-bits/limits.h" <<'LIMITS_H'
#ifndef _ABIBITS_LIMITS_H
#define _ABIBITS_LIMITS_H
#define __MLIBC_IOV_MAX 1024
#define __MLIBC_LOGIN_NAME_MAX 256
#define __MLIBC_HOST_NAME_MAX 64
#define __MLIBC_NAME_MAX 255
#define __MLIBC_OPEN_MAX 256
#endif
LIMITS_H
fi

# Create mlibc/sysdeps.hpp - required header defining sysdep tags
mkdir -p "$SYSDEPS_SRC/include/mlibc"
cat > "$SYSDEPS_SRC/include/mlibc/sysdeps.hpp" <<'SYSDEPS_HPP'
#pragma once

#include <mlibc/sysdep-signatures.hpp>

namespace mlibc {

struct EquinoxSysdepTags :
    LibcPanic,
    LibcLog,
    Isatty,
    Write,
    TcbSet,
    AnonAllocate,
    AnonFree,
    Seek,
    Exit,
    Close,
    FutexWake,
    FutexWait,
    Read,
    Open,
    VmMap,
    VmUnmap,
    ClockGet
{};

template<typename Tag>
using Sysdeps = SysdepOf<EquinoxSysdepTags, Tag>;

} // namespace mlibc
SYSDEPS_HPP

echo "[*] Stub files created successfully."

mkdir -p "$MLIBC_DIR/sysdeps/equinox"
cp -r $SYSDEPS_SRC/* "$MLIBC_DIR/sysdeps/equinox/"

# 3. Create Meson cross-file
echo "[*] Creating Meson cross-file ($CROSS_FILE)..."
cat > $CROSS_FILE <<EOF
[binaries]
c = 'x86_64-elf-gcc'
cpp = 'x86_64-elf-g++'
ar = 'x86_64-elf-ar'
strip = 'x86_64-elf-strip'

[host_machine]
system = 'equinox'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[built-in options]
c_args = ['-ffreestanding', '-D__clang__', '-D_GNU_SOURCE']
cpp_args = ['-ffreestanding', '-fno-exceptions', '-fno-rtti', '-D__clang__', '-D_GNU_SOURCE']
EOF

# 4. Patch mlibc/meson.build to accept 'equinox' system
echo "[*] Registering 'equinox' in mlibc/meson.build..."
if ! grep -q "'equinox'" "$MLIBC_DIR/meson.build"; then
    # We insert our elif block before the demo-sysdeps anchor
    sed -i "/# ANCHOR: demo-sysdeps/i elif host_machine.system() == 'equinox'\n    rtld_include_dirs += include_directories('sysdeps/equinox/include')\n    libc_include_dirs += include_directories('sysdeps/equinox/include')\n    subdir('sysdeps/equinox')\n" "$MLIBC_DIR/meson.build"
fi

# 5. Build and Install
echo "[*] Setting up Meson build directory..."
if [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
fi

# Note: Removed -Dsysdeps=equinox as it is determined by host_machine.system
meson setup "$BUILD_DIR" "$MLIBC_DIR" \
    --cross-file "$CROSS_FILE" \
    -Dheaders_only=false \
    -Ddefault_library=static \
    --prefix="$INSTALL_PREFIX"

echo "[*] Compiling mlibc..."
ninja -C "$BUILD_DIR"

echo "[*] Installing mlibc to $INSTALL_PREFIX..."
ninja -C "$BUILD_DIR" install

echo "=== Build Complete! ==="
echo "mlibc is now installed in sdk/lib and sdk/include"
