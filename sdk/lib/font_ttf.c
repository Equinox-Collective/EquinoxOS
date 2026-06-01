#include <eid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// --- ЖЕСТКАЯ НАСТРОЙКА STB ДЛЯ EQUOS ---
// Эти макросы ДОЛЖНЫ быть перед инклудом хедера

#define STBTT_if(x) if (x)        // ФИКС ОШИБКИ "else without if"
#define STBTT_assert(x) ((void)0) // Чтобы не тянуть лишние зависимости

#define STBTT_malloc(x, u) malloc(x)
#define STBTT_free(x, u) free(x)
#define STBTT_memset memset
#define STBTT_memcpy memcpy
#define STBTT_pow pow
#define STBTT_sqrt sqrt
#define STBTT_fmod fmod
#define STBTT_cos cos
#define STBTT_acos acos
#define STBTT_fabs fabs
#define STBTT_floor floor
#define STBTT_ceil ceil

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

// РЕАЛИЗУЕМ СТРУКТУРУ, КОТОРУЮ ОБЪЯВИЛИ В eid.h
// Убедись, что в eid.h написано: struct eid_font;
struct eid_font {
  stbtt_fontinfo info;
  unsigned char *data;
  float scale;
  int ascent, descent, lineGap;
};

eid_font_t *eid_load_font(unsigned char *ttf_data, float size) {
  if (!ttf_data)
    return NULL;

  eid_font_t *font = malloc(sizeof(eid_font_t));
  if (!font)
    return NULL;

  font->data = ttf_data;

  if (!stbtt_InitFont(&font->info, ttf_data, 0)) {
    free(font);
    return NULL;
  }

  font->scale = stbtt_ScaleForPixelHeight(&font->info, size);
  stbtt_GetFontVMetrics(&font->info, &font->ascent, &font->descent,
                        &font->lineGap);

  return font;
}

void eid_draw_text_ttf(eid_ctx_t *ctx, eid_font_t *font, int x, int y,
                       const char *text, uint32_t color) {
  if (!font || !text || !ctx || !ctx->fb)
    return;

  int curr_x = x;
  float scale = font->scale;
  int baseline = y + (int)(font->ascent * scale);

  for (int i = 0; text[i]; i++) {
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&font->info, text[i], &advance, &lsb);

    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font->info, text[i], scale, scale, &x0, &y0,
                                &x1, &y1);

    int out_w = x1 - x0;
    int out_h = y1 - y0;

    if (out_w > 0 && out_h > 0) {
      unsigned char *bitmap = malloc(out_w * out_h);
      if (!bitmap)
        continue;

      stbtt_MakeCodepointBitmap(&font->info, bitmap, out_w, out_h, out_w, scale,
                                scale, text[i]);

      for (int row = 0; row < out_h; row++) {
        for (int col = 0; col < out_w; col++) {
          unsigned char alpha = bitmap[row * out_w + col];
          if (alpha > 0) {
            int px = curr_x + (int)(lsb * scale) + col;
            int py = baseline + y0 + row;

            if (px >= 0 && px < ctx->win_w && py >= 0 && py < ctx->win_h) {
              uint32_t bg = ctx->fb[py * ctx->win_w + px];

              // Alpha blending (сглаживание)
              uint8_t r = (((color >> 16) & 0xFF) * alpha +
                           ((bg >> 16) & 0xFF) * (255 - alpha)) >>
                          8;
              uint8_t g = (((color >> 8) & 0xFF) * alpha +
                           ((bg >> 8) & 0xFF) * (255 - alpha)) >>
                          8;
              uint8_t b =
                  ((color & 0xFF) * alpha + (bg & 0xFF) * (255 - alpha)) >> 8;

              ctx->fb[py * ctx->win_w + px] = (r << 16) | (g << 8) | b;
            }
          }
        }
      }
      free(bitmap);
    }
    curr_x += (int)(advance * scale);
  }
}

/* Faux-bold: stamp each glyph twice with a 1px horizontal smear to
 * thicken the strokes, and add 1px of extra advance per glyph so the
 * heavier shapes keep a gap and words don't run together. */
void eid_draw_text_ttf_bold(eid_ctx_t *ctx, eid_font_t *font, int x, int y,
                            const char *text, uint32_t color) {
  if (!font || !text || !ctx || !ctx->fb)
    return;

  int curr_x = x;
  float scale = font->scale;
  int baseline = y + (int)(font->ascent * scale);

  int cr = (color >> 16) & 0xFF;
  int cg = (color >> 8) & 0xFF;
  int cb = color & 0xFF;

  for (int i = 0; text[i]; i++) {
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&font->info, text[i], &advance, &lsb);

    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font->info, text[i], scale, scale, &x0, &y0,
                                &x1, &y1);

    int out_w = x1 - x0;
    int out_h = y1 - y0;

    if (out_w > 0 && out_h > 0) {
      unsigned char *bitmap = malloc(out_w * out_h);
      if (!bitmap)
        continue;

      stbtt_MakeCodepointBitmap(&font->info, bitmap, out_w, out_h, out_w, scale,
                                scale, text[i]);

      for (int row = 0; row < out_h; row++) {
        for (int col = 0; col < out_w; col++) {
          unsigned char alpha = bitmap[row * out_w + col];
          if (!alpha)
            continue;
          int py = baseline + y0 + row;
          /* Soft smear: the base glyph is drawn at full coverage (dx=0),
           * the +1px copy at REDUCED coverage (dx=1). A hard full-alpha
           * double-stamp thickens strokes by a solid pixel and fills the
           * inner counters of o/e/a/g at 16px, which reads as a glued
           * blob. Blending the offset copy at ~55% gives a soft ~1.4px
           * stroke that still looks bold but keeps the counters open. */
          for (int dx = 0; dx <= 1; dx++) {
            int px = curr_x + (int)(lsb * scale) + col + dx;
            if (px < 0 || px >= ctx->win_w || py < 0 || py >= ctx->win_h)
              continue;
            unsigned int a = (dx == 0) ? alpha : ((alpha * 140) >> 8);
            if (!a)
              continue;
            uint32_t bg = ctx->fb[py * ctx->win_w + px];
            uint8_t r = (cr * a + ((bg >> 16) & 0xFF) * (255 - a)) >> 8;
            uint8_t g = (cg * a + ((bg >> 8) & 0xFF) * (255 - a)) >> 8;
            uint8_t b = (cb * a + (bg & 0xFF) * (255 - a)) >> 8;
            ctx->fb[py * ctx->win_w + px] = (r << 16) | (g << 8) | b;
          }
        }
      }
      free(bitmap);
    }
    /* +1px tracking compensates for the smear so glyphs keep their gap. */
    curr_x += (int)(advance * scale) + 1;
  }
}