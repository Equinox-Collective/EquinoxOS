#include <SDL.h>

#define WIN_W 400
#define WIN_H 300

int main(int argc, char* argv[]) {
    /* Отключаем попытку SDL использовать texture/GPU framebuffer —
     * на EquinoxOS нет OpenGL, и неудачная попытка может оставить
     * window surface в невалидном состоянии. Форсируем software path. */
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        SDL_Log("SDL Init Failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Equinox SDL Test", 
                                          SDL_WINDOWPOS_UNDEFINED, 
                                          SDL_WINDOWPOS_UNDEFINED, 
                                          WIN_W, WIN_H, 0);
    if (!window) {
        SDL_Log("Window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

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

    while (running) {
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

        /* Выводим буфер на экран (дергает UpdateWindowFramebuffer) */
        SDL_RenderPresent(renderer);
        SDL_Delay(16); /* ~60 FPS */
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}