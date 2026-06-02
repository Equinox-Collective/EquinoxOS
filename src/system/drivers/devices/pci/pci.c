#include "pci.h"
#include "../../../core/io.h"
#include "../../../drivers/hardware/net/rtl8139.h"
#include "../../../drivers/devices/audio/ac97.h"
#include "../../../mem/vmm.h" // Для vmm_map и флагов страниц

extern void term_print(const char* str); 

// Импортируем точки входа для всех USB-контроллеров
extern void uhci_init(uint8_t bus, uint8_t slot, uint8_t func, uint32_t io_base);
extern void ohci_init(uint8_t bus, uint8_t slot, uint8_t func, uintptr_t mmio_base);
extern void ehci_init(uint8_t bus, uint8_t slot, uint8_t func, uintptr_t mmio_base);
extern void xhci_init(uint8_t bus, uint8_t slot, uint8_t func, uintptr_t mmio_base);

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
#define PCI_CMD_IO_SPACE      (1 << 0)
#define PCI_CMD_MEM_SPACE     (1 << 1)
#define PCI_CMD_BUS_MASTER    (1 << 2)

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

static void print_hex_16(uint16_t val) {
    char buf[5];
    const char* hex = "0123456789ABCDEF";
    for (int i = 3; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[4] = '\0';
    term_print(buf);
}

// =========================================================================
//              УНИВЕРСАЛЬНЫЙ ТРАНСЛЯТОР MMIO БЕЗ КЭШИРОВАНИЯ
// =========================================================================

void *pci_map_mmio(uint64_t phys_addr, uint32_t size) {
    uint64_t cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    page_table_t *pml4 = (page_table_t *)VIRT(cr3_val & ~0xFFFULL);

    // Свободный виртуальный диапазон в верхней половине ядра под MMIO
    static uint64_t mmio_virt_ptr = 0xFFFFC20000000000;
    uint64_t virt_start = mmio_virt_ptr;
    
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    mmio_virt_ptr += (pages * PAGE_SIZE);

    // Мапим физику на виртуальные адреса с отключенным кэшем (PCD | PWT)
    for (uint32_t i = 0; i < pages; i++) {
        vmm_map(pml4, virt_start + (i * PAGE_SIZE), (phys_addr & ~0xFFFULL) + (i * PAGE_SIZE),
                PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
    }

    // Возвращаем виртуальный адрес с сохранением смещения внутри страницы
    return (void *)(virt_start + (phys_addr & 0xFFF));
}

// =========================================================================
//                   ПОИСК И ИНИЦИАЛИЗАЦИЯ УСТРОЙСТВ
// =========================================================================

static void pci_check_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t vendor_device = pci_read_dword(bus, slot, func, PCI_REG_VENDOR_DEVICE);
    uint16_t vendor = vendor_device & 0xFFFF;
    uint16_t device = (vendor_device >> 16) & 0xFFFF;

    if (vendor == 0xFFFF) return; 

    uint32_t class_rev = pci_read_dword(bus, slot, func, PCI_REG_REVISION_CLASS);
    uint8_t class_code = (class_rev >> 24) & 0xFF;
    uint8_t subclass   = (class_rev >> 16) & 0xFF;
    uint8_t prog_if    = (class_rev >> 8)  & 0xFF;

    term_print("[PCI Dev] BDF: ");
    print_hex_16((bus << 8) | (slot << 3) | func);
    term_print(" | Ven:Dev ");
    print_hex_16(vendor);
    term_print(":");
    print_hex_16(device);
    term_print(" | Class:Sub:Prog ");
    print_hex_16((class_code << 8) | subclass);
    term_print(":");
    print_hex_16(prog_if);
    term_print("\n");

    // --- Realtek RTL8139 ---
    if (vendor == 0x10EC && device == 0x8139) {
        term_print("[PCI] Initializing RTL8139...\n");
        uint16_t command = pci_read_dword(bus, slot, func, PCI_REG_COMMAND) & 0xFFFF;
        command |= (PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER); 
        pci_write_word(bus, slot, func, PCI_REG_COMMAND, command);
        uint32_t bar0 = pci_read_dword(bus, slot, func, PCI_REG_BAR0);
        rtl8139_init(bar0);
        return;
    }

    // --- Intel AC'97 Audio ---
    if (vendor == 0x8086 && (device == 0x2415 || device == 0x2425)) {
        term_print("[PCI] Initializing AC'97 Audio...\n");
        uint16_t command = pci_read_dword(bus, slot, func, PCI_REG_COMMAND) & 0xFFFF;
        command |= (PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER);
        pci_write_word(bus, slot, func, PCI_REG_COMMAND, command);

        uint32_t bar0 = pci_read_dword(bus, slot, func, PCI_REG_BAR0);
        uint32_t bar1 = pci_read_dword(bus, slot, func, 0x14);
        ac97_init(bar0, bar1);
        return;
    }

    // --- USB Хост-Контроллеры ---
    if (class_code == 0x0C && subclass == 0x03) {
        uint16_t command = pci_read_dword(bus, slot, func, PCI_REG_COMMAND) & 0xFFFF;
        command |= PCI_CMD_BUS_MASTER;

        if (prog_if == 0x00) { // UHCI (IO-порты)
            command |= PCI_CMD_IO_SPACE;
            pci_write_word(bus, slot, func, PCI_REG_COMMAND, command);

            uint32_t bar4 = pci_read_dword(bus, slot, func, PCI_REG_BAR4);
            uint32_t io_base = bar4 & ~0x3;
            
            term_print("[PCI] Found USB UHCI Controller (USB 1.1)!\n");
            uhci_init(bus, slot, func, io_base);

        } else if (prog_if == 0x10) { // OHCI (MMIO, BAR0)
            command |= PCI_CMD_MEM_SPACE;
            pci_write_word(bus, slot, func, PCI_REG_COMMAND, command);

            uint32_t bar0 = pci_read_dword(bus, slot, func, PCI_REG_BAR0);
            void *mapped_mmio = pci_map_mmio(bar0, 4096); // мапим 4 КБ под регистры

            term_print("[PCI] Found USB OHCI Controller (USB 1.1)!\n");
            ohci_init(bus, slot, func, (uintptr_t)mapped_mmio);

        } else if (prog_if == 0x20) { // EHCI (MMIO, BAR0)
            command |= PCI_CMD_MEM_SPACE;
            pci_write_word(bus, slot, func, PCI_REG_COMMAND, command);

            uint32_t bar0 = pci_read_dword(bus, slot, func, PCI_REG_BAR0);
            void *mapped_mmio = pci_map_mmio(bar0, 4096);

            term_print("[PCI] Found USB EHCI Controller (USB 2.0)!\n");
            ehci_init(bus, slot, func, (uintptr_t)mapped_mmio);

        } else if (prog_if == 0x30) { // xHCI (MMIO, BAR0)
            command |= PCI_CMD_MEM_SPACE;
            pci_write_word(bus, slot, func, PCI_REG_COMMAND, command);

            uint32_t bar0 = pci_read_dword(bus, slot, func, PCI_REG_BAR0);
            // xHCI требует больше адресного пространства под регистры (обычно мапят 64 КБ)
            void *mapped_mmio = pci_map_mmio(bar0, 65536);

            term_print("[PCI] Found USB xHCI Controller (USB 3.0)!\n");
            xhci_init(bus, slot, func, (uintptr_t)mapped_mmio);
        }
    }
}

static void pci_check_device(uint8_t bus, uint8_t slot) {
    uint32_t vendor_device = pci_read_dword(bus, slot, 0, PCI_REG_VENDOR_DEVICE);
    uint16_t vendor = vendor_device & 0xFFFF;

    if (vendor == 0xFFFF) return; 

    uint32_t header_reg = pci_read_dword(bus, slot, 0, PCI_REG_HEADER_TYPE);
    uint8_t header_type = (header_reg >> 16) & 0xFF;

    if (header_type & 0x80) {
        for (uint8_t func = 0; func < 8; func++) {
            pci_check_function(bus, slot, func);
        }
    } else {
        pci_check_function(bus, slot, 0);
    }
}

void pci_init() {
    term_print("[PCI] Scanning buses...\n");
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            pci_check_device(bus, slot);
        }
    }
    term_print("[PCI] Scan complete.\n");
}