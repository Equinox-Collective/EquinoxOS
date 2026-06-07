#include "../../SDL_internal.h"
#include "../SDL_timer_c.h"

/* Сисколлы EquinoxOS */
static inline uint64_t equos_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t ret;
    __asm__ volatile("mov %1, %%rax; "
                     "mov %2, %%rdi; "
                     "mov %3, %%rsi; "
                     "mov %4, %%rdx; "
                     "mov %5, %%rcx; "
                     "mov %6, %%r8; "
                     "int $0x80; "
                     "mov %%rax, %0; "
                     : "=r"(ret)
                     : "r"(num), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
                     : "rax", "rdi", "rsi", "rdx", "rcx", "r8", "memory");
    return ret;
}

static Uint64 start_ticks = 0;
static SDL_bool ticks_started = SDL_FALSE;

void SDL_TicksInit(void) {
    if (ticks_started) {
        return;
    }
    start_ticks = equos_syscall(6, 0, 0, 0, 0, 0); /* SYS_GET_TIME (6) */
    ticks_started = SDL_TRUE;
}

void SDL_TicksQuit(void) {
    ticks_started = SDL_FALSE;
}

Uint64 SDL_GetTicks64(void) {
    if (!ticks_started) {
        SDL_TicksInit();
    }
    Uint64 now = equos_syscall(6, 0, 0, 0, 0, 0); /* SYS_GET_TIME */
    return (now - start_ticks);
}

void SDL_Delay(Uint32 ms) {
    if (ms > 0) {
        equos_syscall(13, ms, 0, 0, 0, 0); /* SYS_SLEEP (13) */
    }
}