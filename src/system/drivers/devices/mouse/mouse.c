#include "mouse.h"
#include "../../../core/io.h"
#include "../../vesa/vesa.h"

extern void term_print(const char* str); 

// --- ПОРТЫ PS/2 КОНТРОЛЛЕРА ---
#define PS2_DATA_PORT         0x60
#define PS2_CMD_PORT          0x64

#define PS2_CTRL_READ_CCB     0x20
#define PS2_CTRL_WRITE_CCB    0x60
#define PS2_CTRL_DISABLE_AUX  0xA7
#define PS2_CTRL_ENABLE_AUX   0xA8
#define PS2_CTRL_TEST         0xAA
#define PS2_CTRL_ENABLE_PORT  0xAE
#define PS2_CTRL_WRITE_MOUSE  0xD4

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
//                   УНИВЕРСАЛЬНЫЙ ОБНОВИТЕЛЬ ДЛЯ USB МЫШИ
// =========================================================================

void usb_mouse_update(int8_t dx, int8_t dy, uint8_t buttons) {
    mouse_x += dx;
    mouse_y += dy; // У USB-мышей направление дельты Y стандартное (+Y = вниз)

    mouse_left_button = buttons & 0x01;
    mouse_right_button = (buttons >> 1) & 0x01;

    // Клампим к размерам экрана
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x >= (int32_t)screen_width) mouse_x = screen_width - 1;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y >= (int32_t)screen_height) mouse_y = screen_height - 1;
}

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
    
    if (!(status & 0x01)) {
        return; 
    }

    uint8_t data = inb(PS2_DATA_PORT);

    if (!(status & 0x20)) {
        keyboard_push(data);
        return;
    }

    switch(mouse_cycle) {
        case 0:
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

            int8_t delta_x = mouse_packet[1];
            int8_t delta_y = mouse_packet[2];

            mouse_left_button = mouse_packet[0] & 0x01;
            mouse_right_button = (mouse_packet[0] >> 1) & 0x01;

            mouse_x += delta_x;
            mouse_y -= delta_y; // У PS/2-мыши дельта Y инвертирована

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

bool init_mouse() {
    mouse_flush_buffer(); 

    mouse_wait_input(); 
    outb(PS2_CMD_PORT, PS2_CTRL_DISABLE_AUX); 

    mouse_wait_input(); 
    outb(PS2_CMD_PORT, PS2_CTRL_READ_CCB); 
    uint8_t ccb = mouse_read();

    ccb |= 0x02;  
    ccb &= ~0x20; 
    
    mouse_wait_input(); 
    outb(PS2_CMD_PORT, PS2_CTRL_WRITE_CCB); 
    mouse_wait_input(); 
    outb(PS2_DATA_PORT, ccb);

    mouse_wait_input(); 
    outb(PS2_CMD_PORT, PS2_CTRL_ENABLE_AUX);

    // Тест PS/2 контроллера
    mouse_wait_input(); 
    outb(PS2_CMD_PORT, PS2_CTRL_TEST); 
    uint8_t test_res = mouse_read();
    if (test_res != 0x55) {
        term_print("[SYS] PS/2 Controller Diagnostic Failed!\n");
        return false;
    }

    mouse_wait_input(); 
    outb(PS2_CMD_PORT, PS2_CTRL_ENABLE_PORT); 

    mouse_write(MOUSE_CMD_RESET);
    mouse_read(); // ACK
    mouse_read(); // ID

    mouse_write(MOUSE_CMD_DEFAULTS);
    mouse_read();

    mouse_write(MOUSE_CMD_SAMPLE_RATE);
    mouse_read();
    mouse_write(200);  
    mouse_read();

    mouse_write(MOUSE_CMD_ENABLE_DATA);
    mouse_read();

    term_print("[SYS] PS/2 Mouse Initialized\n");
    return true;
}