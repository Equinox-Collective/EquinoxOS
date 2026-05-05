#include <eid.h>
#include <equos.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
  uint16_t type;
  uint32_t size;
  uint16_t reserved1;
  uint16_t reserved2;
  uint32_t offset;
} bmp_file_header_t;

typedef struct {
  uint32_t size;
  int32_t width;
  int32_t height;
  uint16_t planes;
  uint16_t bit_count;
  uint32_t compression;
  uint32_t size_image;
  int32_t x_ppm;
  int32_t y_ppm;
  uint32_t colors_used;
  uint32_t colors_important;
} bmp_info_header_t;
#pragma pack(pop)

#define WIN_W 520
#define WIN_H 420
#define VIEW_X 12
#define VIEW_Y 58
#define VIEW_W (WIN_W - 24)
#define VIEW_H (WIN_H - 92)

#define CLR_BG 0xF4F6F8
#define CLR_TOP 0x26313F
#define CLR_TOP_2 0x334255
#define CLR_TEXT 0x17202A
#define CLR_MUTED 0x687487
#define CLR_DANGER 0xA32929
#define CLR_PANEL 0xFFFFFF
#define CLR_BORDER 0xB8C2CF

static uint32_t fb[WIN_W * WIN_H];

static void print(const char *s) {
  _syscall(SYS_PRINT, (uint64_t)s, 0, 0, 0, 0);
}

static int abs_i32(int value) {
  return value < 0 ? -value : value;
}

static void copy_filename(char *dst, int dst_size, const char *src) {
  int i = 0;
  if (!dst || dst_size <= 0)
    return;
  if (!src) {
    dst[0] = '\0';
    return;
  }

  while (src[i] != '\0' && src[i] != '\r' && src[i] != '\n' &&
         src[i] != ' ' && i < dst_size - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void clear(uint32_t color) {
  for (int i = 0; i < WIN_W * WIN_H; i++)
    fb[i] = color;
}

static void draw_frame(const char *title) {
  eid_draw_rect(fb, WIN_W, WIN_H, 0, 0, WIN_W, WIN_H, CLR_BG);
  eid_draw_rect(fb, WIN_W, WIN_H, 0, 0, WIN_W, 38, CLR_TOP);
  eid_draw_rect(fb, WIN_W, WIN_H, 0, 38, WIN_W, 2, CLR_TOP_2);
  eid_draw_text(fb, WIN_W, WIN_H, 12, 12, "BMP Viewer", 0xFFFFFF);
  eid_draw_text(fb, WIN_W, WIN_H, 118, 12, title, 0xD8E4F0);

  eid_draw_rect(fb, WIN_W, WIN_H, VIEW_X - 1, VIEW_Y - 1, VIEW_W + 2, VIEW_H + 2,
                CLR_BORDER);
  eid_draw_rect(fb, WIN_W, WIN_H, VIEW_X, VIEW_Y, VIEW_W, VIEW_H, CLR_PANEL);
  eid_draw_text(fb, WIN_W, WIN_H, 12, WIN_H - 22, "ESC - close", CLR_MUTED);
}

static bool validate_bmp(const uint8_t *file_data, uint32_t file_size,
                         bmp_file_header_t **out_fh,
                         bmp_info_header_t **out_ih,
                         uint32_t *out_row_size) {
  if (!file_data || file_size < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t))
    return false;

  bmp_file_header_t *fh = (bmp_file_header_t *)file_data;
  bmp_info_header_t *ih = (bmp_info_header_t *)(file_data + sizeof(bmp_file_header_t));

  if (fh->type != 0x4D42 || ih->planes != 1 || ih->compression != 0)
    return false;
  if (ih->bit_count != 24 && ih->bit_count != 32)
    return false;
  if (ih->width <= 0 || ih->height == 0)
    return false;
  if (fh->offset >= file_size)
    return false;

  uint32_t height = (uint32_t)abs_i32(ih->height);
  uint64_t row_size = ((((uint64_t)ih->width * ih->bit_count) + 31) / 32) * 4;
  uint64_t needed_size = (uint64_t)fh->offset + row_size * height;

  if (row_size == 0 || row_size > 0xFFFFFFFFu || needed_size > file_size)
    return false;

  *out_fh = fh;
  *out_ih = ih;
  *out_row_size = (uint32_t)row_size;
  return true;
}

static void draw_bmp(uint8_t *file_data, uint32_t file_size, const char *filename) {
  bmp_file_header_t *fh = 0;
  bmp_info_header_t *ih = 0;
  uint32_t row_size = 0;

  draw_frame(filename);

  if (!validate_bmp(file_data, file_size, &fh, &ih, &row_size)) {
    eid_draw_text(fb, WIN_W, WIN_H, 24, 86, "Unsupported or broken BMP file", CLR_DANGER);
    eid_draw_text(fb, WIN_W, WIN_H, 24, 108, "Supported: uncompressed 24/32-bit BMP", CLR_MUTED);
    return;
  }

  int bmp_w = ih->width;
  int bmp_h = abs_i32(ih->height);
  int bpp = ih->bit_count / 8;
  bool top_down = ih->height < 0;

  int scale = 1;
  while (((bmp_w + scale - 1) / scale) > VIEW_W ||
         ((bmp_h + scale - 1) / scale) > VIEW_H) {
    scale++;
  }

  int draw_w = (bmp_w + scale - 1) / scale;
  int draw_h = (bmp_h + scale - 1) / scale;
  int dst_x = VIEW_X + (VIEW_W - draw_w) / 2;
  int dst_y = VIEW_Y + (VIEW_H - draw_h) / 2;
  uint8_t *pixels = file_data + fh->offset;

  for (int y = 0; y < draw_h; y++) {
    int src_y = y * scale;
    if (src_y >= bmp_h)
      src_y = bmp_h - 1;
    if (!top_down)
      src_y = bmp_h - 1 - src_y;

    for (int x = 0; x < draw_w; x++) {
      int src_x = x * scale;
      if (src_x >= bmp_w)
        src_x = bmp_w - 1;

      uint8_t *p = pixels + (src_y * row_size) + (src_x * bpp);
      uint32_t color = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
      fb[(dst_y + y) * WIN_W + (dst_x + x)] = color;
    }
  }
}

int main(int argc, char **argv) {
  eid_init();

  char filename[32];
  copy_filename(filename, sizeof(filename), "IMAGE.BMP");
  if (argc > 1 && argv[1])
    copy_filename(filename, sizeof(filename), argv[1]);

  print("[BMPVIEW] Loading ");
  print(filename);
  print("\n");

  uint32_t file_size = 0;
  uint8_t *file_data = (uint8_t *)sys_read_file(filename, &file_size);

  if (!file_data && argc <= 1) {
    copy_filename(filename, sizeof(filename), "LOGO.BMP");
    print("[BMPVIEW] Trying fallback LOGO.BMP\n");
    file_data = (uint8_t *)sys_read_file(filename, &file_size);
  }

  clear(CLR_BG);
  if (file_data) {
    draw_bmp(file_data, file_size, filename);
  } else {
    draw_frame(filename);
    eid_draw_text(fb, WIN_W, WIN_H, 24, 86, "File not found", CLR_DANGER);
    eid_draw_text(fb, WIN_W, WIN_H, 24, 108, "Save IMAGE.BMP from Paint or pass a BMP name.", CLR_MUTED);
  }

  sys_draw_buffer(140, 120, WIN_W, WIN_H, fb);

  while (1) {
    if (sys_get_scancode() == 0x01)
      break;
    sleep(10);
  }

  sys_exit(0);
  return 0;
}
