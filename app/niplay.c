#include <eid.h>
#include <equos.h>
#include <codec_wav.h>
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

void draw_niplay_ui(const char *name, uint64_t processed_bytes, uint64_t total_bytes) {
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

  eid_draw_rect(fb, WIN_W, WIN_H, bar_x, bar_y, bar_w, bar_h, 0x333333); // Фон бара
  if (total_bytes > 0) {
    int progress = (int)((uint64_t)processed_bytes * bar_w / total_bytes);
    if (progress > bar_w) progress = bar_w;
    eid_draw_rect(fb, WIN_W, WIN_H, bar_x, bar_y, progress, bar_h, CLR_ACCENT);
    // "Свечение" на конце бара
    eid_draw_rect(fb, WIN_W, WIN_H, bar_x + progress - 2, bar_y - 2, 4, 10, 0xFFFFFF);
  }

  // 5. Визуализатор (Спектрограмма)
  for (int i = 0; i < 20; i++) {
    int v_h = rand() % 40 + 5;
    eid_draw_rect(fb, WIN_W, WIN_H, 20 + i * 18, 180 - v_h, 12, v_h, CLR_ACCENT);
    eid_draw_rect(fb, WIN_W, WIN_H, 20 + i * 18, 180 - v_h - 4, 12, 2, 0x555555); // "Пики"
  }

  // 6. Таймер (для вывода 16-бит стерео 44100 Гц: 1 секунда = 176400 байт)
  char time_str[16];
  uint32_t sec = processed_bytes / 176400; 
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

  if (!file_data) {
    _syscall(SYS_PRINT, (uint64_t)"[NiPlay] Failed to read file!\n", 0, 0, 0, 0);
    return 1;
  }

  // Инициализируем наш новый кодек из SDK
  wav_decoder_t decoder;
  if (wav_init(&decoder, file_data, file_size) < 0) {
    _syscall(SYS_PRINT, (uint64_t)"[NiPlay] Unsupported WAV format!\n", 0, 0, 0, 0);
    return 1;
  }

  // Жестко задаем аппаратуре частоту 44100 (кодек сам сделает ресемплинг любой частоты под нее!)
  uint32_t target_rate = 44100;
  _syscall(SYS_AUDIO_SET_RATE, target_rate, 0, 0, 0, 0);

  // Локальный потоковый буфер декодирования (16-бит стерео, 2048 сэмплов = 8192 байта)
  #define CHUNK_SAMPLES 2048
  int16_t play_buffer[CHUNK_SAMPLES * 2];
  
  uint64_t total_played_bytes = 0;
  // Считаем эквивалентный размер файла в целевом формате для прогресс-бара
  double rate_ratio = (double)target_rate / (double)decoder.sample_rate;
  size_t src_frame_size = decoder.channels * (decoder.bits_per_sample / 8);
  size_t total_src_frames = decoder.data_size / src_frame_size;
  uint64_t target_total_bytes = (uint64_t)(total_src_frames * rate_ratio) * 4; // 4 байта на стерео сэмпл 16-бит

  int ui_counter = 0;

  while (1) {
    // Декодируем и ресемплируем очередную порцию
    size_t decoded_samples = wav_decode(&decoder, play_buffer, CHUNK_SAMPLES, target_rate);
    if (decoded_samples == 0) {
      break; // Доиграли до конца
    }

    uint32_t bytes_to_play = decoded_samples * 4; // 16-бит стерео = 4 байта на сэмпл

    // Отправляем на воспроизведение
    _syscall(SYS_AUDIO_PLAY, (uintptr_t)play_buffer, (uint64_t)bytes_to_play, 0, 0, 0);
    total_played_bytes += bytes_to_play;

    // Обновляем GUI каждые 8 чанков (~15 FPS)
    if (ui_counter++ % 8 == 0) {
      draw_niplay_ui(filename, total_played_bytes, target_total_bytes);
    }

    if ((uint8_t)_syscall(SYS_GET_SCANCODE, 0, 0, 0, 0, 0) == 0x01)
      break; // ESC для выхода
    sys_yield();
  }

  exit(0);
  return 0;
}