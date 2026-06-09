#include "../../SDL_internal.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "../../events/scancodes_ascii.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "SDL_video_equinox.h"

/* Кастомные обертки системных вызовов EquinoxOS.
 *
 * Используем явные регистровые constraints вместо ручного `mov %N, %%reg`.
 * Старый вариант (a) полагался на то, что компилятор сам разложит входы по
 * регистрам, а потом перекладывал их инструкциями mov — это легко даёт
 * конфликт, если GCC поместит вход в регистр, который asm перетирает; и
 * (b) не помечал r9/r10/r11 как clobbered. r10/r11 — caller-saved в SysV ABI,
 * поэтому ядро вправе их затирать; держать в них живые значения через
 * inline `int 0x80` нельзя. Теперь GCC сам спилит то, что нужно. */
static inline uint64_t equos_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    register uint64_t r8v __asm__("r8") = a5;
    uint64_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3), "c"(a4), "r"(r8v)
                     : "r9", "r10", "r11", "memory");
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
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory"
    );
    if (x) *x = (int)rx;
    if (y) *y = (int)rb;
}

/* Таблица маппинга PS/2 Set 1 скан-кодов на SDL Scancodes */
static const SDL_Scancode equos_scancode_table[128] = {
    [0x01] = SDL_SCANCODE_ESCAPE,
    [0x02] = SDL_SCANCODE_1,
    [0x03] = SDL_SCANCODE_2,
    [0x04] = SDL_SCANCODE_3,
    [0x05] = SDL_SCANCODE_4,
    [0x06] = SDL_SCANCODE_5,
    [0x07] = SDL_SCANCODE_6,
    [0x08] = SDL_SCANCODE_7,
    [0x09] = SDL_SCANCODE_8,
    [0x0A] = SDL_SCANCODE_9,
    [0x0B] = SDL_SCANCODE_0,
    [0x0C] = SDL_SCANCODE_MINUS,
    [0x0D] = SDL_SCANCODE_EQUALS,
    [0x0E] = SDL_SCANCODE_BACKSPACE,
    [0x0F] = SDL_SCANCODE_TAB,
    [0x10] = SDL_SCANCODE_Q,
    [0x11] = SDL_SCANCODE_W,
    [0x12] = SDL_SCANCODE_E,
    [0x13] = SDL_SCANCODE_R,
    [0x14] = SDL_SCANCODE_T,
    [0x15] = SDL_SCANCODE_Y,
    [0x16] = SDL_SCANCODE_U,
    [0x17] = SDL_SCANCODE_I,
    [0x18] = SDL_SCANCODE_O,
    [0x19] = SDL_SCANCODE_P,
    [0x1C] = SDL_SCANCODE_RETURN,
    [0x1D] = SDL_SCANCODE_LCTRL,
    [0x1E] = SDL_SCANCODE_A,
    [0x1F] = SDL_SCANCODE_S,
    [0x20] = SDL_SCANCODE_D,
    [0x21] = SDL_SCANCODE_F,
    [0x22] = SDL_SCANCODE_G,
    [0x23] = SDL_SCANCODE_H,
    [0x24] = SDL_SCANCODE_J,
    [0x25] = SDL_SCANCODE_K,
    [0x26] = SDL_SCANCODE_L,
    [0x2C] = SDL_SCANCODE_Z,
    [0x2D] = SDL_SCANCODE_X,
    [0x2E] = SDL_SCANCODE_C,
    [0x2F] = SDL_SCANCODE_V,
    [0x30] = SDL_SCANCODE_B,
    [0x31] = SDL_SCANCODE_N,
    [0x32] = SDL_SCANCODE_M,
    [0x39] = SDL_SCANCODE_SPACE,
    [0x2A] = SDL_SCANCODE_LSHIFT,
    [0x36] = SDL_SCANCODE_RSHIFT,
    [0x38] = SDL_SCANCODE_LALT,
    [0x48] = SDL_SCANCODE_UP,
    [0x50] = SDL_SCANCODE_DOWN,
    [0x4B] = SDL_SCANCODE_LEFT,
    [0x4D] = SDL_SCANCODE_RIGHT,
};

static int Equinox_VideoInit(SDL_VideoDevice *_this) {
    SDL_VideoDisplay display;
    SDL_DisplayMode current_mode;
    
    uint64_t width = 1024;
    uint64_t height = 768;
    
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
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory"
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
}

/* Структура данных окна: позиция + framebuffer */
typedef struct {
    int win_x;
    int win_y;
    void *framebuffer;
} EquinoxWindowData;

static int Equinox_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window) {
    /* Получаем стартовую позицию из ядра (может быть 100,100 — дефолт) */
    EquinoxWindowData *data = (EquinoxWindowData *)SDL_malloc(sizeof(EquinoxWindowData));
    if (!data) {
        return SDL_OutOfMemory();
    }

    int x = 100, y = 100;
    equos_get_window_pos(&x, &y);
    data->win_x = x;
    data->win_y = y;
    data->framebuffer = NULL;
    window->driverdata = data;

    return 0;
}

static void Equinox_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window) {
    if (window->driverdata) {
        EquinoxWindowData *data = (EquinoxWindowData *)window->driverdata;
        if (data->framebuffer) {
            SDL_free(data->framebuffer);
            data->framebuffer = NULL;
        }
        SDL_free(data);
        window->driverdata = NULL;
    }
}

static int Equinox_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, Uint32 *format, void **pixels, int *pitch) {
    int w = window->w;
    int h = window->h;

    EquinoxWindowData *data = (EquinoxWindowData *)window->driverdata;
    if (!data) {
        return -1;
    }

    void *buffer = SDL_malloc(w * h * 4);
    if (!buffer) {
        return SDL_OutOfMemory();
    }

    SDL_memset(buffer, 0, w * h * 4);
    data->framebuffer = buffer;

    *format = SDL_PIXELFORMAT_ARGB8888;
    *pixels = buffer;
    *pitch = w * 4;

    return 0;
}

static int Equinox_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int numrects) {
    EquinoxWindowData *data = (EquinoxWindowData *)window->driverdata;
    if (!data || !data->framebuffer) {
        return -1;
    }

    /* Обновляем позицию окна из ядра только если ядро вернуло ненулевые
     * координаты (значит enGUI активно управляет окном через app_container).
     * Если ядро вернуло (0,0) — enGUI сбросил позицию (SDL-приложение
     * запущено не через контейнер), используем последнюю известную позицию. */
    int kernel_x = 0, kernel_y = 0;
    equos_get_window_pos(&kernel_x, &kernel_y);
    if (kernel_x > 0 || kernel_y > 0) {
        data->win_x = kernel_x;
        data->win_y = kernel_y;
    }

    int x = data->win_x;
    int y = data->win_y;

    /* Сообщаем ядру актуальную позицию и размер перед блитом */
    equos_syscall(36, (uint64_t)x, (uint64_t)y, (uint64_t)window->w, (uint64_t)window->h, 0);
    equos_syscall(5, (uint64_t)x, (uint64_t)y, (uint64_t)window->w, (uint64_t)window->h, (uint64_t)data->framebuffer);
    return 0;
}

static void Equinox_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window) {
    EquinoxWindowData *data = (EquinoxWindowData *)window->driverdata;
    if (data && data->framebuffer) {
        SDL_free(data->framebuffer);
        data->framebuffer = NULL;
    }
}

/* --- ОБРАБОТКА ВВОДА КЛАВИАТУРЫ И МЫШИ (PUMP EVENTS) --- */
static void Equinox_PumpEvents(SDL_VideoDevice *_this) {
    /* 1. Клавиатура */
    static SDL_bool pending_extended = SDL_FALSE;
    while (1) {
        uint8_t scancode = (uint8_t)equos_syscall(9, 0, 0, 0, 0, 0);
        if (scancode == 0) {
            break;
        }
        
        if (scancode == 0xE0) {
            pending_extended = SDL_TRUE;
            continue;
        }
        
        SDL_bool released = (scancode & 0x80) ? SDL_TRUE : SDL_FALSE;
        uint8_t key_index = scancode & ~0x80;
        
        if (key_index < 128) {
            SDL_Scancode sdl_scancode = equos_scancode_table[key_index];
            if (sdl_scancode != SDL_SCANCODE_UNKNOWN) {
                SDL_SendKeyboardKey(released ? SDL_RELEASED : SDL_PRESSED, sdl_scancode);
            }
        }
        pending_extended = SDL_FALSE;
    }
    
    /* 2. Мышь */
    uint64_t mx = 0, my = 0, m_btn = 0;
    __asm__ volatile(
        "mov $7, %%rax\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0\n\t"
        "mov %%rbx, %1\n\t"
        "mov %%rcx, %2\n\t"
        : "=r"(mx), "=r"(my), "=r"(m_btn)
        :
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory"
    );
    
    int win_x = 100;
    int win_y = 100;
    equos_get_window_pos(&win_x, &win_y);
    
    /* Переводим глобальные координаты мыши в локальные оконные */
    int local_x = (int)mx - win_x;
    int local_y = (int)my - win_y;
    
    SDL_Window *focus = _this->windows;
    if (focus) {
        /* Передаем координаты только если мышь находится в границах окна */
        if (local_x >= 0 && local_x < focus->w && local_y >= 0 && local_y < focus->h) {
            SDL_SendMouseMotion(focus, 0, 0, local_x, local_y);
            
            static Uint8 last_buttons = 0;
            Uint8 current_buttons = 0;
            if (m_btn & 1) current_buttons |= SDL_BUTTON_LMASK;
            if (m_btn & 2) current_buttons |= SDL_BUTTON_RMASK;
            
            /* Сравниваем маску кнопок */
            if ((current_buttons & SDL_BUTTON_LMASK) != (last_buttons & SDL_BUTTON_LMASK)) {
                SDL_SendMouseButton(focus, 0, (current_buttons & SDL_BUTTON_LMASK) ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_LEFT);
            }
            if ((current_buttons & SDL_BUTTON_RMASK) != (last_buttons & SDL_BUTTON_RMASK)) {
                SDL_SendMouseButton(focus, 0, (current_buttons & SDL_BUTTON_RMASK) ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_RIGHT);
            }
            last_buttons = current_buttons;
        }
    }
}

static void Equinox_DeleteDevice(SDL_VideoDevice *device) {
    SDL_free(device);
}

static SDL_VideoDevice *Equinox_CreateDevice(void) {
    SDL_VideoDevice *device;
    
    /* Используем malloc + memset вместо calloc */
    device = (SDL_VideoDevice *)SDL_malloc(sizeof(SDL_VideoDevice));
    if (!device) {
        return NULL;
    }
    SDL_memset(device, 0, sizeof(SDL_VideoDevice));
    
    device->VideoInit = Equinox_VideoInit;
    device->VideoQuit = Equinox_VideoQuit;
    device->CreateSDLWindow = Equinox_CreateWindow;
    device->DestroyWindow = Equinox_DestroyWindow;
    
    device->CreateWindowFramebuffer = Equinox_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = Equinox_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = Equinox_DestroyWindowFramebuffer;
    
    device->PumpEvents = Equinox_PumpEvents;
    device->free = Equinox_DeleteDevice;
    
    return device;
}

static int Equinox_Available(void) {
    return 1;
}

VideoBootStrap EQUINOX_bootstrap = {
    "equinox", "Equinox OS Software Video Driver",
    Equinox_CreateDevice,
    NULL /* no ShowMessageBox implementation */
};