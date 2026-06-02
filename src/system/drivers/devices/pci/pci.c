#include "pci.h"
#include "../../../core/io.h"
#include "../../../drivers/hardware/net/rtl8139.h"
#include "../../../drivers/devices/audio/ac97.h"

// Будут созданы на следующем шаге
extern void uhci_init(uint8_t bus, uint8_t slot, uint8_t func, uint32_t io_base);

extern void term_print(const char* str); 

// --- ПОРТЫ PCI ---
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

// --- СМЕЩЕНИЯ РЕГИСТРОВ PCI ---
#define PCI_REG_VENDOR_DEVICE 0x00
#define PCI_REG_COMMAND       0x04
#define PCI_REG_REVISION_CLASS 0x08
#define PCI_REG_HEADER_TYPE   0x0C
#define PCI_REG_BAR0          0x10
#define PCI_REG_BAR4          0x20

// --- ФЛАГИ COMMAND REGISTER ---
#define PCI_CMD_IO_SPACE      (1 << 0) // Разрешить доступ через IN/OUT (порты)
#define PCI_CMD_MEM_SPACE     (1 << 1) // Разрешить доступ через MMIO
#define PCI_CMD_BUS_MASTER    (1 << 2) // Разрешить устройству DMA (прямой доступ к памяти)

// =========================================================================
//                   БАЗОВЫЕ ФУНКЦИИ ЧТЕНИЯ/ЗАПИСИ
// =========================================================================

uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) |
                                  (func << 8) | (offset & 0xFC) | 0x80000000);
              
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | 
                                  (func << 8) | (offset & 0xFC) | 0x80000000);
                                  
    outl(PCI_CONFIG_ADDRESS, address);
    outw(PCI_CONFIG_DATA + (offset & 2), val);
}

// =========================================================================
//                   ПОИСК И ИНИЦИАЛИЗАЦИЯ УСТРОЙСТВ
// =========================================================================

static void pci_check_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t vendor_device = pci_read_dword(bus, slot, func, PCI_REG_VENDOR_DEVICE);
    uint16_t vendor = vendor_device & 0xFFFF;
    uint16_t device = (vendor_device >> 16) & 0xFFFF;

    if (vendor == 0xFFFF) return; 

    // Получаем класс, подкласс и Prog IF
    uint32_t class_rev = pci_read_dword(bus, slot, func, PCI_REG_REVISION_CLASS);
    uint8_t class_code = (class_rev >> 24) & 0xFF;
    uint8_t subclass   = (class_rev >> 16) & 0xFF;
    uint8_t prog_if    = (class_rev >> 8)  & 0xFF;

    // --- 1. Realtek RTL8139 (Сетевая карта) ---
    if (vendor == 0x10EC && device == 0x8139) {
        term_print("[PCI] Found Realtek RTL8139 Network Card!\n");
        uint16_t command = pci_read_dword(bus, slot, func, PCI_REG_COMMAND) & 0xFFFF;
        command |= (PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER); 
        pci_write_word(bus, slot, func, PCI_REG_COMMAND, command);
        uint32_t bar0 = pci_read_dword(bus, slot, func, PCI_REG_BAR0);
        rtl8139_init(bar0);
        return;
    }

    // --- 2. Intel AC'97 Audio ---
    if (vendor == 0x8086 && (device == 0x2415 || device == 0x2425)) {
        term_print("[PCI] Found Intel AC'97 Audio!\n");
        uint16_t command = pci_read_dword(bus, slot, func, PCI_REG_COMMAND) & 0xFFFF;
        command |= (PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER);
        pci_write_word(bus, slot, func, PCI_REG_COMMAND, command);

        uint32_t bar0 = pci_read_dword(bus, slot, func, PCI_REG_BAR0); // NAM
        uint32_t bar1 = pci_read_dword(bus, slot, func, 0x14);         // NAB
        ac97_init(bar0, bar1);
        return;
    }

    // --- 3. USB Хост-Контроллеры ---
    if (class_code == 0x0C && subclass == 0x03) {
        // Включаем Bus Master (для DMA буферов) и порты/память
        uint16_t command = pci_read_dword(bus, slot, func, PCI_REG_COMMAND) & 0xFFFF;
        command |= PCI_CMD_BUS_MASTER;

        if (prog_if == 0x00) { // UHCI использует IO-порты
            command |= PCI_CMD_IO_SPACE;
            pci_write_word(bus, slot, func, PCI_REG_COMMAND, command);

            // Базовый адрес I/O портов у UHCI хранится в BAR4 (offset 0x20)
            uint32_t bar4 = pci_read_dword(bus, slot, func, PCI_REG_BAR4);
            uint32_t io_base = bar4 & ~0x3; // убираем биты флагов
            
            term_print("[PCI] Found USB UHCI Controller (USB 1.1)!\n");
            uhci_init(bus, slot, func, io_base);

        } else if (prog_if == 0x10) { // OHCI
            command |= PCI_CMD_MEM_SPACE;
            pci_write_word(bus, slot, func, PCI_REG_COMMAND, command);
            term_print("[PCI] Found USB OHCI Controller (USB 1.1) [Not implemented yet].\n");

        } else if (prog_if == 0x20) { // EHCI
            command |= PCI_CMD_MEM_SPACE;
            pci_write_word(bus, slot, func, PCI_REG_COMMAND, command);
            term_print("[PCI] Found USB EHCI Controller (USB 2.0) [Not implemented yet].\n");

        } else if (prog_if == 0x30) { // xHCI
            command |= PCI_CMD_MEM_SPACE;
            pci_write_word(bus, slot, func, PCI_REG_COMMAND, command);
            term_print("[PCI] Found USB xHCI Controller (USB 3.0) [Not implemented yet].\n");
        }
    }
}

// Вспомогательная функция для проверки одного слота
static void pci_check_device(uint8_t bus, uint8_t slot) {
    uint32_t vendor_device = pci_read_dword(bus, slot, 0, PCI_REG_VENDOR_DEVICE);
    uint16_t vendor = vendor_device & 0xFFFF;

    // Если устройства нет в слоте на функции 0, сразу выходим
    if (vendor == 0xFFFF) return; 

    // Читаем тип заголовка, чтобы узнать, многофункциональное ли устройство
    uint32_t header_reg = pci_read_dword(bus, slot, 0, PCI_REG_HEADER_TYPE);
    uint8_t header_type = (header_reg >> 16) & 0xFF;

    if (header_type & 0x80) {
        // Устройство многофункциональное (Multi-function) — проверяем функции 0-7
        for (uint8_t func = 0; func < 8; func++) {
            pci_check_function(bus, slot, func);
        }
    } else {
        // Обычное однофункциональное устройство
        pci_check_function(bus, slot, 0);
    }
}

void pci_init() {
    term_print("[PCI] Scanning buses...\n");
    
    // Перебираем все шины (0-255) и все слоты (0-31)
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            pci_check_device(bus, slot);
        }
    }
    
    term_print("[PCI] Scan complete.\n");
}