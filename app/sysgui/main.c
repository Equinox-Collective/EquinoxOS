#include "api_gui.h"
#include "lua/lauxlib.h"
#include "lua/lua.h"
#include "lua/lualib.h"
#include <eid.h>
#include <eid_ext.h>
#include <equos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

uint32_t *vram = NULL;
uint32_t *backbuffer = NULL;
uint32_t *draw_target = NULL; // Наша цель для всех функций отрисовки в Lua API
uint32_t screen_w = 1024;
uint32_t screen_h = 768;

static inline void fast_memcpy_sse(void *dest, const void *src, size_t bytes) {
  size_t blocks = bytes / 64;
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  for (size_t i = 0; i < blocks; i++) {
    __asm__ volatile("movups 0(%0), %%xmm0\n"
                     "movups 16(%0), %%xmm1\n"
                     "movups 32(%0), %%xmm2\n"
                     "movups 48(%0), %%xmm3\n"
                     "movntdq %%xmm0, 0(%1)\n"
                     "movntdq %%xmm1, 16(%1)\n"
                     "movntdq %%xmm2, 32(%1)\n"
                     "movntdq %%xmm3, 48(%1)\n"
                     :
                     : "r"(s), "r"(d)
                     : "xmm0", "xmm1", "xmm2", "xmm3", "memory");
    s += 64;
    d += 64;
  }

  size_t remaining = bytes % 64;
  for (size_t i = 0; i < remaining; i++) {
    d[i] = s[i];
  }

  __asm__ volatile("sfence" ::: "memory");
}

eid_ctx_t eid_ctx;

extern bool is_any_anim_active(void);

// Отрисовка курсора в пользовательском пространстве поверх бэкбуфера
void draw_cursor_user(uint32_t *fb, int x, int y, int w, int h) {
  static const int cursor_map[8][8] = {
      {2, 0, 0, 0, 0, 0, 0, 0}, {2, 2, 0, 0, 0, 0, 0, 0},
      {2, 1, 2, 0, 0, 0, 0, 0}, {2, 1, 1, 2, 0, 0, 0, 0},
      {2, 1, 1, 1, 2, 0, 0, 0}, {2, 1, 1, 1, 1, 2, 0, 0},
      {2, 2, 2, 2, 2, 2, 2, 0}, {0, 0, 2, 2, 2, 0, 0, 0}};
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 8; j++) {
      int px = x + j;
      int py = y + i;
      if (px >= 0 && px < w && py >= 0 && py < h) {
        if (cursor_map[i][j] == 1)
          fb[py * w + px] = 0xFFFFFF;
        else if (cursor_map[i][j] == 2)
          fb[py * w + px] = 0x000000;
      }
    }
  }
}

int main(int argc, char **argv) {
  uint64_t phys_fb = 0;
  uint64_t width = 0;
  uint64_t height = 0;
  uint64_t pitch = 0;

  // Опрос физического фреймбуфера
  __asm__ volatile("mov $32, %%rax\n"
                   "int $0x80\n"
                   : "=a"(phys_fb), "=b"(width), "=c"(height), "=d"(pitch));

  screen_w = (uint32_t)width;
  screen_h = (uint32_t)height;

  // Мапим физическую видеопамять (фронтбуфер)
  vram = (uint32_t *)_syscall(SYS_MAP_PHYS, phys_fb, screen_w * screen_h * 4, 0,
                              0, 0);

  // Выделяем локальный бэкбуфер для двойной буферизации
  backbuffer = (uint32_t *)malloc(screen_w * screen_h * 4);
  memset(backbuffer, 0, screen_w * screen_h * 4);

  // Присваиваем бэкбуфер как цель для Lua-отрисовки
  draw_target = backbuffer;

  eid_init();
  memset(&eid_ctx, 0, sizeof(eid_ctx));

  lua_State *L = luaL_newstate();
  luaL_openlibs(L);

  register_gui_api(L);

  if (luaL_dofile(L, "res/sysgui/init.lua")) {
    printf("enGUI Lua Error: %s\n", lua_tostring(L, -1));
    return 1;
  }

  /*
   * SYS_GET_TIME now returns milliseconds-since-boot directly: the kernel
   * runs PIT at 1 kHz (`init_timer(1000)` in EquinoxOS' kernel.c) and
   * exposes `tick` as the raw ms counter via the syscall (see kernel.c /
   * syscall.c, case 6). So one returned unit == 1 ms of wall-clock time.
   *
   * Historical note: when PIT ran at 50 Hz and the syscall returned
   * `tick * 10`, TICK_MS used to be 2. Both have since changed in
   * lock-step; keeping TICK_MS at the old value made every `dt` 2× too
   * large and animations played at 2× speed.
   */
  const uint32_t TICK_MS = 1;
  /*
   * Frame cap = 16 ms (~60 FPS). Ранее пробовал 8 ms ради сглаживания
   * под 200 Hz PS/2-мышь, но на медленных конфигурациях (TCG без KVM/WHPX
   * + debug-флаги `-d int,...` в `make run`) это удваивает количество
   * полных перерисовок в секунду без реального прироста FPS и только
   * выедает CPU. 16 ms — разумный кэп, для сглаживания drag'а реальный
   * фикс лежит в Makefile (`-accel whpx` и убрать `-d int`).
   */
  const uint32_t TARGET_FRAME_MS = 16;
  int last_mx = -9999, last_my = -9999;
  int last_mdown = -1;
  uint8_t last_key = 0;
  uint32_t force_frames = 4;

  uint32_t last_tick = (uint32_t)_syscall(SYS_GET_TIME, 0, 0, 0, 0, 0);
  uint32_t frame_start = last_tick;

  while (1) {
    // Если запущено полно­экранное приложение (doom, snake, bmpview...),
    // оно само владеет vram через SYS_DRAW_BUFFER. В этом случае нам
    // ни рисовать в backbuffer (логика интерфейса не видна), ни — что
    // важнее — копировать backbuffer во фронт (он сотрёт кадр игры).
    // Спим подольше и идём дальше.
    uint64_t fg = _syscall(SYS_GET_FG_APP, 0, 0, 0, 0, 0);
    // Защита от рассинхрона: старое ядро без case 74 в syscall-
    // диспетчере вернёт сам номер syscall (RAX остаётся равным 74,
    // т.к. default-ветка ничего не пишет в regs->rax). Если у юзера
    // обновился sysgui, но не пересобралось ядро, без этой проверки
    // sysgui решит, что есть foreground-app, и навсегда перестанет
    // композитить — экран намертво застывает. Подстраховываемся.
    if (fg == SYS_GET_FG_APP)
      fg = 0;
    if (fg != 0) {
      sys_sleep(50);
      // Когда foreground-app исчезнет, форсируем несколько кадров
      // полной перерисовки, чтобы курсор/анимации вернулись на свои
      // места без артефактов от чужих кадров.
      force_frames = 4;
      last_mx = -9999; last_my = -9999;
      continue;
    }

    uint64_t mx = 0, my = 0, m_btn = 0;
    __asm__ volatile("mov $7, %%rax\n int $0x80"
                     : "=a"(mx), "=b"(my), "=c"(m_btn));
    int cur_mx = (int)mx;
    int cur_my = (int)my;
    int cur_mdown = (int)((m_btn & 1) != 0);

    uint8_t cur_key = (uint8_t)_syscall(SYS_GET_SCANCODE, 0, 0, 0, 0, 0);

    int need_redraw = (force_frames > 0) || (cur_mx != last_mx) ||
                      (cur_my != last_my) || (cur_mdown != last_mdown) ||
                      (cur_key != 0 && cur_key != last_key) ||
                      is_any_anim_active();

    uint32_t now = (uint32_t)_syscall(SYS_GET_TIME, 0, 0, 0, 0, 0);

    if (need_redraw) {
      uint32_t elapsed = now - last_tick;
      float dt = (float)(elapsed * TICK_MS);
      if (dt > 200.0f)
        dt = 200.0f;

      // Отрисовываем всё в бэкбуфер
      eid_begin(&eid_ctx, backbuffer, screen_w, screen_h);
      eid_ctx.mx = cur_mx;
      eid_ctx.my = cur_my;
      eid_ctx.m_down = cur_mdown;
      eid_ctx.last_key = cur_key;

      lua_getglobal(L, "on_tick");
      if (lua_isfunction(L, -1)) {
        lua_pushnumber(L, dt);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
          printf("Lua Tick Error: %s\n", lua_tostring(L, -1));
          break;
        }
      } else {
        lua_pop(L, 1);
      }

      /*
       * НЕ зовём eid_end(&eid_ctx, 0, 0): он делает syscall SYS_DRAW_BUFFER,
       * который копирует весь backbuffer (1024*768*4 = 3 MB) в kernel-side
       * app_win->buffer через `memcpy` под stac()/clac(). Это полезно для
       * обычных приложений (kernel-compositor рисует их окно), но sysgui
       * сам пишет напрямую в vram через `fast_memcpy_sse` ниже — kernel
       * compositor его не композитит. То есть SYS_DRAW_BUFFER на каждом
       * кадре впустую копировал 3 MB в ядре, что заметно подъедало CPU
       * и душило общий FPS. Убираем.
       */

      /* Читаем _G.needs_redraw из Lua — если Lua хочет ещё кадр
       * (matrix-анимация, переход cursor blink), форсируем следующую итерацию */
      lua_getglobal(L, "needs_redraw");
      if (lua_toboolean(L, -1) && force_frames == 0)
        force_frames = 1;
      lua_pop(L, 1);

      // Накладываем курсор поверх бэкбуфера прямо перед отправкой кадра
      draw_cursor_user(backbuffer, cur_mx, cur_my, screen_w, screen_h);

      // Копируем готовый бэкбуфер во фронтбуфер
      fast_memcpy_sse(vram, backbuffer, screen_w * screen_h * 4);

      last_mx = cur_mx;
      last_my = cur_my;
      last_mdown = cur_mdown;
      last_key = cur_key;
      if (force_frames > 0)
        force_frames--;

      /* Обновляем last_tick только когда рисовали, чтобы dt был корректен */
      last_tick = now;
    }

    /*
     * Frame-rate limiter: спим оставшуюся часть 16ms кванта вместо
     * busy-loop через sys_yield(). Без этого цикл крутится тысячи
     * раз в секунду, выедая CPU и не давая ядру обработать ввод.
     *
     * SYS_SLEEP ждёт до frame_start + TARGET_FRAME_MS; если кадр
     * занял больше — сразу возвращается (без дополнительного штрафа).
     */
    uint32_t frame_end = (uint32_t)_syscall(SYS_GET_TIME, 0, 0, 0, 0, 0);
    uint32_t frame_elapsed = frame_end - frame_start;
    if (frame_elapsed < TARGET_FRAME_MS) {
      sys_sleep(TARGET_FRAME_MS - frame_elapsed);
    } else {
      sys_yield(); /* кадр и так долгий — просто отдаём квант */
    }
    frame_start = (uint32_t)_syscall(SYS_GET_TIME, 0, 0, 0, 0, 0);
  }

  lua_close(L);
  free(backbuffer);
  return 0;
}