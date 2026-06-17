#include "eqstart.h"
#include "../system/drivers/vesa/vesa.h"
#include "../system/mem/pmm.h"
#include "../system/misc/timer.h"
#include "../system/mem/vmm.h"
#include "../syslibc/stdio.h"
#include "../syslibc/string.h"
#include "boot_config.h"
#include <stdbool.h>
#include <stdint.h>

extern uint64_t hhdm_offset;
extern volatile uint32_t tick;
volatile int nyan_boot_active = 0;
volatile uint32_t boot_measured_ms = 0;

// --- ПАРАМЕТРЫ ТЕКСТОВОЙ КОНСОЛИ ---
static int term_cursor_x = 0;
static int term_cursor_y = 0;
#define LINE_HEIGHT 12
#define CHAR_WIDTH 8

// Быстрая очистка экрана под консоль
static void console_clear(void) {
    draw_rect_direct(0, 0, screen_width, screen_height, 0x07080B); // Deep Dark background
    term_cursor_x = 20;
    term_cursor_y = 20;
}

// Попиксельный скроллинг экрана вверх при заполнении
static void console_scroll_up(void) {
    uint32_t line_bytes = LINE_HEIGHT * screen_width * 4;
    uint32_t total_bytes = screen_height * screen_width * 4;
    
    // Сдвигаем backbuffer ядра вверх на одну строку
    memcpy(backbuffer, (void*)((uintptr_t)backbuffer + line_bytes), total_bytes - line_bytes);
    
    // Очищаем освободившуюся строку внизу
    memset((void*)((uintptr_t)backbuffer + total_bytes - line_bytes), 0, line_bytes);
    
    // Заливаем фоном нижнюю строку, чтобы не было черной полосы
    draw_rect_direct(0, screen_height - LINE_HEIGHT, screen_width, LINE_HEIGHT, 0x07080B);
    
    term_cursor_y -= LINE_HEIGHT;
}

// Интеллектуальный парсер цвета для сообщений ядра в стиле dmesg
static uint32_t get_keyword_color(const char *str) {
    if (strstr(str, "OK") || strstr(str, "present") || strstr(str, "passed") || strstr(str, "validated")) {
        return 0x98C379; // Приятный зеленый (OneDark)
    }
    if (strstr(str, "ERROR") || strstr(str, "MISSING") || strstr(str, "failed") || strstr(str, "integrity fault")) {
        return 0xE06C75; // Красный
    }
    if (strstr(str, "Warning") || strstr(str, "warning") || strstr(str, "unavailable")) {
        return 0xD19A66; // Оранжевый/Желтый
    }
    if (strstr(str, "[PCI]") || strstr(str, "[USB]") || strstr(str, "[SHM]") || strstr(str, "[HAL]") || strstr(str, "[init]")) {
        return 0x61AFEF; // Голубой
    }
    if (strstr(str, "Starting") || strstr(str, "Ready")) {
        return 0xC678DD; // Фиолетовый
    }
    return 0xABB2BF; // Стандартный светло-серый
}

// Системная функция вывода сырого символа на экран
void kprint_char(char c, uint32_t color) {
    if (c == '\n') {
        term_cursor_x = 20;
        term_cursor_y += LINE_HEIGHT;
        if (term_cursor_y >= (int)screen_height - 30) {
            console_scroll_up();
        }
        return;
    }
    if (c == '\r') {
        term_cursor_x = 20;
        return;
    }

    // Рендерим символ из встроенного шрифта 8x8
    vesa_draw_char_direct(c, term_cursor_x, term_cursor_y, color);
    term_cursor_x += CHAR_WIDTH;

    if (term_cursor_x >= (int)screen_width - 20) {
        term_cursor_x = 20;
        term_cursor_y += LINE_HEIGHT;
        if (term_cursor_y >= (int)screen_height - 30) {
            console_scroll_up();
        }
    }
}

// Глобальная функция печати строки ядра с авто-подсветкой синтаксиса
void kprint_raw(const char *str) {
    if (!str) return;
    
    uint32_t color = get_keyword_color(str);
    while (*str) {
        kprint_char(*str, color);
        str++;
    }
    
    // Мгновенно выводим изменения во фреймбуфер
    vesa_update();
}

// Заглушка для старой анимации
void nyan_init_geometry(void) {}
void nyan_draw_frame(int frame) { (void)frame; }
void nyan_boot_anim_frame(void) {}

// Проверка FPU
static bool test_cpu_fpu(void) {
  volatile float f1 = 3.14f;
  volatile float f2 = 2.71f;
  return ((int)(f1 * f2) == 8);
}

// Точка входа в запуск ядра
bool eqstart_perform_tests(void) {
    console_clear();
    
    kprint_raw("Equinox OS Loader starting up...\n");
    kprint_raw("[ TIME ] Validating PIT timer hardware...\n");
    
    // Мгновенный тест таймера
    uint32_t start_tick = tick;
    uint32_t deadline = start_tick + 100;
    while (tick == start_tick && tick < deadline) {
        __asm__ volatile("hlt");
    }
    
    if (tick == start_tick) {
        draw_rect_direct(0, 0, screen_width, screen_height, 0x330000);
        vesa_draw_string_direct("!!! CRITICAL ERROR: PIT TIMER IS DEAD !!!", 50, 50, 0xFF0000);
        while(1) { __asm__("cli; hlt"); }
    }
    
    kprint_raw("[  OK  ] PIT Timer active.\n");
    
    kprint_raw("[ CPU  ] Testing FPU/SSE vector units...\n");
    if (!test_cpu_fpu()) {
        draw_rect_direct(0, 0, screen_width, screen_height, 0x330000);
        vesa_draw_string_direct("!!! CRITICAL ERROR: FPU TEST FAILED !!!", 50, 50, 0xFF0000);
        while(1) { __asm__("cli; hlt"); }
    }
    kprint_raw("[  OK  ] FPU/SSE mathematical state validated.\n");
    
    return true;
}

void boot_eta_set(uint32_t ms) {
    (void)ms;
}