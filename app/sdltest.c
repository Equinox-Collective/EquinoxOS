#include <SDL.h>

#define WIN_W 400
#define WIN_H 300

/* --- DEBUG: прямой вывод в ядерный лог (COM1) через SYS_PRINT (1) ---
 * SDL_Log на EquinoxOS может не доходить до серийника, поэтому печатаем
 * этапы напрямую сисколлом, чтобы поймать точку зависания. */
static void DBG(const char *s) {
    register unsigned long num __asm__("rax") = 1;          /* SYS_PRINT */
    register unsigned long a1  __asm__("rdi") = (unsigned long)s;
    __asm__ volatile("int $0x80"
                     : "+r"(num)
                     : "r"(a1)
                     : "rsi", "rdx", "rcx", "r8", "r9", "r10", "r11", "memory");
}

/* DBG + десятичное число */
static void DBGN(const char *s, unsigned long n) {
    DBG(s);
    char num[24]; int p = 0;
    char tmp[24]; int t = 0;
    if (n == 0) { tmp[t++] = '0'; }
    while (n) { tmp[t++] = (char)('0' + (n % 10)); n /= 10; }
    while (t) num[p++] = tmp[--t];
    num[p++] = '\n'; num[p] = 0;
    DBG(num);
}

int main(int argc, char* argv[]) {
    DBG("[SDLT] 1: main entered\n");
    /* Отключаем попытку SDL использовать texture/GPU framebuffer —
     * на EquinoxOS нет OpenGL, и неудачная попытка может оставить
     * window surface в невалидном состоянии. Форсируем software path. */
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    DBG("[SDLT] 2: before SDL_Init\n");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        SDL_Log("SDL Init Failed: %s\n", SDL_GetError());
        return 1;
    }

    DBG("[SDLT] 3: SDL_Init OK, before CreateWindow\n");
    SDL_Window* window = SDL_CreateWindow("Equinox SDL Test", 
                                          SDL_WINDOWPOS_UNDEFINED, 
                                          SDL_WINDOWPOS_UNDEFINED, 
                                          WIN_W, WIN_H, 0);
    if (!window) {
        SDL_Log("Window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    DBG("[SDLT] 4: window OK, before CreateRenderer\n");
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        SDL_Log("Renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_Log("SDL init OK, window and renderer created.\n");

    SDL_bool running = SDL_TRUE;
    SDL_Event event;
    
    int mouse_x = WIN_W / 2;
    int mouse_y = WIN_H / 2;
    SDL_bool mouse_pressed = SDL_FALSE;
    Uint8 r_offset = 0;

    DBG("[SDLT] 5: renderer OK, entering main loop\n");
    int frame = 0;
    while (running) {
        if (frame % 30 == 0) DBGN("[SDLT] HEARTBEAT frame=", (unsigned long)frame);
        if (frame == 0) DBG("[SDLT] 6: loop iter 0, before PollEvent\n");
        /* Обработка очереди событий (опрашивает наш PumpEvents) */
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = SDL_FALSE;
                    break;
                case SDL_KEYDOWN:
                    SDL_Log("[KEY] Scancode pressed: %d\n", event.key.keysym.scancode);
                    if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                        running = SDL_FALSE;
                    }
                    break;
                case SDL_MOUSEMOTION:
                    mouse_x = event.motion.x;
                    mouse_y = event.motion.y;
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        mouse_pressed = SDL_TRUE;
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        mouse_pressed = SDL_FALSE;
                    }
                    break;
            }
        }

        if (frame == 0) DBG("[SDLT] 7: PollEvent drained, before gradient\n");
        /* 1. Рисуем анимированный градиент на фоне */
        for (int y = 0; y < WIN_H; y++) {
            Uint8 r = (Uint8)(((y * 255) / WIN_H) + r_offset);
            Uint8 g = (Uint8)(128 + y / 4);
            Uint8 b = 200;
            SDL_SetRenderDrawColor(renderer, r, g, b, 255);
            SDL_RenderDrawLine(renderer, 0, y, WIN_W, y);
        }
        r_offset++;

        /* 2. Рисуем интерактивный квадрат в позиции курсора */
        SDL_Rect rect;
        rect.x = mouse_x - 20;
        rect.y = mouse_y - 20;
        rect.w = 40;
        rect.h = 40;

        if (mouse_pressed) {
            SDL_SetRenderDrawColor(renderer, 0, 255, 100, 255); /* Зеленый при клике */
        } else {
            SDL_SetRenderDrawColor(renderer, 255, 0, 100, 255); /* Красный обычно */
        }
        SDL_RenderFillRect(renderer, &rect);

        if (frame == 0) {
            /* PROBE: сравниваем буфер surface окна с тем, что блитится,
             * и пишем напрямую в surface->pixels красный/зелёный пиксель. */
            SDL_Surface *ws = SDL_GetWindowSurface(window);
            DBGN("[SDLT] PROBE ws_ptr=", (unsigned long)ws);
            if (ws) {
                DBGN("[SDLT] PROBE ws_pixels=", (unsigned long)ws->pixels);
                DBGN("[SDLT] PROBE ws_w=", (unsigned long)ws->w);
                DBGN("[SDLT] PROBE ws_h=", (unsigned long)ws->h);
                DBGN("[SDLT] PROBE ws_pitch=", (unsigned long)ws->pitch);
            }
        }
        if (frame == 0) DBG("[SDLT] 8: before RenderPresent\n");
        /* Выводим буфер на экран (дергает UpdateWindowFramebuffer) */
        SDL_RenderPresent(renderer);
        if (frame == 0) DBG("[SDLT] 9: after RenderPresent, before Delay\n");
        if (frame == 0) {
            /* ВИЗУАЛЬНЫЙ ТЕСТ: пишем magenta напрямую в surface окна, минуя
             * рендерер SDL, и презентим вручную. Если экран мигнёт пурпурным —
             * значит буфер+блит работают, а баг строго в draw-路ине SDL. */
            SDL_Surface *ws = SDL_GetWindowSurface(window);
            if (ws && ws->pixels) {
                Uint32 *p = (Uint32 *)ws->pixels;
                int n = ws->w * ws->h;
                for (int i = 0; i < n; i++) p[i] = 0xFFFF00FF; /* ARGB magenta */
                DBGN("[SDLT] manual magenta fill px0=", (unsigned long)p[0]);
                SDL_UpdateWindowSurface(window);
                DBG("[SDLT] manual present done -> screen should flash MAGENTA\n");
                SDL_Delay(1500);
            }
        }
        SDL_Delay(16); /* ~60 FPS */
        if (frame == 0) DBG("[SDLT] 10: after first Delay (frame 0 complete)\n");
        frame++;
        if (frame >= 150) {
            DBG("[SDLT] reached 150 frames -> auto-exit (NOT hung)\n");
            running = SDL_FALSE;
        }
    }
    DBG("[SDLT] 11: left main loop, cleaning up\n");

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}