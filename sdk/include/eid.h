#ifndef EID_H
#define EID_H

#include <stdbool.h>
#include <stdint.h>

// Состояния взаимодействия (битовая маска)
#define EID_STATE_NONE 0
#define EID_STATE_HOVER (1 << 0)   // Мышь над виджетом
#define EID_STATE_ACTIVE (1 << 1)  // Виджет зажат (Hold)
#define EID_STATE_CLICKED (1 << 2) // Произошел клик (отпускание над виджетом)
#define EID_STATE_FOCUSED (1 << 3) // Виджет имеет фокус ввода

// Контекст EID (Мозг системы)
typedef struct {
  uint32_t *fb;     // Целевой буфер
  int win_w, win_h; // Размеры холста
  int mx, my;       // Координаты мыши
  bool m_down;      // Зажата ли ЛКМ
  bool m_clicked;   // Был ли клик в этом кадре

  uint32_t hot_id;    // ID виджета под мышкой
  uint32_t active_id; // ID виджета, который мы удерживаем
  uint32_t focus_id;  // ID виджета с фокусом клавиатуры

  uint16_t last_key; // Последний сканкод. Для extended-клавиш (0xE0-префикс)
                     // выставляется верхний бит: 0x100 | code. Например:
                     //   Up=0x148, Down=0x150, Left=0x14B, Right=0x14D,
                     //   PgUp=0x149, PgDn=0x151, Home=0x147, End=0x14F,
                     //   Insert=0x152, Delete=0x153.
} eid_ctx_t;

// Инициализация
void eid_init();
void eid_begin(eid_ctx_t *ctx, uint32_t *buffer, int w, int h);
void eid_end(eid_ctx_t *ctx, int win_x, int win_y);

// Генерация уникального ID для виджета (на основе имени и позиции)
uint32_t eid_get_id(const char *label, int x, int y);

// ЛОГИЧЕСКИЕ ВИДЖЕТЫ (Не рисуют, а возвращают состояние)
uint32_t eid_process_interaction(eid_ctx_t *ctx, uint32_t id, int x, int y,
                                 int w, int h);

// ПРИМИТИВЫ ОТРИСОВКИ (Кодер использует их сам)
void eid_draw_pixel(uint32_t *fb, int win_w, int win_h, int x, int y,
                    uint32_t color);
void eid_draw_rect(uint32_t *fb, int win_w, int win_h, int x, int y, int w,
                   int h, uint32_t color);
void eid_draw_text(uint32_t *fb, int win_w, int win_h, int x, int y,
                   const char *text, uint32_t color);
void eid_draw_line(uint32_t *fb, int win_w, int win_h, int x1, int y1, int x2,
                   int y2, uint32_t color);
void eid_draw_gradient_rect(uint32_t *fb, int win_w, int win_h, int x, int y,
                            int w, int h, uint32_t col1, uint32_t col2,
                            bool vertical);

typedef struct eid_font eid_font_t; // Непрозрачная структура
eid_font_t *eid_load_font(unsigned char *ttf_data, float size);
void eid_draw_text_ttf(eid_ctx_t *ctx, eid_font_t *font, int x, int y,
                       const char *text, uint32_t color);
#endif