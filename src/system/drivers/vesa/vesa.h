#ifndef VESA_H
#define VESA_H

#include <stdint.h>
#include <stdbool.h>

// Теперь эти значения будут устанавливаться динамически из Limine
// В vesa.h
extern uint32_t* backbuffer;
extern uintptr_t fb_base_addr;
extern uint32_t screen_width;
extern uint32_t screen_height;
extern uint32_t screen_pitch;


// Цвета в формате 0xRRGGBB (32 бита достаточно)
#define COLOR_BLACK       0x000000
#define COLOR_WHITE       0xFFFFFF
#define COLOR_RED         0xFF0000
#define COLOR_GREEN       0x00FF00
#define COLOR_BLUE        0x0000FF
#define COLOR_AERO_BLUE   0x0078D7
#define COLOR_GREY        0xCCCCCC
#define COLOR_DARK_GREY   0x333333

typedef struct {
    uint32_t magic;         // 0x864ab536
    uint32_t version;
    uint32_t headersize;    // Смещение до данных шрифта
    uint32_t flags;
    uint32_t numglyph;      // Количество символов
    uint32_t bytesperglyph; // Сколько байт на один символ (например, 16)
    uint32_t height;        // Высота (16)
    uint32_t width;         // Ширина (8)
} psf_t;

typedef struct {
    uint8_t magic[2];     // 0x36, 0x04
    uint8_t mode;         // Режим (0..3)
    uint8_t charsize;     // Высота символа (у тебя будет 16)
} psf1_t;

typedef struct {
    int x1, y1, x2, y2;
    bool modified;
} dirty_rect_t;

extern dirty_rect_t screen_dirty;

// Прототипы функций с правильными аргументами
void init_vesa(uint64_t fb_addr, uint32_t width, uint32_t height, uint32_t pitch);
void put_pixel(int x, int y, uint32_t color);
void draw_rect(int x, int y, int w, int h, uint32_t color);
void draw_background();
void vesa_draw_string(const char* s, int x, int y, uint32_t fg);
void vesa_draw_char(char c, int x, int y, uint32_t fg);
void draw_transparent_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
void vesa_draw_string_hex(const char* prefix, int x, int y, uint64_t val, uint32_t fg);
void hex_to_string(uint64_t val, char* buf);
void vesa_update(); 
void vesa_draw_buffer(int x, int y, int w, int h, uint32_t* buffer);
void fb_install_vfs(void);
void put_pixel_alpha(int x, int y, uint32_t argb);
void vesa_draw_psf_char(psf_t* font, char c, int x, int y, uint32_t fg);
void vesa_set_font(void* font_addr);
void vesa_set_font_size(uint64_t size);
uint64_t vesa_get_font_size(void);
void put_pixel_direct(int x, int y, uint32_t color);
void draw_rect_direct(int x, int y, int w, int h, uint32_t color);
void vesa_draw_char_direct(char c, int x, int y, uint32_t fg);
void vesa_draw_string_direct(const char* s, int x, int y, uint32_t fg);
void vesa_draw_string_hex_direct(const char* prefix, int x, int y, uint64_t val, uint32_t fg);
// Функция для пометки области как "нуждающейся в перерисовке"
void vesa_mark_dirty(int x, int y, int w, int h);
void vesa_clear_dirty();

// Быстрые версии (SSE)
void vesa_fill_color_fast(uint32_t* dest, uint32_t count, uint32_t color);
void vesa_copy_buffer_fast(uint32_t* dest, uint32_t* src, uint32_t count);

// Новая система слоев (опционально для 19.4, но заложим базу)
void vesa_draw_rect_fast(int x, int y, int w, int h, uint32_t color);

#endif