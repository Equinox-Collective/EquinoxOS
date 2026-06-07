#include "../../SDL_internal.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "SDL_video_equinox.h"
#include <stdint.h>

/* Кастомные обертки системных вызовов EquinoxOS */
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

/* Извлекаем координаты окна из RAX (X) и RBX (Y) */
static inline void equos_get_window_pos(int *x, int *y) {
    uint64_t rx = 0, rb = 0;
    __asm__ volatile(
        "mov $33, %%rax\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0\n\t"
        "mov %%rbx, %1\n\t"
        : "=r"(rx), "=r"(rb)
        :
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "memory"
    );
    if (x) *x = (int)rx;
    if (y) *y = (int)rb;
}

static int Equinox_VideoInit(SDL_VideoDevice *_this) {
    SDL_VideoDisplay display;
    SDL_DisplayMode current_mode;
    
    uint64_t width = 1024;
    uint64_t height = 768;
    
    /* Опрашиваем VESA информацию через сисколл 32 */
    uint64_t r_ax = 0, r_bx = 0, r_cx = 0, r_dx = 0;
    __asm__ volatile(
        "mov $32, %%rax\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0\n\t"
        "mov %%rbx, %1\n\t"
        "mov %%rcx, %2\n\t"
        "mov %%rdx, %3\n\t"
        : "=r"(r_ax), "=r"(r_bx), "=r"(r_cx), "=r"(r_dx)
        :
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "memory"
    );
    
    if (r_bx > 0) width = r_bx;
    if (r_cx > 0) height = r_cx;
    
    SDL_zero(current_mode);
    current_mode.w = width;
    current_mode.h = height;
    current_mode.refresh_rate = 60;
    current_mode.format = SDL_PIXELFORMAT_ARGB8888;
    
    SDL_zero(display);
    display.desktop_mode = current_mode;
    display.current_mode = current_mode;
    
    if (SDL_AddVideoDisplay(&display, SDL_FALSE) < 0) {
        return -1;
    }
    
    return 0;
}

static void Equinox_VideoQuit(SDL_VideoDevice *_this) {
    /* Очистка ресурсов при закрытии */
}

static int Equinox_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window) {
    /* Просто возвращаем 0, так как само окно создается силами композитора */
    return 0;
}

static void Equinox_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window) {
    /* Заглушка деструктора окна */
}

/* Создаем программный буфер кадра для окна */
static int Equinox_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, Uint32 *format, void **pixels, int *pitch) {
    int w = window->w;
    int h = window->h;
    
    void *buffer = SDL_malloc(w * h * 4); /* 32-битный ARGB */
    if (!buffer) {
        return SDL_OutOfMemory();
    }
    
    SDL_memset(buffer, 0, w * h * 4);
    
    window->driverdata = buffer;
    
    *format = SDL_PIXELFORMAT_ARGB8888;
    *pixels = buffer;
    *pitch = w * 4;
    
    return 0;
}

/* Копируем наш буфер на экран в координаты окна */
static int Equinox_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int numrects) {
    void *buffer = window->driverdata;
    if (!buffer) {
        return -1;
    }
    
    int x = 100;
    int y = 100;
    equos_get_window_pos(&x, &y);
    
    /* Сисколл 5 (SYS_DRAW_BUFFER) */
    equos_syscall(5, (uint64_t)x, (uint64_t)y, (uint64_t)window->w, (uint64_t)window->h, (uint64_t)buffer);
    
    return 0;
}

static void Equinox_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window) {
    if (window->driverdata) {
        SDL_free(window->driverdata);
        window->driverdata = NULL;
    }
}

static void Equinox_DeleteDevice(SDL_VideoDevice *device) {
    SDL_free(device);
}

static SDL_VideoDevice *Equinox_CreateDevice(void) {
    SDL_VideoDevice *device;
    
    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        return NULL;
    }
    
    device->VideoInit = Equinox_VideoInit;
    device->VideoQuit = Equinox_VideoQuit;
    device->CreateSDLWindow = Equinox_CreateWindow;
    device->DestroyWindow = Equinox_DestroyWindow;
    
    device->CreateWindowFramebuffer = Equinox_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = Equinox_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = Equinox_DestroyWindowFramebuffer;
    
    device->free = Equinox_DeleteDevice;
    
    return device;
}

static int Equinox_Available(void) {
    return 1;
}

VideoBootStrap EQUINOX_bootstrap = {
    "equinox", "Equinox OS Software Video Driver",
    Equinox_Available, Equinox_CreateDevice
};