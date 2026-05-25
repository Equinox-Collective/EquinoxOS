#include <eid.h>
#include <equos.h>
#include <string.h>

// Структура шрифта PSF1 (встроена в ядро)
typedef struct {
  uint16_t magic;
  uint8_t mode;
  uint8_t charsize;
} psf1_t;

static psf1_t *sys_font = NULL;

void eid_init() { sys_font = (psf1_t *)_syscall(SYS_GET_FONT, 0, 0, 0, 0, 0); }

// Хэш-функция Murmur-style для генерации ID
uint32_t eid_get_id(const char *label, int x, int y) {
  uint32_t h = 0x811c9dc5;
  while (*label)
    h = (h ^ (uint8_t)*label++) * 0x01000193;
  h ^= (uint32_t)x;
  h ^= (uint32_t)(y << 16);
  return h;
}

void eid_begin(eid_ctx_t *ctx, uint32_t *buffer, int w, int h) {
  ctx->fb = buffer;
  ctx->win_w = w;
  ctx->win_h = h;

  uint64_t mx, my, btns;
  __asm__ volatile(
      "mov $7, %%rax; int $0x80; mov %%rax, %0; mov %%rbx, %1; mov %%rcx, %2"
      : "=r"(mx), "=r"(my), "=r"(btns)::"rax", "rbx", "rcx");

  // ИЗМЕНЕНИЕ: УБИРАЕМ ХАРДКОД -150.
  // Теперь mx/my - это абсолютные координаты экрана.
  // Приложение само вычтет смещение окна (например, mx - WIN_X).
  ctx->mx = (int)mx;
  ctx->my = (int)my;

  bool was_down = ctx->m_down;
  ctx->m_down = (btns & 1);
  ctx->m_clicked = (ctx->m_down && !was_down);
  ctx->last_key = (uint8_t)_syscall(SYS_GET_SCANCODE, 0, 0, 0, 0, 0);
  ctx->hot_id = 0;
}

uint32_t eid_process_interaction(eid_ctx_t *ctx, uint32_t id, int x, int y,
                                 int w, int h) {
  uint32_t state = EID_STATE_NONE;

  // Проверка попадания мыши
  bool inside =
      (ctx->mx >= x && ctx->mx <= x + w && ctx->my >= y && ctx->my <= y + h);

  if (inside) {
    ctx->hot_id = id;
    state |= EID_STATE_HOVER;
  }

  // Логика Active (удержание)
  if (ctx->active_id == id) {
    if (ctx->m_down) {
      state |= EID_STATE_ACTIVE;
    } else {
      // Если отпустили над виджетом — это клик
      if (inside)
        state |= EID_STATE_CLICKED;
      ctx->active_id = 0;
    }
  } else {
    if (ctx->hot_id == id && ctx->m_clicked) {
      ctx->active_id = id;
      ctx->focus_id = id; // Даем фокус при клике
    }
  }

  if (ctx->focus_id == id)
    state |= EID_STATE_FOCUSED;

  return state;
}

// --- ПРИМИТИВЫ (БЕЗ ЦВЕТОВЫХ ОГРАНИЧЕНИЙ) ---

void eid_draw_pixel(uint32_t *fb, int win_w, int win_h, int x, int y,
                    uint32_t color) {
  if (x < 0 || y < 0 || x >= win_w || y >= win_h)
    return;
  fb[y * win_w + x] = color;
}

void eid_draw_rect(uint32_t *fb, int win_w, int win_h, int x, int y, int w,
                   int h, uint32_t color) {
  // Клиппинг границ один раз на весь прямоугольник
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > win_w)
    w = win_w - x;
  if (y + h > win_h)
    h = win_h - y;
  if (w <= 0 || h <= 0)
    return;

  // Прямой построчный рендеринг без перевычисления адресов
  for (int i = 0; i < h; i++) {
    uint32_t *line = &fb[(y + i) * win_w + x];
    for (int j = 0; j < w; j++) {
      line[j] = color;
    }
  }
}

void eid_draw_text(uint32_t *fb, int win_w, int win_h, int x, int y,
                   const char *text, uint32_t color) {
  if (!sys_font)
    return;

  while (*text) {
    uint8_t *glyph = (uint8_t *)sys_font + sizeof(psf1_t) +
                     ((uint8_t)*text * sys_font->charsize);

    for (int cy = 0; cy < sys_font->charsize; cy++) {
      int py = y + cy;
      if (py < 0 || py >= win_h) {
        glyph++;
        continue;
      }

      uint32_t *line = &fb[py * win_w];
      for (int cx = 0; cx < 8; cx++) {
        int px = x + cx;
        if (px >= 0 && px < win_w) {
          if ((*glyph >> (7 - cx)) & 1) {
            line[px] = color;
          }
        }
      }
      glyph++;
    }
    x += 8;
    text++;
  }
}

void eid_draw_line(uint32_t *fb, int win_w, int win_h, int x1, int y1, int x2,
                   int y2, uint32_t color) {
  int dx = (x2 - x1 < 0) ? -(x2 - x1) : (x2 - x1);
  int dy = (y2 - y1 < 0) ? -(y2 - y1) : (y2 - y1);
  int sx = (x1 < x2) ? 1 : -1;
  int sy = (y1 < y2) ? 1 : -1;
  int err = dx - dy;

  while (1) {
    eid_draw_pixel(fb, win_w, win_h, x1, y1, color);
    if (x1 == x2 && y1 == y2)
      break;
    int e2 = 2 * err;
    if (e2 > -dy) {
      err -= dy;
      x1 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y1 += sy;
    }
  }
}

void eid_end(eid_ctx_t *ctx, int win_x, int win_y) {
  // Просто отправляем буфер в ядро
  _syscall(SYS_DRAW_BUFFER, win_x, win_y, ctx->win_w, ctx->win_h,
           (uint64_t)ctx->fb);
}

void eid_draw_gradient_rect(uint32_t *fb, int win_w, int win_h, int x, int y,
                            int w, int h, uint32_t col1, uint32_t col2,
                            bool vertical) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > win_w)
    w = win_w - x;
  if (y + h > win_h)
    h = win_h - y;
  if (w <= 0 || h <= 0)
    return;

  if (vertical) {
    int r1 = (col1 >> 16) & 0xFF;
    int g1 = (col1 >> 8) & 0xFF;
    int b1 = col1 & 0xFF;

    int r2 = (col2 >> 16) & 0xFF;
    int g2 = (col2 >> 8) & 0xFF;
    int b2 = col2 & 0xFF;

    // Fixed-point 16.16
    int r = r1 << 16;
    int g = g1 << 16;
    int b = b1 << 16;

    int dr = ((r2 - r1) << 16) / h;
    int dg = ((g2 - g1) << 16) / h;
    int db = ((b2 - b1) << 16) / h;

    for (int i = 0; i < h; i++) {
      uint32_t color = (((r >> 16) & 0xFF) << 16) | (((g >> 16) & 0xFF) << 8) |
                       ((b >> 16) & 0xFF);

      uint32_t *line = &fb[(y + i) * win_w + x];
      for (int j = 0; j < w; j++) {
        line[j] = color;
      }
      r += dr;
      g += dg;
      b += db;
    }
  }
}