#include "xhci.h"

extern void term_print(const char* str);

void xhci_init(uint8_t bus, uint8_t slot, uint8_t func, uintptr_t mmio_base) {
    (void)bus; (void)slot; (void)func;
    volatile uint8_t *cap_regs = (volatile uint8_t *)mmio_base;

    term_print("[xHCI] Initializing USB 3.0 SuperSpeed Controller...\n");

    uint8_t cap_length = cap_regs[0];
    volatile uint32_t *op_regs = (volatile uint32_t *)(mmio_base + cap_length);

    // Сброс xHCI контроллера (бит 1 / HCRST в USBCMD, офсет op_regs + 0x00)
    op_regs[0] |= (1 << 1);

    int timeout = 10000;
    while (op_regs[0] & (1 << 1)) {
        if (--timeout == 0) {
            term_print("[xHCI] ERROR: Controller Reset Timeout!\n");
            return;
        }
        __asm__ volatile("pause");
    }

    term_print("[xHCI] Reset successful. Modern SuperSpeed controller detected.\n");
}