#include "mouse.h"
#include "../../../core/io.h"
#include "../../vesa/vesa.h"

extern void term_print(const char* str); // Для аккуратного вывода в терминал

// --- ПОРТЫ PS/2 КОНТРОЛЛЕРА ---
#define PS2_DATA_PORT         0x60
#define PS2_CMD_PORT          0x64

// --- КОМАНДЫ КОНТРОЛЛЕРА ---
#define PS2_CTRL_READ_CCB     0x20
#define PS2_CTRL_WRITE_CCB    0x60
#define PS2_CTRL_DISABLE_AUX  0xA7
#define PS2_CTRL_ENABLE_AUX   0xA8
#define PS2_CTRL_TEST         0xAA
#define PS2_CTRL_ENABLE_PORT  0xAE
#define PS2_CTRL_WRITE_MOUSE  0xD4

// --- КОМАНДЫ МЫШИ ---
#define MOUSE_CMD_RESET       0xFF
#define MOUSE_CMD_DEFAULTS    0xF6
#define MOUSE_CMD_SAMPLE_RATE 0xF3
#define MOUSE_CMD_ENABLE_DATA 0xF4

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ---
volatile int32_t mouse_x = 0;
volatile int32_t mouse_y = 0;
volatile uint8_t mouse_left_button = 0;
volatile uint8_t mouse_right_button = 0;
extern void keyboard_push(uint8_t scancode);
static uint8_t mouse_packet[3];
static int mouse_cycle = 0;

// =========================================================================
//                   ВНУТРЕННИЕ ФУНКЦИИ (STATIC)
// =========================================================================

static void mouse_wait_input() {
    uint32_t timeout = 100000; 
    while ((inb(PS2_CMD_PORT) & 2) && timeout--);
}

static void mouse_wait_output() {
    uint32_t timeout = 100000;
    while (!(inb(PS2_CMD_PORT) & 1) && timeout--);
}

static void mouse_write(uint8_t data) {
    mouse_wait_input();
    outb(PS2_CMD_PORT, PS2_CTRL_WRITE_MOUSE);
    mouse_wait_input();
    outb(PS2_DATA_PORT, data);
}

static uint8_t mouse_read() {
    mouse_wait_output();
    return inb(PS2_DATA_PORT);
}

static void mouse_flush_buffer() {
    uint32_t timeout = 100000;
    while ((inb(PS2_CMD_PORT) & 1) && timeout--) {
        inb(PS2_DATA_PORT);
    }
}

// =========================================================================
//                   ОБРАБОТЧИК ПРЕРЫВАНИЯ (IRQ 12)
// =========================================================================

void mouse_callback() {
    uint8_t status = inb(PS2_CMD_PORT);
    
    // 1. Проверяем, есть ли вообще данные в выходном буфере (бит 0)
    if (!(status & 0x01)) {
        return; 
    }

    // 2. ОБЯЗАТЕЛЬНО читаем байт из порта данных (0x60) сразу!
    // Это гарантирует, что контроллер PS/2 никогда не зависнет.
    uint8_t data = inb(PS2_DATA_PORT);

    // 3. Проверяем бит 5 статуса: 1 - данные от мыши, 0 - данные от клавиатуры
    if (!(status & 0x20)) {
        // Ого! Клавиатурный скан-код ошибочно вызвал прерывание мыши.
        // Спасаем его и перенаправляем в кольцевой буфер клавиатуры!
        keyboard_push(data);
        return;
    }

    // 4. Если это действительно данные мыши, обрабатываем их как обычно:
    switch(mouse_cycle) {
        case 0:
            // Бит 3 должен быть установлен в 1 для корректной синхронизации пакета
            if (!(data & 0x08)) {
                mouse_cycle = 0; 
                return;
            }
            mouse_packet[0] = data;
            mouse_cycle++;
            break;

        case 1:
            mouse_packet[1] = data;
            mouse_cycle++;
            break;

        case 2:
            mouse_packet[2] = data;
            mouse_cycle = 0; 

            // --- Парсим полный пакет ---
            int8_t delta_x = mouse_packet[1];
            int8_t delta_y = mouse_packet[2];

            mouse_left_button = mouse_packet[0] & 0x01;
            mouse_right_button = (mouse_packet[0] >> 1) & 0x01;

            mouse_x += delta_x;
            mouse_y -= delta_y;

            // Ограничиваем курсор размерами экрана
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x >= (int32_t)screen_width) mouse_x = screen_width - 1;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y >= (int32_t)screen_height) mouse_y = screen_height - 1;
            
            break;
    }
}
// =========================================================================
//                           ИНИЦИАЛИЗАЦИЯ
// =========================================================================

void init_mouse() {
    mouse_flush_buffer(); 

    // 1. Отключаем мышь
    mouse_wait_input(); 
    outb(PS2_CMD_PORT, PS2_CTRL_DISABLE_AUX); 

    // 2. Читаем Controller Command Byte (CCB)
    mouse_wait_input(); 
    outb(PS2_CMD_PORT, PS2_CTRL_READ_CCB); 
    uint8_t ccb = mouse_read();

    // 3. Изменяем CCB: Включаем IRQ12 (бит 1), очищаем Clock (бит 5)
    ccb |= 0x02;  
    ccb &= ~0x20; 
    
    // 4. Записываем измененный CCB
    mouse_wait_input(); 
    outb(PS2_CMD_PORT, PS2_CTRL_WRITE_CCB); 
    mouse_wait_input(); 
    outb(PS2_DATA_PORT, ccb);

    // 5. Включаем мышь на уровне контроллера
    mouse_wait_input(); 
    outb(PS2_CMD_PORT, PS2_CTRL_ENABLE_AUX);

    // 6. Проверка PS/2 контроллера
    mouse_wait_input(); 
    outb(PS2_CMD_PORT, PS2_CTRL_TEST); 
    mouse_read(); // Должен вернуть 0x55

    // 7. Активируем порт
    mouse_wait_input(); 
    outb(PS2_CMD_PORT, PS2_CTRL_ENABLE_PORT); 

    // 8. Настройка самой мыши
    mouse_write(MOUSE_CMD_RESET);
    mouse_read(); // ACK (0xFA)
    mouse_read(); // ID (0xAA)

    mouse_write(MOUSE_CMD_DEFAULTS);
    mouse_read();

    // Установка частоты (200 сэмплов)
    mouse_write(MOUSE_CMD_SAMPLE_RATE);
    mouse_read();
    mouse_write(200);  
    mouse_read();

    // Включаем передачу данных от мыши
    mouse_write(MOUSE_CMD_ENABLE_DATA);
    mouse_read();

    term_print("[SYS] PS/2 Mouse Initialized\n");
}