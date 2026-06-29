#include "../../../syslibc/string.h"
#include "../../mem/memory.h"
#include "vesa.h"
#include "font8x8.h"
#include "../../fs/vfs.h"
#include "../../mem/vmm.h"
#include <stdint.h>

#define TILE_SIZE 32

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ЭКРАНА ---
uintptr_t fb_base_addr;
uint32_t screen_width;
uint32_t screen_height;
uint32_t screen_pitch;
uint32_t *backbuffer;
static uint32_t *cached_bg = NULL;
psf1_t *current_font = NULL;
dirty_rect_t screen_dirty = {0, 0, 0, 0, false};
extern uint32_t tile_cols;
extern uint32_t tile_rows;
extern uint8_t *tile_grid;  // Карта грязных тайлов (1 - изменен, 0 - чист)
extern bool grid_modified;  // Были ли изменения вообще

void *vesa_get_font() { return current_font; }

void init_vesa(uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch) {
  fb_base_addr = (uintptr_t)addr;
  screen_width = width;
  screen_height = height;
  screen_pitch = pitch;

  backbuffer = (uint32_t *)vmm_alloc_large_buffer(width * height * 4);
  if (!backbuffer) {
    backbuffer = (uint32_t *)kmalloc(width * height * 4);
  }
  
  memset(backbuffer, 0, width * height * 4);
  vesa_mark_dirty(0, 0, screen_width, screen_height);
  screen_dirty.x1 = 0;
  screen_dirty.y1 = 0;
  screen_dirty.x2 = width;
  screen_dirty.y2 = height;
  screen_dirty.modified = true;
}

void put_pixel(int x, int y, uint32_t color) {
  if (x < 0 || x >= (int)screen_width || y < 0 || y >= (int)screen_height)
    return;
  backbuffer[y * screen_width + x] = color;
}

void draw_background() {
  if (!cached_bg) {
    cached_bg = (uint32_t *)vmm_alloc_large_buffer(screen_width * screen_height * 4);
    if (!cached_bg) {
      cached_bg = (uint32_t *)kmalloc(screen_width * screen_height * 4);
    }
    
    for (int y = 0; y < (int)screen_height; y++) {
      for (int x = 0; x < (int)screen_width; x++) {
        uint8_t r = 20;
        uint8_t g = 30;
        uint8_t b = 50 + (y * 50 / screen_height);
        cached_bg[y * screen_width + x] = (r << 16) | (g << 8) | b;
      }
    }
  }
  memcpy(backbuffer, cached_bg, screen_width * screen_height * 4);
}

void draw_rect(int x, int y, int w, int h, uint32_t color) {
    vesa_mark_dirty(x, y, w, h);
    
    int start_x = x;
    int start_y = y;
    int end_x = x + w;
    int end_y = y + h;

    if (start_x < 0) start_x = 0;
    if (start_y < 0) start_y = 0;
    if (end_x > (int)screen_width) end_x = screen_width;
    if (end_y > (int)screen_height) end_y = screen_height;

    int draw_w = end_x - start_x;
    if (draw_w <= 0 || start_y >= end_y) return;

    for (int i = start_y; i < end_y; i++) {
        vesa_fill_color_fast(&backbuffer[i * screen_width + start_x], draw_w, color);
    }
}

// Быстрое деление на 255 без использования тяжелой инструкции DIV
static inline uint32_t div255(uint32_t val) {
    return ((val + 1 + (val >> 8)) >> 8);
}

static uint32_t blend(uint32_t color_bg, uint32_t color_fg, uint8_t alpha) {
  uint32_t rb = (((color_fg & 0xFF00FF) * alpha) +
                 ((color_bg & 0xFF00FF) * (255 - alpha)));
  uint32_t g = (((color_fg & 0x00FF00) * alpha) +
                ((color_bg & 0x00FF00) * (255 - alpha)));
  
  rb = (div255(rb & 0xFF00FF00) & 0xFF00FF) | (div255(rb & 0x00FF00FF) & 0xFF00FF);
  g = div255(g) & 0x00FF00;
  
  return rb | g;
}

void draw_transparent_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
  for (int i = y; i < y + h; i++) {
    for (int j = x; j < x + w; j++) {
      if (j >= 0 && j < (int)screen_width && i >= 0 && i < (int)screen_height) {
        uint32_t bg_color = backbuffer[i * screen_width + j];
        put_pixel(j, i, blend(bg_color, color, alpha));
      }
    }
  }
}

void vesa_draw_char(char c, int x, int y, uint32_t fg) {
  if (!current_font) return;

  uint8_t *glyph = (uint8_t *)current_font + sizeof(psf1_t) +
                   ((uint8_t)c * current_font->charsize);

  for (int cy = 0; cy < current_font->charsize; cy++) {
    for (int cx = 0; cx < 8; cx++) {
      if ((*glyph >> (7 - cx)) & 1) {
        put_pixel(x + cx, y + cy, fg);
      }
    }
    glyph++;
  }
}

void vesa_draw_string(const char *s, int x, int y, uint32_t fg) {
  while (*s) {
    vesa_draw_char(*s, x, y, fg);
    x += 8;
    s++;
  }
}

void hex_to_string(uint64_t val, char *buf) {
  const char *hex_chars = "0123456789ABCDEF";
  buf[16] = '\0';
  for (int i = 15; i >= 0; i--) {
    buf[i] = hex_chars[val & 0xF];
    val >>= 4;
  }
}

void vesa_draw_string_hex(const char *prefix, int x, int y, uint64_t val, uint32_t fg) {
  vesa_draw_string(prefix, x, y, fg);
  char buf[17];
  hex_to_string(val, buf);
  vesa_draw_string(buf, x + 8 * strlen(prefix), y, fg);
}

void vesa_draw_buffer(int x, int y, int w, int h, uint32_t *buffer) {
    int src_x = 0;
    int dst_x = x;
    int draw_w = w;

    if (dst_x < 0) {
        src_x = -dst_x;
        draw_w -= src_x;
        dst_x = 0;
    }
    if (dst_x + draw_w > (int)screen_width) {
        draw_w = screen_width - dst_x;
    }

    if (draw_w <= 0) return;

    for (int row = 0; row < h; row++) {
        int draw_y = y + row;
        if (draw_y < 0 || draw_y >= (int)screen_height) continue;

        uint32_t *dst = &backbuffer[draw_y * screen_width + dst_x];
        uint32_t *src = &buffer[row * w + src_x];
        memcpy(dst, src, draw_w * 4);
    }
}

void vesa_update() {
    if (!screen_dirty.modified) return;

    int x1 = (screen_dirty.x1 < 0) ? 0 : screen_dirty.x1;
    int y1 = (screen_dirty.y1 < 0) ? 0 : screen_dirty.y1;
    int x2 = (screen_dirty.x2 > (int)screen_width) ? (int)screen_width : screen_dirty.x2;
    int y2 = (screen_dirty.y2 > (int)screen_height) ? (int)screen_height : screen_dirty.y2;

    int copy_w = x2 - x1;
    if (copy_w <= 0 || y1 >= y2) {
        screen_dirty.modified = false;
        return;
    }

    for (int i = y1; i < y2; i++) {
        uint8_t* fb_line = (uint8_t*)fb_base_addr + (i * screen_pitch);
        uint32_t* dst = (uint32_t*)(fb_line + (x1 * 4));
        uint32_t* src = &backbuffer[i * screen_width + x1];
        vesa_copy_buffer_fast(dst, src, (uint32_t)copy_w);
    }
    
    screen_dirty.modified = false;
}

void vesa_draw_psf_char(psf_t *font, char c, int x, int y, uint32_t fg) {
  uint8_t *glyph = (uint8_t *)font + font->headersize + (uint8_t)c * font->bytesperglyph;
  for (uint32_t cy = 0; cy < font->height; cy++) {
    for (uint32_t cx = 0; cx < font->width; cx++) {
      if ((glyph[cy] >> (font->width - 1 - cx)) & 1) {
        put_pixel(x + cx, y + cy, fg);
      }
    }
  }
}

static uint64_t current_font_size = 0;

void vesa_set_font(void *font_addr) { current_font = (psf1_t *)font_addr; }
void vesa_set_font_size(uint64_t size) { current_font_size = size; }
uint64_t vesa_get_font_size(void) { return current_font_size; }

uint32_t fb_vfs_write(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
  (void)node;
  memcpy((uint8_t *)backbuffer + offset, buffer, size);
  return size;
}

void fb_install_vfs() {
  vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
  memset(node, 0, sizeof(vfs_node_t));

  strcpy(node->name, "fb0");
  node->write = fb_vfs_write;
  node->flags = 2;

  vfs_register_device(node);
}

void put_pixel_alpha(int x, int y, uint32_t argb) {
  if (x < 0 || x >= (int)screen_width || y < 0 || y >= (int)screen_height)
    return;

  uint8_t a = (argb >> 24) & 0xFF;
  if (a == 0) return;
  if (a == 255) {
    backbuffer[y * screen_width + x] = argb & 0xFFFFFF;
    return;
  }

  uint32_t bg = backbuffer[y * screen_width + x];

  uint32_t r_bg = (bg >> 16) & 0xFF;
  uint32_t g_bg = (bg >> 8) & 0xFF;
  uint32_t b_bg = bg & 0xFF;

  uint32_t r_fg = (argb >> 16) & 0xFF;
  uint32_t g_fg = (argb >> 8) & 0xFF;
  uint32_t b_fg = argb & 0xFF;

  // Высокооптимизированное умножение и сдвиг вместо медленного деления
  uint32_t r_out = div255(r_fg * a + r_bg * (255 - a));
  uint32_t g_out = div255(g_fg * a + g_bg * (255 - a));
  uint32_t b_out = div255(b_fg * a + b_bg * (255 - a));

  backbuffer[y * screen_width + x] = (r_out << 16) | (g_out << 8) | b_out;
}

void vesa_fill_color_fast(uint32_t* dest, uint32_t count, uint32_t color) {
    uint64_t val = ((uint64_t)color << 32) | color;
    uint64_t qcount = count / 2;
    uint32_t rem = count % 2;

    __asm__ volatile (
        "rep stosq\n"
        : "+D"(dest), "+c"(qcount)
        : "a"(val)
        : "memory"
    );

    if (rem) {
        *dest = color;
    }
}

void vesa_copy_buffer_fast(uint32_t* dest, uint32_t* src, uint32_t count) {
    uint64_t qcount = count / 2;
    uint32_t rem = count % 2;

    __asm__ volatile (
        "rep movsq\n"
        : "+D"(dest), "+S"(src), "+c"(qcount)
        : 
        : "memory"
    );

    if (rem) {
        dest[0] = src[0];
    }
}

void vesa_mark_dirty(int x, int y, int w, int h) {
    if (x + w < 0 || y + h < 0 || x >= (int)screen_width || y >= (int)screen_height) return;

    int nx1 = (x < 0) ? 0 : x;
    int ny1 = (y < 0) ? 0 : y;
    int nx2 = (x + w > (int)screen_width) ? (int)screen_width : x + w;
    int ny2 = (y + h > (int)screen_height) ? (int)screen_height : y + h;

    if (!screen_dirty.modified) {
        screen_dirty.x1 = nx1; screen_dirty.y1 = ny1;
        screen_dirty.x2 = nx2; screen_dirty.y2 = ny2;
        screen_dirty.modified = true;
    } else {
        if (nx1 < screen_dirty.x1) screen_dirty.x1 = nx1;
        if (ny1 < screen_dirty.y1) screen_dirty.y1 = ny1;
        if (nx2 > screen_dirty.x2) screen_dirty.x2 = nx2;
        if (ny2 > screen_dirty.y2) screen_dirty.y2 = ny2;
    }
}

void vesa_clear_dirty() {
    screen_dirty.modified = false;
}

void put_pixel_direct(int x, int y, uint32_t color) {
  if (x < 0 || x >= (int)screen_width || y < 0 || y >= (int)screen_height)
    return;
  uint32_t *pixel_ptr = (uint32_t *)(fb_base_addr + (y * screen_pitch) + (x * 4));
  *pixel_ptr = color;
}

void draw_rect_direct(int x, int y, int w, int h, uint32_t color) {
  for (int i = y; i < y + h; i++) {
    if (i < 0 || i >= (int)screen_height)
      continue;
    uint32_t *dest = (uint32_t *)(fb_base_addr + (i * screen_pitch));
    for (int j = x; j < x + w; j++) {
      if (j >= 0 && j < (int)screen_width)
        dest[j] = color;
    }
  }
}

void vesa_draw_char_direct(char c, int x, int y, uint32_t fg) {
  if (c < 0 || c > 127)
    return;
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 8; j++) {
      if (font8x8_basic[(int)c][i] & (1 << j)) {
        put_pixel_direct(x + j, y + i, fg);
      }
    }
  }
}

void vesa_draw_string_direct(const char *s, int x, int y, uint32_t fg) {
  while (*s) {
    vesa_draw_char_direct(*s, x, y, fg);
    x += 8;
    s++;
  }
}

void vesa_draw_string_hex_direct(const char *prefix, int x, int y, uint64_t val, uint32_t fg) {
  vesa_draw_string_direct(prefix, x, y, fg);
  char buf[17];
  hex_to_string(val, buf);
  vesa_draw_string_direct(buf, x + strlen(prefix) * 8, y, fg);
}