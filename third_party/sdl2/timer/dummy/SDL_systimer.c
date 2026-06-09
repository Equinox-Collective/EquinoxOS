#include "../../SDL_internal.h"
#include "../SDL_timer_c.h"

/* Сисколлы EquinoxOS */
static inline uint64_t equos_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    /* Явные регистровые constraints — см. подробный комментарий в
     * video/equinox/SDL_video_equinox.c. r9/r10/r11 помечаем clobbered. */
    register uint64_t r8v __asm__("r8") = a5;
    uint64_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3), "c"(a4), "r"(r8v)
                     : "r9", "r10", "r11", "memory");
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