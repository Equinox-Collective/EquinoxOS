#include "xhci.h"
#include "../../../mem/pmm.h"
#include "../../../mem/vmm.h"
#include "../../../misc/timer.h"

extern void term_print(const char* str);

// Структура TRB (Transfer Request Block) — 16-байтная базовая единица xHCI
typedef struct {
    uint32_t parameter1;
    uint32_t parameter2;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

// Запись в таблице сегментов Event Ring (ERST Entry)
typedef struct {
    uint64_t ring_segment_phys_addr;
    uint32_t segment_size;
    uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;

static void print_hex_32(uint32_t val) {
    char buf[9];
    const char* hex = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[8] = '\0';
    term_print(buf);
}

void xhci_init(uint8_t bus, uint8_t slot, uint8_t func, uintptr_t mmio_base) {
    (void)bus; (void)slot; (void)func;
    volatile uint8_t *cap_regs = (volatile uint8_t *)mmio_base;

    term_print("[xHCI] Initializing USB 3.0 SuperSpeed Controller...\n");

    uint8_t cap_length = cap_regs[0];
    volatile uint32_t *op_regs = (volatile uint32_t *)(mmio_base + cap_length);

    // 1. Аппаратный сброс контроллера (HCRST - Host Controller Reset)
    op_regs[0] |= (1 << 1); 

    int timeout = 10000;
    while (op_regs[0] & (1 << 1)) {
        if (--timeout == 0) {
            term_print("[xHCI] ERROR: Controller Reset Timeout!\n");
            return;
        }
        __asm__ volatile("pause");
    }
    term_print("[xHCI] Reset successful.\n");

    // Читаем смещения Runtime-регистров и регистров Doorbell
    uint32_t dboff = *(volatile uint32_t *)(mmio_base + 0x14);
    uint32_t rtsoff = *(volatile uint32_t *)(mmio_base + 0x18);

    // 2. Выделяем одну страницу памяти и нарезаем её под все структуры:
    // Offset 0:    Command Ring (64 TRB * 16 байт = 1024 байта)
    // Offset 1024: Event Ring (64 TRB * 16 байт = 1024 байта)
    // Offset 2048: ERST Table (1 entry * 64 байта = 64 байта)
    // Offset 2112: DCBAA (64 entries * 8 байт = 512 байт, зануляем)
    void *phys_page = pmm_alloc();
    if (!phys_page) {
        term_print("[xHCI] ERROR: Out of physical memory for rings!\n");
        return;
    }
    void *virt_page = (void *)VIRT(phys_page);
    uint64_t phys_addr = (uint64_t)phys_page;

    xhci_trb_t *cmd_ring = (xhci_trb_t *)virt_page;
    xhci_trb_t *event_ring = (xhci_trb_t *)((uintptr_t)virt_page + 1024);
    xhci_erst_entry_t *erst = (xhci_erst_entry_t *)((uintptr_t)virt_page + 2048);

    // Полностью зануляем выделенную страницу
    for (uint32_t i = 0; i < 4096 / 4; i++) {
        ((uint32_t *)virt_page)[i] = 0;
    }

    // 3. Создаем Link TRB в конце Command Ring (индекс 63), чтобы закольцевать его
    uint64_t phys_cmd_ring = phys_addr;
    cmd_ring[63].parameter1 = (uint32_t)phys_cmd_ring;
    cmd_ring[63].parameter2 = (uint32_t)(phys_cmd_ring >> 32);
    cmd_ring[63].status     = 0;
    // Type = 6 (Link TRB), TC (Toggle Cycle, бит 1) = 1, Cycle (бит 0) = 1
    cmd_ring[63].control    = (6 << 10) | (1 << 1) | 1;

    // 4. Настраиваем дескриптор таблицы ERST
    erst->ring_segment_phys_addr = phys_addr + 1024; // Физика Event Ring
    erst->segment_size = 64;
    erst->reserved = 0;

    // 5. Очищаем регистр статуса USBSTS (пишем 1 во все биты ошибок для сброса)
    op_regs[1] = 0xFFFFFFFF;

    // 6. Регистрируем Event Ring в Runtime регистрах (двумя 32-битными порциями)
    volatile uint32_t *rts = (volatile uint32_t *)(mmio_base + rtsoff);
    volatile uint32_t *iman        = (volatile uint32_t *)((uintptr_t)rts + 0x20);
    volatile uint32_t *erstsz      = (volatile uint32_t *)((uintptr_t)rts + 0x28);
    volatile uint32_t *erstba_low  = (volatile uint32_t *)((uintptr_t)rts + 0x30);
    volatile uint32_t *erstba_high = (volatile uint32_t *)((uintptr_t)rts + 0x34);
    volatile uint32_t *erdp_low    = (volatile uint32_t *)((uintptr_t)rts + 0x38);
    volatile uint32_t *erdp_high   = (volatile uint32_t *)((uintptr_t)rts + 0x3C);

    *erstsz = 1; // 1 сегмент в ERST
    
    // Пишем адрес ERST Table
    *erstba_low  = (uint32_t)(phys_addr + 2048);
    *erstba_high = (uint32_t)((phys_addr + 2048) >> 32);

    // Пишем Dequeue Pointer и сбрасываем бит EHB (бит 3)
    *erdp_low    = (uint32_t)(phys_addr + 1024) | (1 << 3);
    *erdp_high   = (uint32_t)((phys_addr + 1024) >> 32);

    // ОБЯЗАТЕЛЬНО включаем прерывания на уровне интерраптера (IE = 1, бит 1)
    *iman = (1 << 1); 

    // 7. Регистрируем Command Ring (Operational) двумя 32-битными порциями
    volatile uint32_t *crcr_low  = (volatile uint32_t *)((uintptr_t)op_regs + 0x18);
    volatile uint32_t *crcr_high = (volatile uint32_t *)((uintptr_t)op_regs + 0x1C);
    
    *crcr_low  = (uint32_t)phys_addr | 1; // RCS (Ring Cycle State) = 1
    *crcr_high = (uint32_t)(phys_addr >> 32);

    // 8. Регистрируем DCBAAP (Device Context Base Address Array)
    volatile uint32_t *dcbaap_low  = (volatile uint32_t *)((uintptr_t)op_regs + 0x30);
    volatile uint32_t *dcbaap_high = (volatile uint32_t *)((uintptr_t)op_regs + 0x34);
    
    *dcbaap_low  = (uint32_t)(phys_addr + 2112);
    *dcbaap_high = (uint32_t)(phys_addr >> 32); // DCBAAP физический адрес

    // 9. Настраиваем количество доступных слотов устройств (MaxSlots)
    uint32_t hcsparams1 = *(volatile uint32_t *)(mmio_base + 0x04);
    uint32_t max_slots = hcsparams1 >> 24;
    op_regs[0x38 / 4] = max_slots; // Записываем в регистр CONFIG

    // 10. Запускаем xHCI контроллер (Run/Stop бит = 1 в USBCMD)
    op_regs[0] |= (1 << 0);

    // ИСПРАВЛЕНО: Ждем запуска (пока HCHalted (бит 0!) в USBSTS очистится)
    timeout = 100;
    while (op_regs[1] & (1 << 0)) {
        if (--timeout == 0) {
            term_print("[xHCI] ERROR: Controller failed to start (HCHalted bit remains set)!\n");
            return;
        }
        sleep(1); // Даем контроллеру переключить состояние
    }
    term_print("[xHCI] Controller is RUNNING!\n");

    // --- ОТЛАДОЧНЫЙ ВЫВОД РЕГИСТРОВ ПЕРЕД ОТПРАВКОЙ ---
    term_print("[xHCI Debug] Regs before Doorbell:\n");
    term_print("  USBCMD:  "); print_hex_32(op_regs[0]);
    term_print(" | USBSTS: "); print_hex_32(op_regs[1]);
    term_print(" | CRCR_L: "); print_hex_32(*crcr_low);
    term_print("\n");

    // 11. ТЕСТ: Отправляем команду NO-OP (Type 23) в первый элемент Command Ring
    term_print("[xHCI] Submitting NO-OP command to Command Ring...\n");
    cmd_ring[0].parameter1 = 0;
    cmd_ring[0].parameter2 = 0;
    cmd_ring[0].status     = 0;
    cmd_ring[0].control    = (23 << 10) | 1; // Type = 23 (NO_OP), Cycle Bit = 1

    // 12. ЗВОНИМ В АППАРАТНЫЙ ДВЕРНОЙ ЗВОНОК (Doorbell 0)
    volatile uint32_t *db = (volatile uint32_t *)(mmio_base + dboff);
    db[0] = 0; // Направляем звонок на Command Ring контроллера

    // --- ОТЛАДОЧНЫЙ ВЫВОД РЕГИСТРОВ ПОСЛЕ ОТПРАВКИ ---
    term_print("[xHCI Debug] Regs after Doorbell:\n");
    term_print("  USBSTS:  "); print_hex_32(op_regs[1]);
    term_print("\n");

    // 13. ЖДЕМ СОБЫТИЯ В EVENT RING (опрашиваем память)
    term_print("[xHCI] Waiting for Command Completion Event...\n");
    volatile xhci_trb_t *event = &event_ring[0];
    
    timeout = 5000000;
    // Контроллер должен выставить Cycle Bit в 1 при записи события
    while (!(event->control & 1)) {
        if (--timeout == 0) {
            term_print("[xHCI] ERROR: Command Completion Timeout!\n");
            break;
        }
        __asm__ volatile("pause");
    }

    if (event->control & 1) {
        uint32_t ev_type = (event->control >> 10) & 0x3F; // Биты 15-10: тип события
        uint32_t ev_code = (event->status >> 24) & 0xFF;  // Биты 31-24: код завершения

        term_print("[xHCI] >>> SUCCESS! Received Event TRB <<<\n");
        term_print("       Event Type:      ");
        if (ev_type == 33) {
            term_print("Command Completion (33)\n");
        } else {
            term_print("Unknown (");
            print_hex_32(ev_type);
            term_print(")\n");
        }
        
        term_print("       Completion Code: ");
        if (ev_code == 1) {
            term_print("Success (1)\n");
        } else {
            term_print("Error (");
            print_hex_32(ev_code);
            term_print(")\n");
        }
    }

    // Освобождаем физическую страницу
    pmm_free(phys_page);
}