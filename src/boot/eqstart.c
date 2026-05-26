#include "eqstart.h"
#include "../system/drivers/vesa/vesa.h"
#include "../system/mem/pmm.h"
#include "../system/misc/timer.h"
#include "../system/mem/vmm.h"
#include <stdbool.h>
#include <stdint.h>

// Задержка для "кинематографичности" (имитация работы)
#define BOOT_DELAY 15000000
#define STATUS_X 550

static int log_row = 0;

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

static void log_info(const char *msg) {
  vesa_draw_string_direct(">>", 40, 60 + (log_row * 18), 0x00FF00);
  vesa_draw_string_direct(msg, 70, 60 + (log_row * 18), 0xCCCCCC);
}

static void log_status(const char *status, uint32_t color) {
  vesa_draw_string_direct("[", STATUS_X, 60 + (log_row * 18), 0xAAAAAA);
  vesa_draw_string_direct(status, STATUS_X + 15, 60 + (log_row * 18), color);
  vesa_draw_string_direct("]", STATUS_X + 60, 60 + (log_row * 18), 0xAAAAAA);
  log_row++;
}

// --- РЕАЛЬНЫЕ ТЕСТЫ ---

// 1. Проверка маппинга NULL-страницы (Защита от нулевых указателей)
bool test_vmm_null_protection() {
  log_info("VMM: Checking NULL-pointer protection...");

  // Получаем адрес текущей PML4 таблицы
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

  // В x86_64 CR3 хранит физический адрес. Переводим в виртуальный через HHDM.
  uint64_t *pml4 = (uint64_t *)(cr3 + hhdm_offset);

  // Проверяем первую запись (она отвечает за 0x0 - 0x7FFFFFFFFFFF)
  // Если она Present, значит риск разыменования NULL выше.
  if (pml4[0] & PTE_PRESENT) {
    // Если замаплено, проверяем, нет ли там PTE_USER. Юзеру там быть нельзя!
    if (pml4[0] & PTE_USER) {
      log_status("DANGER", 0xFF0000);
      return false;
    }
    log_status("WARN", 0xFFFF00);
  } else {
    log_status("SAFE", 0x00FF00);
  }
  return true;
}

// 2. Стресс-тест PMM (Выделение и проверка целостности данных)
bool test_pmm_stress() {
  log_info("PMM: Stress-testing physical allocator...");
  void *test_pages[32];

  // Выделяем страницы и пишем в них уникальный мусор
  for (int i = 0; i < 32; i++) {
    test_pages[i] = pmm_alloc();
    if (!test_pages[i]) {
      log_status("NOMEM", 0xFF0000);
      return false;
    }

    uint64_t *ptr = (uint64_t *)((uint64_t)test_pages[i] + hhdm_offset);
    *ptr = 0xABCDEF0123456789 ^ (uint64_t)test_pages[i];
  }

  // Проверяем, не перезаписали ли страницы друг друга
  for (int i = 0; i < 32; i++) {
    uint64_t *ptr = (uint64_t *)((uint64_t)test_pages[i] + hhdm_offset);
    if (*ptr != (0xABCDEF0123456789 ^ (uint64_t)test_pages[i])) {
      log_status("CORRUPT", 0xFF0000);
      return false;
    }
    pmm_free(test_pages[i]);
  }
  log_status("PASSED", 0x00FF00);
  return true;
}

// 3. Проверка FPU/SSE (Важно для многозадачности)
bool test_cpu_fpu() {
  log_info("CPU: Verifying FPU/SSE state integrity...");
  volatile float f1 = 3.14f;
  volatile float f2 = 2.71f;
  if ((int)(f1 * f2) != 8) { // 3.14 * 2.71 = 8.5094
    log_status("FAULT", 0xFF0000);
    return false;
  }
  log_status("OK", 0x00FF00);
  return true;
}

bool eqstart_perform_tests() {
  // Очистка экрана в "терминальный" стиль
  draw_rect_direct(0, 0, screen_width, screen_height, 0x020202);
  log_row = 0;

  vesa_draw_string_direct("EQUINOX OS BOOT PROTOCOL v2.1", 50, 30, 0x00FFFF);
  vesa_draw_string_direct("------------------------------------------", 50, 45,
                          0x444444);

  // Тест 1: HHDM (база системы)
  log_info("HHDM: Mapping verification...");
  CERBERUS_ASSERT(hhdm_offset >= 0xFFFF800000000000, "HHDM Invalid offset");
  log_status("OK", 0x00FF00);

  // Тест 2: VMM Security
  // if (!test_vmm_null_protection()) {
  //   CERBERUS_ASSERT(false, "VMM security breach: User access to NULL page");
  // }

  // Тест 3: PMM Stress
  if (!test_pmm_stress()) {
    CERBERUS_ASSERT(false, "PMM memory corruption detected");
  }

  // Тест 4: CPU FPU
  if (!test_cpu_fpu()) {
    CERBERUS_ASSERT(false,
                    "FPU math error - CPU features not properly enabled");
  }

  // Тест 5: Heartbeat (PIT).
  //
  // Раньше тут был `for (volatile int i = 0; i < 15000000; i++);` без `hlt`.
  // На WHPX/KVM с быстрым CPU этот busy-loop отрабатывал заметно быстрее,
  // чем успевал тикнуть PIT (~1 мс), и тест случайным образом валился с
  // "PIT Timer is not ticking. Interrupts dead?" на каждом 2-3 запуске.
  // Чиним: ждём ДО 100 мс в `hlt`-цикле, при первом же тике объявляем PIT
  // живым. CPU спит, гипервизор гарантированно прокидывает прерывание.
  log_info("TIME: Testing interrupt fire rate...");
  uint32_t start_tick = tick;
  uint32_t deadline = start_tick + 100; // ждём максимум 100 мс
  while (tick == start_tick && tick < deadline) {
    __asm__ volatile("hlt");
  }
  if (tick == start_tick) {
    log_status("FROZEN", 0xFF0000);
    CERBERUS_ASSERT(false, "PIT Timer is not ticking. Interrupts dead?");
  }
  log_status("STABLE", 0x00FF00);

  // Тест 6: GDT/TSS
  log_info("GDT: Checking Task State Segment...");
  uint16_t tr;
  __asm__ volatile("str %0" : "=r"(tr));
  if (tr == 0) {
    log_status("MISSING", 0xFF0000);
    CERBERUS_ASSERT(false,
                    "TSS not loaded. Multitasking will cause Triple Fault");
  }
  log_status("LOADED", 0x00FF00);

  vesa_draw_string_direct("------------------------------------------", 50,
                          60 + (log_row * 18), 0x444444);
  log_row++;
  log_info("SYSTEM READY. HANDING OVER CONTROL...");
  log_status("BOOT", 0x00FFFF);
  return true;
}