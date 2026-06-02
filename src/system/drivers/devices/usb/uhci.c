#include "uhci.h"
#include "../../../core/io.h"
#include "../../../mem/pmm.h"
#include "../../../mem/vmm.h"
#include "../../../misc/timer.h" // Подключаем таймер для работы sleep()

extern void term_print(const char* str); 

void uhci_probe_ports(uint32_t io_base) {
    term_print("[UHCI] Probing ports...\n");
    
    for (int port = 0; port < 2; port++) {
        uint16_t port_reg = io_base + UHCI_REG_PORTSC1 + (port * 2);
        uint16_t status = inw(port_reg);

        // Бит 0: Current Connect Status (CCS) — подключено ли устройство к порту
        if (status & (1 << 0)) {
            term_print("[UHCI] Port ");
            char port_str[2] = { '1' + port, '\0' };
            term_print(port_str);
            term_print(": Device connected! Resetting port...\n");

            // 1. Подаем сигнал RESET на порт (выставляем бит 9)
            // При этом сбрасываем биты 1 (CSC) и 3 (PEC) в маске записи (устанавливаем в 0), 
            // чтобы случайно не очистить их аппаратно раньше времени.
            outw(port_reg, (status & ~((1 << 1) | (1 << 3))) | (1 << 9));

            // 2. Ждем 50 мс согласно спецификации USB
            sleep(50);

            // 3. Снимаем сигнал RESET с порта (очищаем бит 9)
            status = inw(port_reg);
            outw(port_reg, status & ~((1 << 1) | (1 << 3) | (1 << 9)));

            // 4. Даем порту 20 мс на восстановление (Reset Recovery Time)
            sleep(20);

            // 5. Пытаемся активировать порт (выставляем бит 2: Port Enable/Disable)
            status = inw(port_reg);
            uint16_t write_val = (status & ~((1 << 1) | (1 << 3))) | (1 << 2);
            outw(port_reg, write_val);

            // Даем порту время включиться
            sleep(10);

            // 6. Читаем статус порта финально и очищаем прерывания изменений (CSC и PEC)
            status = inw(port_reg);
            
            // Записываем 1 в биты 1 и 3, чтобы очистить флаги изменений, возникшие при сбросе
            outw(port_reg, status | (1 << 1) | (1 << 3));

            // Проверяем, удалось ли перевести порт в активное состояние (бит 2 - Port Enabled)
            status = inw(port_reg);
            if (status & (1 << 2)) {
                term_print("[UHCI] Port ");
                term_print(port_str);
                term_print(" ENABLED! Speed: ");
                
                // Бит 8: Low Speed Device Attached (1 = Low Speed (мышь/клавиатура), 0 = Full Speed)
                if (status & (1 << 8)) {
                    term_print("Low Speed (1.5 Mbps)\n");
                } else {
                    term_print("Full Speed (12 Mbps)\n");
                }
            } else {
                term_print("[UHCI] Port ");
                term_print(port_str);
                term_print(" failed to enable!\n");
            }
        } else {
            term_print("[UHCI] Port ");
            char port_str[2] = { '1' + port, '\0' };
            term_print(port_str);
            term_print(": Empty\n");
        }
    }
}

void uhci_init(uint8_t bus, uint8_t slot, uint8_t func, uint32_t io_base) {
    term_print("[UHCI] Initializing Controller...\n");

    // 1. Сброс контроллера (Host Controller Reset)
    outw(io_base + UHCI_REG_USBCMD, USBCMD_HCRESET);
    
    int timeout = 10000;
    while (inw(io_base + UHCI_REG_USBCMD) & USBCMD_HCRESET) {
        if (--timeout == 0) {
            term_print("[UHCI] ERROR: Controller reset timeout!\n");
            return;
        }
    }
    term_print("[UHCI] Controller reset successful.\n");

    // 2. Выделение памяти под Frame List (Список кадров)
    void *phys_frame_list = pmm_alloc();
    if (!phys_frame_list) {
        term_print("[UHCI] ERROR: Out of physical memory for Frame List!\n");
        return;
    }

    uint32_t *virt_frame_list = (uint32_t *)VIRT(phys_frame_list);

    // Заполняем Frame List невалидными указателями (бит 0 = 1, T-бит)
    for (int i = 0; i < 1024; i++) {
        virt_frame_list[i] = 1; 
    }

    // 3. Записываем физический адрес Frame List
    outl(io_base + UHCI_REG_FRBASEADD, (uint32_t)(uintptr_t)phys_frame_list);

    // Сбрасываем номер кадра
    outw(io_base + UHCI_REG_FRNUM, 0);

    // 4. Очищаем статусы прерываний и выключаем их
    outw(io_base + UHCI_REG_USBSTS, 0xFF); 
    outw(io_base + UHCI_REG_USBINTR, 0);   

    // 5. Запуск контроллера
    uint16_t cmd = inw(io_base + UHCI_REG_USBCMD);
    cmd |= USBCMD_RS | USBCMD_CF; 
    outw(io_base + UHCI_REG_USBCMD, cmd);

    timeout = 10000;
    while (inw(io_base + UHCI_REG_USBSTS) & USBSTS_HCHALTED) {
        if (--timeout == 0) {
            term_print("[UHCI] ERROR: Controller failed to start (Halted bit set)!\n");
            return;
        }
    }

    term_print("[UHCI] Controller is RUNNING!\n");

    // 6. Опрос портов контроллера сразу после запуска
    uhci_probe_ports(io_base);
}