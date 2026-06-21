#include "serial.h"
#include "../../../core/io.h"

// Serial port register offsets
#define SERIAL_DATA         0
#define SERIAL_INT_ENABLE   1
#define SERIAL_FIFO_CTRL    2
#define SERIAL_LINE_CTRL    3
#define SERIAL_MODEM_CTRL   4
#define SERIAL_LINE_STATUS  5
#define SERIAL_MODEM_STATUS 6
#define SERIAL_SCRATCH      7

// Line status bits
#define SERIAL_LS_DATA_READY        0x01
#define SERIAL_LS_OVERRUN_ERROR     0x02
#define SERIAL_LS_PARITY_ERROR      0x04
#define SERIAL_LS_FRAMING_ERROR     0x08
#define SERIAL_LS_BREAK_INDICATOR   0x10
#define SERIAL_LS_TRANSMIT_EMPTY    0x20
#define SERIAL_LS_TRANSMIT_IDLE     0x40
#define SERIAL_LS_FIFO_ERROR        0x80

// Простой спинлок для синхронизации вывода между ядрами/потоками
static volatile int serial_lock = 0;

static inline void serial_acquire_lock(void) {
    while (__sync_lock_test_and_set(&serial_lock, 1)) {
        __asm__ volatile("pause");
    }
}

static inline void serial_release_lock(void) {
    __sync_lock_release(&serial_lock);
}

void serial_init(uint16_t port) {
    // Disable interrupts
    outb(port + SERIAL_INT_ENABLE, 0x00);
    
    // Enable DLAB (set baud rate divisor)
    outb(port + SERIAL_LINE_CTRL, 0x80);
    
    // Set divisor to 1 (115200 baud)
    outb(port + SERIAL_DATA, 0x01);
    outb(port + SERIAL_INT_ENABLE, 0x00);
    
    // 8 bits, no parity, one stop bit
    outb(port + SERIAL_LINE_CTRL, 0x03);
    
    // Enable FIFO, clear them, with 14-byte threshold
    outb(port + SERIAL_FIFO_CTRL, 0xC7);
    
    // IRQs enabled, RTS/DSR set
    outb(port + SERIAL_MODEM_CTRL, 0x0B);
    
    // Set in loopback mode to test the serial chip
    outb(port + SERIAL_MODEM_CTRL, 0x1E);
    
    // Test serial chip by sending a byte
    outb(port + SERIAL_DATA, 0xAE);
    
    // Check if serial is faulty (i.e., not same byte as sent)
    if (inb(port + SERIAL_DATA) != 0xAE) {
        return; // Serial is faulty
    }
    
    // Set in normal operation mode
    outb(port + SERIAL_MODEM_CTRL, 0x0F);
}

int serial_transmit_empty(uint16_t port) {
    return inb(port + SERIAL_LINE_STATUS) & SERIAL_LS_TRANSMIT_EMPTY;
}

void serial_putchar(uint16_t port, char c) {
    // Wait for transmit buffer to be empty
    while (serial_transmit_empty(port) == 0);
    outb(port + SERIAL_DATA, c);
}

void serial_puts(uint16_t port, const char *str) {
    serial_acquire_lock();
    while (*str) {
        serial_putchar(port, *str++);
    }
    serial_release_lock();
}

int serial_received(uint16_t port) {
    return inb(port + SERIAL_LINE_STATUS) & SERIAL_LS_DATA_READY;
}

char serial_getchar(uint16_t port) {
    while (serial_received(port) == 0);
    return inb(port + SERIAL_DATA);
}