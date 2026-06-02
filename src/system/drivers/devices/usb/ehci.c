#include "ehci.h"

extern void term_print(const char* str);

void ehci_init(uint8_t bus, uint8_t slot, uint8_t func, uintptr_t mmio_base) {
    (void)bus; (void)slot; (void)func;
    volatile uint8_t *cap_regs = (volatile uint8_t *)mmio_base;

    term_print("[EHCI] Initializing USB 2.0 Controller...\n");

    // Первым байтом в BAR0 идет CAPLENGTH (размер регистров возможностей)
    uint8_t cap_length = cap_regs[0];
    volatile uint32_t *op_regs = (volatile uint32_t *)(mmio_base + cap_length);

    // Считываем версию интерфейса (офсет 2, 2 байта)
    uint16_t version = *(volatile uint16_t *)(mmio_base + 2);
    
    // Сбрасываем контроллер (Host Controller Reset - бит 1 в USBCMD, офсет op_regs + 0x00)
    op_regs[0] |= (1 << 1);

    int timeout = 10000;
    while (op_regs[0] & (1 << 1)) {
        if (--timeout == 0) {
            term_print("[EHCI] ERROR: Controller Reset Timeout!\n");
            return;
        }
        __asm__ volatile("pause");
    }

    term_print("[EHCI] Reset successful. Controller operational registers mapped.\n");
}