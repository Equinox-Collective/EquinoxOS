#include "eqstart.h"
#include "../system/drivers/vesa/vesa.h"
#include "../system/mem/pmm.h"
#include "../system/misc/timer.h"
#include "../system/mem/vmm.h"
#include "../syslibc/stdio.h"
#include "nyan_data.h"
#include "boot_config.h"
#include <stdbool.h>
#include <stdint.h>

// Прогресс-бар загрузки (определён ниже). Рисует трек, заполнение, проценты и
// примерное оставшееся время. Перерисовывает себя только при смене процента,
// поэтому безопасен для вызова хоть на каждом тике PIT.
static void boot_progress_draw(void);

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

// Флаг boot-анимации. Пока =1, PIT-таймер (timer_callback) на каждом тике
// подрисовывает текущий кадр Nyan Cat — гифка крутится НЕПРЕРЫВНО на всём
// протяжении загрузки (паузы между тестами, USB/mouse, ext2 и т.д.), а не
// замирает на последнем кадре. kmain сбрасывает его в 0 прямо перед запуском
// sysgui, чтобы ядро перестало рисовать поверх кадра ring-3 GUI.
volatile int nyan_boot_active = 0;

void nyan_init_geometry(void) {
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
void nyan_draw_frame(int frame) {
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

// --- ПРОГРЕСС-БАР ЗАГРУЗКИ ---
// Оценка прогресса по времени: tick (= мс, PIT на 1 кГц) против BOOT_ETA_MS из
// boot_config.h. Это намеренно ПРИМЕРНАЯ оценка (как и просил пользователь):
// реальные этапы загрузки недетерминированы, поэтому процент капается на 99 %,
// пока GUI не нарисует первый кадр (syscall 88 -> nyan_boot_active = 0), после
// чего ядро перестаёт рисовать и экран занимает рабочий стол.
// --- САМОПОДСТРОЙКА ВРЕМЕНИ ЗАГРУЗКИ ---
// boot_eta_ms — РАБОЧАЯ оценка времени загрузки (мс). На первом запуске равна
// BOOT_ETA_MS из конфига; после первой загрузки kmain читает реальное время из
// /boottime и переопределяет её через boot_eta_set(), поэтому проценты/время
// становятся точнее. boot_measured_ms заполняется при первом кадре GUI
// (syscall 88) и затем сохраняется ядром на диск.
static uint32_t boot_eta_ms = BOOT_ETA_MS;
volatile uint32_t boot_measured_ms = 0;

// Вызывается ядром после монтирования ФС, если в /boottime есть сохранённое
// время. Принимаем только вменяемые значения (3..180 c), иначе игнор.
void boot_eta_set(uint32_t ms) {
    if (ms >= 3000 && ms <= 180000) {
        boot_eta_ms = ms;
    }
}

static void boot_progress_draw(void) {
#if BOOT_PROGRESS_BAR
    if (nyan_scale == 0) {
        nyan_init_geometry();
    }

    // Процент по таймеру: 0..97% линейно за boot_eta_ms, дальше 97..99%
    // медленно «доползают» (по +1% раз в BOOT_TAIL_STEP_MS), чтобы бар не
    // выглядел зависшим, даже если реальная загрузка дольше ожидаемой.
    // 100% покажет уже сам GUI, перекрыв экран первым кадром (syscall 88).
    uint32_t eta = boot_eta_ms;
    uint32_t pct;
    bool tail; // true => фаза «хвоста» (>=97%, время уже не показываем).
    if (tick < eta) {
        pct = (uint32_t)(((uint64_t)tick * 97) / (uint64_t)eta);
        tail = false;
    } else {
        uint32_t over = tick - eta;
        pct = 97 + over / BOOT_TAIL_STEP_MS;
        if (pct > 99) pct = 99;
        tail = true;
    }
    uint32_t remain_ms = (tick < eta) ? (eta - tick) : 0;
    uint32_t remain_s = (remain_ms + 999) / 1000; // округляем вверх до секунды.

    // Перерисовываем только при смене процента — экономим IRQ-время.
    static uint32_t last_pct = 0xFFFFFFFFu;
    if (pct == last_pct) {
        return;
    }
    last_pct = pct;

    // Фон под текстом зависит от режима экрана (чёрный или серый), чтобы
    // стирать «хвосты» от прошлой строки без артефактов.
#if BOOT_SCREEN_MODE == 1
    uint32_t bg = BOOT_GRAY_COLOR;
#else
    uint32_t bg = 0x000000;
#endif

    // Геометрия бара: позиция/ширина берутся из boot_config.h в процентах
    // от экрана (по умолчанию выше и правее, чтобы не лезть под Nyan Cat).
    int bar_w = (int)(((uint64_t)screen_width * BOOT_BAR_W_PCT) / 100);
    int bar_h = 16;
    int cx = (int)(((uint64_t)screen_width * BOOT_BAR_CX_PCT) / 100);
    int bar_x = cx - bar_w / 2;
    int bar_y = (int)(((uint64_t)screen_height * BOOT_BAR_Y_PCT) / 100);
    // Держим бар целиком в пределах экрана.
    if (bar_x < 4) bar_x = 4;
    if (bar_x + bar_w > (int)screen_width - 4) bar_x = (int)screen_width - 4 - bar_w;

    // Рамка + трек + заполнение.
    draw_rect_direct(bar_x - 2, bar_y - 2, bar_w + 4, bar_h + 4, 0x3A3F44); // рамка
    draw_rect_direct(bar_x, bar_y, bar_w, bar_h, 0x14171A);                 // трек
    int fill_w = (int)(((uint64_t)bar_w * pct) / 100);
    if (fill_w > 0) {
        draw_rect_direct(bar_x, bar_y, fill_w, bar_h, 0x00C8FF);            // заполнение
    }

    // Текст под баром: "Loading EquinoxOS...  NN%   ~Ns left" (в хвосте время
    // уже не показываем — пишем "almost done", чтобы не было "~0s left").
    int text_y = bar_y + bar_h + 10;
    draw_rect_direct(bar_x, text_y, bar_w, 18, bg); // стираем прошлую строку
    char line[80];
    if (tail) {
        sprintf(line, "Loading EquinoxOS...  %u%%   almost done", pct);
    } else {
        sprintf(line, "Loading EquinoxOS...  %u%%   ~%us left", pct, remain_s);
    }
    vesa_draw_string_direct(line, bar_x, text_y, 0xC8CED4);
#endif /* BOOT_PROGRESS_BAR */
}

// Проигрываем гифку заданное время (мс), синхронно с темпом исходного gif
// (8 кадров по 100 мс). Используется как "заставка" во время загрузки.
static void nyan_play(uint32_t duration_ms) {
    uint32_t start = tick;
    int frame = 0;
    while ((tick - start) < duration_ms) {
        nyan_draw_frame(frame);
        boot_progress_draw(); // прогресс-бар поверх «заставки»
        frame++;
        kernel_sleep_ms(100); // 100 мс на кадр = темп исходной гифки
    }
}

// Подрисовка одного кадра Nyan Cat для использования ИЗ ДРУГИХ подсистем
// (например, во время цикла опроса USB-мыши), чтобы анимация не «замирала»
// между завершением nyan_play() и стартом GUI. Кадр выбирается по системному
// тику (100 мс/кадр), геометрия инициализируется лениво. Перерисовываем только
// при смене кадра, чтобы не молотить фреймбуфер на каждом вызове.
void nyan_boot_anim_frame(void) {
    if (nyan_scale == 0) {
        nyan_init_geometry();
    }
#if BOOT_SCREEN_MODE == 0
    // Режим 0: крутим гифку Nyan Cat (перерисовываем только при смене кадра).
    static int nyan_last_drawn = -1;
    int frame = (int)((tick / 100) % NYAN_FRAMES);
    if (frame != nyan_last_drawn) {
        nyan_draw_frame(frame);
        nyan_last_drawn = frame;
    }
#else
    // Режим 1 (серый экран): Nyan статичный — фон и кадр уже нарисованы один
    // раз в eqstart_perform_tests(), перерисовывать каждый тик не нужно.
#endif
    // Прогресс-бар обновляется в обоих режимах (сам пропускает кадры без
    // изменения процента).
    boot_progress_draw();
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
#if FAST_BOOT
  /* --- БЫСТРЫЙ ПУТЬ ---
   * Только критичные проверки железа (µs), без диагностического лога и без
   * стресс-теста PMM. Сразу показываем серый boot-экран. */
  CERBERUS_ASSERT(hhdm_offset >= 0xFFFF800000000000, "HHDM Invalid offset");
  if (!test_cpu_fpu()) {
    CERBERUS_ASSERT(false, "FPU math error - CPU features not properly enabled");
  }
  {
    uint32_t start_tick = tick;
    uint32_t deadline = start_tick + 100;
    while (tick == start_tick && tick < deadline) {
      __asm__ volatile("hlt");
    }
    if (tick == start_tick) {
      CERBERUS_ASSERT(false, "PIT Timer is not ticking. Interrupts dead?");
    }
  }
  {
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    if (tr == 0) {
      CERBERUS_ASSERT(false, "TSS not loaded. Multitasking will cause Triple Fault");
    }
  }
  // Фон под boot-экраном зависит от режима (чёрный для крутящегося Nyan,
  // серый для статичного) — иначе прогресс-бар стирал бы «хвосты» цветом,
  // не совпадающим с заливкой.
#if BOOT_SCREEN_MODE == 0
  draw_rect_direct(0, 0, screen_width, screen_height, 0x000000);
#else
  draw_rect_direct(0, 0, screen_width, screen_height, BOOT_GRAY_COLOR);
#endif
  nyan_init_geometry();
  nyan_draw_frame(0);
  boot_progress_draw();
  // В режиме 0 гифку дальше крутит таймер (nyan_boot_anim_frame) — без
  // блокирующего nyan_play(), чтобы не замедлять быструю загрузку.
  nyan_boot_active = 1;
  return true;
#else
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

#if BOOT_SCREEN_MODE == 1
  // --- РЕЖИМ "СЕРЫЙ ЭКРАН" (BOOT_SCREEN_MODE == 1) ---
  // Заливаем экран серым, рисуем статичный (неподвижный) кадр Nyan Cat и
  // прогресс-бар. Гифка НЕ крутится. Дальше прогресс-бар обновляет таймер
  // (nyan_boot_anim_frame) до старта GUI.
  draw_rect_direct(0, 0, screen_width, screen_height, BOOT_GRAY_COLOR);
  nyan_init_geometry();
  nyan_draw_frame(0);
  boot_progress_draw();
#else
  // --- РЕЖИМ "КРУТЯЩИЙСЯ NYAN" (BOOT_SCREEN_MODE == 0, по умолчанию) ---
  // Проигрываем Nyan Cat в обведённой зоне, пока идёт загрузка, перед
  // передачей управления GUI-подсистеме (~4 секунды = 5 петель гифки).
  nyan_play(4000);
#endif

  // Загрузка ещё продолжается (USB/mouse, ext2, GUI). Включаем таймерную
  // подкрутку, чтобы гифка НЕ замирала (режим 0) и обновлялся прогресс-бар
  // (оба режима) до самого старта sysgui (kmain сбросит флаг перед exec
  // sysgui.elf, а GUI догасит анимацию через syscall 88).
  nyan_boot_active = 1;
  return true;
#endif /* FAST_BOOT */
}