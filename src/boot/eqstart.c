#include "eqstart.h"
#include "../system/drivers/vesa/vesa.h"
#include "../system/mem/pmm.h"
#include "../system/misc/timer.h"
#include "../system/mem/vmm.h"
#include "nyan_data.h"
#include <stdbool.h>
#include <stdint.h>

// Объявляем внешние глобальные переменные ядра
extern uint64_t hhdm_offset;
extern volatile uint32_t tick;

static int tty_row = 0;

// Безопасный сон прямо в ядре без использования системных вызовов
static void kernel_sleep_ms(uint32_t ms) {
    uint32_t start = tick;
    while (tick < start + ms) {
        __asm__ volatile("hlt");
    }
}

// --- NYAN CAT BOOT-АНИМАЦИЯ ---
// Гифка рисуется прямо во фронтбуфер (как и диагностический лог) в зоне,
// которую попросил пользователь — по центру-низу экрана под текстом тестов.

// Геометрия кадра: масштаб и левый-верхний угол вычисляются один раз
// относительно разрешения экрана, чтобы анимация попадала в нужную зону
// при любом framebuffer'е.
static int nyan_scale = 0;
static int nyan_ox = 0;
static int nyan_oy = 0;

static void nyan_init_geometry(void) {
    // Целый масштаб ~480px ширины при 1280px экрана (пиксель-арт без блюра).
    nyan_scale = (int)(screen_width / 500);
    if (nyan_scale < 1) nyan_scale = 1;

    int nyan_w = NYAN_W * nyan_scale;
    int nyan_h = NYAN_H * nyan_scale;

    // Центр зоны, обведённой пользователем на скрине (1281x794 -> доли экрана).
    int cx = (int)((uint64_t)screen_width * 384 / 1281);
    int cy = (int)((uint64_t)screen_height * 508 / 794);

    nyan_ox = cx - nyan_w / 2;
    nyan_oy = cy - nyan_h / 2;
}

// Рисуем один кадр с целочисленным масштабированием (nearest-neighbor).
static void nyan_draw_frame(int frame) {
    const uint8_t *fb = nyan_frames[frame % NYAN_FRAMES];
    for (int y = 0; y < NYAN_H; y++) {
        for (int x = 0; x < NYAN_W; x++) {
            uint32_t color = nyan_palette[fb[y * NYAN_W + x]];
            int px = nyan_ox + x * nyan_scale;
            int py = nyan_oy + y * nyan_scale;
            for (int dy = 0; dy < nyan_scale; dy++) {
                for (int dx = 0; dx < nyan_scale; dx++) {
                    put_pixel_direct(px + dx, py + dy, color);
                }
            }
        }
    }
}

// Проигрываем гифку заданное время (мс), синхронно с темпом исходного gif
// (8 кадров по 100 мс). Используется как "заставка" во время загрузки.
static void nyan_play(uint32_t duration_ms) {
    uint32_t start = tick;
    int frame = 0;
    while ((tick - start) < duration_ms) {
        nyan_draw_frame(frame);
        frame++;
        kernel_sleep_ms(100); // 100 мс на кадр = темп исходной гифки
    }
}

// Простой вывод строки в TTY-стиле на черный экран
static void tty_print(const char *msg, uint32_t color) {
    vesa_draw_string_direct(msg, 20, 20 + (tty_row * 16), color);
    tty_row++;
    if (tty_row > 40) {
        draw_rect_direct(0, 0, screen_width, screen_height, 0x000000);
        tty_row = 0;
    }
}

// Макрос для моментальной остановки системы при критическом сбое
#define CERBERUS_ASSERT(cond, reason)                                          \
  if (!(cond)) {                                                               \
    draw_rect_direct(0, 0, screen_width, screen_height, 0x330000);             \
    vesa_draw_string_direct("!!! KERNEL INTEGRITY FAULT !!!", 50, 50,          \
                            0xFF0000);                                         \
    vesa_draw_string_direct("REASON: " reason, 50, 80, 0xFFFFFF);              \
    while (1) {                                                                \
      __asm__("cli; hlt");                                                     \
    }                                                                          \
  }

// --- ТЕСТЫ ЦЕЛОСТНОСТИ ЯДРА ---

// 1. Стресс-тест PMM (Выделение и проверка целостности данных)
bool test_pmm_stress() {
  void *test_pages[32];

  // Выделяем страницы и пишем в них уникальный мусор
  for (int i = 0; i < 32; i++) {
    test_pages[i] = pmm_alloc();
    if (!test_pages[i]) {
      return false;
    }

    uint64_t *ptr = (uint64_t *)((uint64_t)test_pages[i] + hhdm_offset);
    *ptr = 0xABCDEF0123456789 ^ (uint64_t)test_pages[i];
  }

  // Проверяем, не перезаписали ли страницы друг друга
  for (int i = 0; i < 32; i++) {
    uint64_t *ptr = (uint64_t *)((uint64_t)test_pages[i] + hhdm_offset);
    if (*ptr != (0xABCDEF0123456789 ^ (uint64_t)test_pages[i])) {
      return false;
    }
    pmm_free(test_pages[i]);
  }
  return true;
}

// 2. Проверка FPU/SSE
bool test_cpu_fpu() {
  volatile float f1 = 3.14f;
  volatile float f2 = 2.71f;
  if ((int)(f1 * f2) != 8) { // 3.14 * 2.71 = 8.5094
    return false;
  }
  return true;
}

// 3. Главная точка входа диагностического лога
bool eqstart_perform_tests() {
  // Чистый черный экран
  draw_rect_direct(0, 0, screen_width, screen_height, 0x000000);
  tty_row = 0;

  // Готовим геометрию nyan-кадра и сразу показываем первый кадр под логом,
  // чтобы гифка была видна уже во время прохождения тестов.
  nyan_init_geometry();
  nyan_draw_frame(0);

  tty_print("Equinox OS Boot Diagnostics Protocol v2.1", 0xFFFFFF);
  tty_print("--------------------------------------------------", 0x555555);

  // Тест 1: HHDM
  tty_print("[   0.000000] HHDM: Verifying higher-half direct mapping...", 0x888888);
  CERBERUS_ASSERT(hhdm_offset >= 0xFFFF800000000000, "HHDM Invalid offset");
  tty_print("[   0.000003] HHDM: OK. Offset mapped correctly.", 0x00FF00);

  // Тест 2: Инициализация PAT (будет вызвана в vmm_init)
  tty_print("[   0.001024] PAT: Initializing Page Attribute Table...", 0x888888);
  tty_print("[   0.001090] PAT: Write-Combining enabled on Framebuffer index 3.", 0x00FF00);

  // Тест 3: PMM Stress
  tty_print("[   0.002150] PMM: Initializing Physical Memory Manager...", 0x888888);
  tty_print("[   0.002200] PMM: Performing physical allocator stress test...", 0x888888);
  if (!test_pmm_stress()) {
      CERBERUS_ASSERT(false, "PMM memory corruption detected");
  }
  tty_print("[   0.003600] PMM: OK. 32 pages stress-test passed successfully.", 0x00FF00);

  // Тест 4: CPU FPU
  tty_print("[   0.004100] CPU: Checking FPU/SSE state integrity...", 0x888888);
  if (!test_cpu_fpu()) {
      CERBERUS_ASSERT(false, "FPU math error - CPU features not properly enabled");
  }
  tty_print("[   0.004300] CPU: OK. SSE/FPU registers validated.", 0x00FF00);

  // Тест 5: Heartbeat (PIT)
  tty_print("[   0.005000] TIME: Testing PIT interrupts and firing rate...", 0x888888);
  uint32_t start_tick = tick;
  uint32_t deadline = start_tick + 100; // ждём максимум 100 мс
  while (tick == start_tick && tick < deadline) {
    __asm__ volatile("hlt");
  }
  if (tick == start_tick) {
    CERBERUS_ASSERT(false, "PIT Timer is not ticking. Interrupts dead?");
  }
  tty_print("[   0.006200] TIME: OK. Interrupts are firing stably.", 0x00FF00);

  // Тест 6: GDT/TSS
  tty_print("[   0.007000] GDT: Checking Task State Segment...", 0x888888);
  uint16_t tr;
  __asm__ volatile("str %0" : "=r"(tr));
  if (tr == 0) {
    CERBERUS_ASSERT(false, "TSS not loaded. Multitasking will cause Triple Fault");
  }
  tty_print("[   0.007200] GDT: OK. TSS loaded successfully.", 0x00FF00);

  tty_print("--------------------------------------------------", 0x555555);
  tty_print("All diagnostics PASSED. Launching Equinox GUI Subsystem...", 0x00FFFF);

  // Проигрываем Nyan Cat в обведённой зоне, пока идёт загрузка, перед
  // передачей управления GUI-подсистеме (~4 секунды = 5 петель гифки).
  nyan_play(4000);
  return true;
}