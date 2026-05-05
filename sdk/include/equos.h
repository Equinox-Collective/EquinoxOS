#ifndef EQUOS_H
#define EQUOS_H

#include <stdint.h>

#define SYS_PRINT 1
#define SYS_READ_FILE 2
#define SYS_WRITE_FILE 3
#define SYS_DRAW_BUFFER 5
#define SYS_GET_TIME 6
#define SYS_GET_MOUSE_FULL 7
#define SYS_GET_SCANCODE 9
#define SYS_EXIT 10
#define SYS_YIELD 11
#define SYS_GET_FONT 12
#define SYS_SLEEP 13
#define SYS_MMAP 14
#define SYS_BRK 15
#define SYS_WRITE 16
#define SYS_AUDIO_PLAY 20
#define SYS_AUDIO_SET_RATE 21

// Переименовали в _syscall и всегда принимаем 5 аргументов + номер
static inline uint64_t _syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t ret;
    __asm__ volatile (
        "mov %1, %%rax; "
        "mov %2, %%rdi; "
        "mov %3, %%rsi; "
        "mov %4, %%rdx; "
        "mov %5, %%rcx; "
        "mov %6, %%r8; "
        "int $0x80; "
        "mov %%rax, %0; "
        : "=r"(ret)
        : "r"(num), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r8", "memory"
    );
    return ret;
}

static inline void* get_system_font() {
    return (void*)_syscall(SYS_GET_FONT, 0, 0, 0, 0, 0);
}

static inline void* sys_read_file(const char* name, uint32_t* out_size) {
    return (void*)_syscall(SYS_READ_FILE, (uint64_t)name,
                           (uint64_t)out_size, 0, 0, 0);
}

static inline uint32_t sys_write_file(const char* name, const void* buf, uint32_t size) {
    return (uint32_t)_syscall(SYS_WRITE_FILE, (uint64_t)name,
                              (uint64_t)buf, size, 0, 0);
}

static inline void sys_draw_buffer(int x, int y, int w, int h, uint32_t* buffer) {
    _syscall(SYS_DRAW_BUFFER, (uint64_t)x, (uint64_t)y, (uint64_t)w,
             (uint64_t)h, (uint64_t)buffer);
}

static inline uint8_t sys_get_scancode() {
    return (uint8_t)_syscall(SYS_GET_SCANCODE, 0, 0, 0, 0, 0);
}

static inline uint32_t sys_get_time_ms() {
    return (uint32_t)_syscall(SYS_GET_TIME, 0, 0, 0, 0, 0);
}

static inline uint32_t write_file(const char* name, const void* buf, uint32_t size) {
    return sys_write_file(name, buf, size);
}

static inline void sys_sleep(uint32_t ms) {
    _syscall(SYS_SLEEP, ms, 0, 0, 0, 0);
}

static inline void sleep(uint32_t ms) {
    sys_sleep(ms);
}

static inline void sys_audio_submit(void* buffer, uint32_t size) {
    _syscall(SYS_AUDIO_PLAY, (uint64_t)buffer, (uint64_t)size, 0, 0, 0);
}

static inline void sys_exit(int code) { _syscall(SYS_EXIT, code, 0, 0, 0, 0); }
static inline void sys_yield() { _syscall(SYS_YIELD, 0, 0, 0, 0, 0); }
static inline void *sys_get_font() {
  return (void *)_syscall(SYS_GET_FONT, 0, 0, 0, 0, 0);
}
#endif
