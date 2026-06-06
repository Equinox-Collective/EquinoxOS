#include <eid.h>
#include <equos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define WIN_W 400
#define WIN_H 220
#define WIN_X 180
#define WIN_Y 180

#define CLR_BG 0x121212
#define CLR_PANEL 0x1E1E1E
#define CLR_ACCENT 0xBB86FC // Фиолетовый
#define CLR_TEXT 0xE0E0E0
#define CLR_DIM 0x757575

static eid_ctx_t gui;
static uint32_t fb[WIN_W * WIN_H];

// Храним данные WAV
#pragma pack(push, 1)
typedef struct {
  char id[4];
  uint32_t size;
  char wave[4];
} WavHeader;
typedef struct {
  char id[4];
  uint32_t size;
} ChunkHeader;
typedef struct {
  uint16_t fmt;
  uint16_t ch;
  uint32_t rate;
  uint32_t brate;
  uint16_t align;
  uint16_t bps;
} FmtChunk;
#pragma pack(pop)

void draw_niplay_ui(const char *name, uint32_t processed, uint32_t total) {
  eid_begin(&gui, fb, WIN_W, WIN_H);
  gui.mx -= WIN_X;
  gui.my -= WIN_Y;

  // 1. Фон окна
  eid_draw_rect(fb, WIN_W, WIN_H, 0, 0, WIN_W, WIN_H, CLR_BG);

  // 2. Заголовок (Header)
  eid_draw_rect(fb, WIN_W, WIN_H, 0, 0, WIN_W, 30, CLR_PANEL);
  eid_draw_line(fb, WIN_W, WIN_H, 0, 30, WIN_W, 30, CLR_ACCENT);
  eid_draw_text(fb, WIN_W, WIN_H, 12, 8, "NiPlay Media", CLR_ACCENT);

  // Кнопка закрытия
  uint32_t close_id = eid_get_id("X", WIN_W - 25, 5);
  uint32_t close_st =
      eid_process_interaction(&gui, close_id, WIN_W - 25, 5, 20, 20);
  eid_draw_text(fb, WIN_W, WIN_H, WIN_W - 20, 8, "X",
                (close_st & EID_STATE_HOVER) ? 0xFF0000 : CLR_TEXT);
  if (close_st & EID_STATE_CLICKED)
    exit(0);

  // 3. Информация о треке
  eid_draw_text(fb, WIN_W, WIN_H, 20, 50, "NOW PLAYING:", CLR_DIM);
  eid_draw_text(fb, WIN_W, WIN_H, 20, 70, name, CLR_TEXT);

  // 4. Прогресс-бар
  int bar_x = 20;
  int bar_y = 110;
  int bar_w = WIN_W - 40;
  int bar_h = 6;

  eid_draw_rect(fb, WIN_W, WIN_H, bar_x, bar_y, bar_w, bar_h,
                0x333333); // Фон бара
  if (total > 0) {
    int progress = (int)((uint64_t)processed * bar_w / total);
    eid_draw_rect(fb, WIN_W, WIN_H, bar_x, bar_y, progress, bar_h, CLR_ACCENT);
    // "Свечение" на конце бара
    eid_draw_rect(fb, WIN_W, WIN_H, bar_x + progress - 2, bar_y - 2, 4, 10,
                  0xFFFFFF);
  }

  // 5. Визуализатор (Спектрограмма)
  for (int i = 0; i < 20; i++) {
    int v_h = rand() % 40 + 5; // Пока рандом, позже прикрутим реальный FFT
    eid_draw_rect(fb, WIN_W, WIN_H, 20 + i * 18, 180 - v_h, 12, v_h,
                  CLR_ACCENT);
    eid_draw_rect(fb, WIN_W, WIN_H, 20 + i * 18, 180 - v_h - 4, 12, 2,
                  0x555555); // "Пики"
  }

  // 6. Таймер
  char time_str[16];
  uint32_t sec = processed / 176400; // Примерно для 44100/16/stereo
  sprintf(time_str, "%02d:%02d", sec / 60, sec % 60);
  eid_draw_text(fb, WIN_W, WIN_H, WIN_W - 70, 125, time_str, CLR_DIM);

  eid_end(&gui, WIN_X, WIN_Y);
}

int main(int argc, char **argv) {
  eid_init();
  char *filename = "MUSIC.WAV";
  if (argc > 1)
    filename = argv[1];

  uint32_t file_size = 0;
  uint8_t *file_data = (uint8_t *)_syscall(SYS_READ_FILE, (uintptr_t)filename,
                                           (uintptr_t)&file_size, 0, 0, 0);

  if (!file_data)
    return 1;

  // Парсинг WAV
  FmtChunk fmt;
  uint8_t *audio_ptr = NULL;
  uint32_t audio_len = 0;
  uint32_t offset = 12;

  while (offset < file_size - 8) {
    ChunkHeader *ch = (ChunkHeader *)(file_data + offset);
    if (strncmp(ch->id, "fmt ", 4) == 0)
      memcpy(&fmt, file_data + offset + 8, 16);
    else if (strncmp(ch->id, "data", 4) == 0) {
      audio_len = ch->size;
      audio_ptr = file_data + offset + 8;
      break;
    }
    offset += 8 + ch->size;
  }

  if (!audio_ptr)
    return 1;

  _syscall(SYS_AUDIO_SET_RATE, fmt.rate, 0, 0, 0, 0);

  uint32_t processed = 0;
  uint32_t chunk_size = 8192;
  int ui_counter = 0;

  while (processed < audio_len) {
    uint32_t to_play = (audio_len - processed > chunk_size)
                           ? chunk_size
                           : (audio_len - processed);

    _syscall(SYS_AUDIO_PLAY, (uintptr_t)(audio_ptr + processed),
             (uint64_t)to_play, 0, 0, 0);
    processed += to_play;

    // Обновляем UI каждые 8 чанков (~15 FPS), чтобы не тормозить звук
    if (ui_counter++ % 8 == 0) {
      draw_niplay_ui(filename, processed, audio_len);
    }

    if ((uint8_t)_syscall(SYS_GET_SCANCODE, 0, 0, 0, 0, 0) == 0x01)
      break;
    sys_yield();
  }

  exit(0);
  return 0;
}