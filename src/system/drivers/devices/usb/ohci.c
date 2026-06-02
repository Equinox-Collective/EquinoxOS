#include "ohci.h"
#include "../../../mem/pmm.h"
#include "../../../mem/vmm.h"
#include "../../../misc/timer.h"

extern void term_print(const char* str);

// Структура HCCA (Host Controller Communications Area) — 256 байт, выровнена по 256 байт
typedef struct {
    uint32_t interrupt_table[32];
    uint16_t frame_number;
    uint16_t pad1;
    uint32_t done_head;
    uint8_t  reserved[116];
} __attribute__((packed, aligned(256))) ohci_hcca_t;

void ohci_init(uint8_t bus, uint8_t slot, uint8_t func, uintptr_t mmio_base) {
    (void)bus; (void)slot; (void)func;
    volatile uint32_t *regs = (volatile uint32_t *)mmio_base;

    term_print("[OHCI] Initializing MMIO Controller...\n");

    // 1. Читаем ревизию
    uint32_t rev = regs[OHCI_REG_REVISION / 4];
    term_print("[OHCI] Revision register value: ");
    // Выведем ревизию для проверки связи
    char rev_buf[32];
    // Простейший способ убедиться, что MMIO доступен
    if ((rev & 0xFF) == 0x10) {
        term_print("1.0 Compliant\n");
    } else {
        term_print("Unknown/Read failure\n");
        return;
    }

    // 2. Инициируем программный сброс контроллера (HostControllerReset)
    regs[OHCI_REG_COMMAND_STATUS / 4] = OHCI_CMD_HCR;

    int timeout = 10000;
    while (regs[OHCI_REG_COMMAND_STATUS / 4] & OHCI_CMD_HCR) {
        if (--timeout == 0) {
            term_print("[OHCI] ERROR: Controller Reset Timeout!\n");
            return;
        }
        __asm__ volatile("pause");
    }
    term_print("[OHCI] Reset successful.\n");

    // Ждем 1 мс по спецификации
    sleep(1);

    // 3. Выделяем физическую страницу под HCCA
    void *phys_hcca = pmm_alloc();
    if (!phys_hcca) {
        term_print("[OHCI] ERROR: Out of RAM for HCCA!\n");
        return;
    }
    
    // Обнуляем HCCA
    ohci_hcca_t *virt_hcca = (ohci_hcca_t *)VIRT(phys_hcca);
    for (uint32_t i = 0; i < sizeof(ohci_hcca_t) / 4; i++) {
        ((uint32_t *)virt_hcca)[i] = 0;
    }

    // Записываем физический адрес HCCA в регистр HcHCCA
    regs[OHCI_REG_HCCA / 4] = (uint32_t)(uintptr_t)phys_hcca;

    // 4. Настраиваем интервалы кадров (Frame Interval)
    // По спецификации стандартное значение FrameInterval = 0x2EDF (11,999 тиков на кадр в 1 мс)
    regs[OHCI_REG_FM_INTERVAL / 4] = 0x2EDF | (((0x2EDF - 210) * 6 / 7) << 16);
    regs[OHCI_REG_PERIODIC_START / 4] = (0x2EDF * 9) / 10; // 90% времени отдаем под периодические трансферы

    // 5. Переводим контроллер в режим OPERATIONAL (запуск обработки шины)
    uint32_t control = regs[OHCI_REG_CONTROL / 4];
    control &= ~(3 << 6); // Очищаем биты состояния
    control |= OHCI_CTRL_USB_OPERATIONAL; // Выставляем Operational State
    regs[OHCI_REG_CONTROL / 4] = control;

    term_print("[OHCI] Controller is RUNNING!\n");
}