#include "ohci.h"
#include "../../../mem/pmm.h"
#include "../../../mem/vmm.h"
#include "../../../misc/timer.h"

extern void term_print(const char* str);

typedef struct {
    uint32_t interrupt_table[32];
    uint16_t frame_number;
    uint16_t pad1;
    uint32_t done_head;
    uint8_t  reserved[116];
} __attribute__((packed, aligned(256))) ohci_hcca_t;

void ohci_probe_ports(uint32_t *regs) {
    term_print("[OHCI] Probing Root Hub ports...\n");

    // 1. Получаем количество портов из дескриптора HcRhDescriptorA (биты 7:0)
    uint32_t desc_a = regs[OHCI_REG_RH_DESCRIPTOR_A / 4];
    uint32_t num_ports = desc_a & 0xFF;

    // 2. Включаем питание на всех портах (пишем 1 в бит 8 - SetPortPower на каждом порту)
    for (uint32_t i = 0; i < num_ports; i++) {
        regs[(OHCI_REG_RH_PORT_STATUS1 + i * 4) / 4] = (1 << 8);
    }
    // Даем питанию стабилизироваться (PowerOnToPowerGoodTime из HcRhDescriptorA)
    sleep(100);

    // 3. Опрашиваем порты
    for (uint32_t i = 0; i < num_ports; i++) {
        uint32_t port_reg_idx = (OHCI_REG_RH_PORT_STATUS1 + i * 4) / 4;
        uint32_t status = regs[port_reg_idx];

        // Бит 0: CurrentConnectStatus (CCS) — подключено ли устройство
        if (status & (1 << 0)) {
            term_print("[OHCI] Port ");
            char port_str[2] = { '1' + i, '\0' };
            term_print(port_str);
            term_print(": Device connected! Resetting port...\n");

            // Инициируем Reset: пишем 1 в SetPortReset (бит 4)
            regs[port_reg_idx] = (1 << 4);

            // Ждем завершения сброса (пока взлетит бит 20 - PortResetStatusChange)
            int timeout = 10000;
            while (!(regs[port_reg_idx] & (1 << 20)) && --timeout > 0) {
                __asm__ volatile("pause");
            }

            // Очищаем флаг изменения сброса (пишем 1 в бит 20 — ClearPortResetStatusChange)
            regs[port_reg_idx] = (1 << 20);
            sleep(10); // Время восстановления порта после сброса (Port Recovery Time)

            // Активируем порт: пишем 1 в SetPortEnable (бит 1)
            regs[port_reg_idx] = (1 << 1);
            sleep(10);

            status = regs[port_reg_idx];
            // Проверяем, активировался ли порт (бит 1 — PortEnableStatus)
            if (status & (1 << 1)) {
                term_print("[OHCI] Port ");
                term_print(port_str);
                term_print(" ENABLED! Speed: ");

                // Бит 9: LowSpeedDeviceAttached (1 = Low Speed, 0 = Full Speed)
                if (status & (1 << 9)) {
                    term_print("Low Speed (1.5 Mbps)\n");
                } else {
                    term_print("Full Speed (12 Mbps)\n");
                }
            } else {
                term_print("[OHCI] Port failed to enable!\n");
            }
        } else {
            term_print("[OHCI] Port ");
            char port_str[2] = { '1' + i, '\0' };
            term_print(port_str);
            term_print(": Empty\n");
        }
    }
}

void ohci_init(uint8_t bus, uint8_t slot, uint8_t func, uintptr_t mmio_base) {
    (void)bus; (void)slot; (void)func;
    volatile uint32_t *regs = (volatile uint32_t *)mmio_base;

    term_print("[OHCI] Initializing MMIO Controller...\n");

    uint32_t rev = regs[OHCI_REG_REVISION / 4];
    term_print("[OHCI] Revision register value: ");
    if ((rev & 0xFF) == 0x10) {
        term_print("1.0 Compliant\n");
    } else {
        term_print("Unknown/Read failure\n");
        return;
    }

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

    sleep(1);

    void *phys_hcca = pmm_alloc();
    if (!phys_hcca) {
        term_print("[OHCI] ERROR: Out of RAM for HCCA!\n");
        return;
    }
    
    ohci_hcca_t *virt_hcca = (ohci_hcca_t *)VIRT(phys_hcca);
    for (uint32_t i = 0; i < sizeof(ohci_hcca_t) / 4; i++) {
        ((uint32_t *)virt_hcca)[i] = 0;
    }

    regs[OHCI_REG_HCCA / 4] = (uint32_t)(uintptr_t)phys_hcca;

    regs[OHCI_REG_FM_INTERVAL / 4] = 0x2EDF | (((0x2EDF - 210) * 6 / 7) << 16);
    regs[OHCI_REG_PERIODIC_START / 4] = (0x2EDF * 9) / 10;

    uint32_t control = regs[OHCI_REG_CONTROL / 4];
    control &= ~(3 << 6); 
    control |= OHCI_CTRL_USB_OPERATIONAL; 
    regs[OHCI_REG_CONTROL / 4] = control;

    term_print("[OHCI] Controller is RUNNING!\n");

    // Запускаем опрос портов Root Hub на нашем OHCI-контроллере
    ohci_probe_ports((uint32_t *)regs);
}