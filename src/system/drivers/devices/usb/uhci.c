#include "uhci.h"
#include "../../../core/io.h"
#include "../../../mem/pmm.h"
#include "../../../mem/vmm.h"
#include "../../../misc/timer.h"

extern void term_print(const char* str); 

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ УСТРОЙСТВА ---
static uint32_t *virt_frame_list = NULL;
static uint8_t dev_is_low_speed = 0;
static uint8_t dev_is_connected = 0;

// --- СТРУКТУРЫ UHCI ДЛЯ ТРАНСФЕРОВ ---

typedef struct {
    uint32_t head;
    uint32_t element;
    uint32_t reserved1;
    uint32_t reserved2;
} __attribute__((packed, aligned(16))) uhci_qh_t;

typedef struct {
    uint32_t link_ptr;
    uint32_t ctrl_status;
    uint32_t token;
    uint32_t buffer_ptr;
    uint32_t reserved[4];
} __attribute__((packed, aligned(16))) uhci_td_t;

// Структура управляющего пакета USB
struct usb_setup_packet {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed));

// Структура дескриптора устройства USB
struct usb_device_descriptor {
    uint8_t  length;
    uint8_t  descriptor_type;
    uint16_t bcd_usb;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  max_packet_size0;
    uint16_t id_vendor;
    uint16_t id_product;
    uint16_t bcd_device;
    uint8_t  i_manufacturer;
    uint8_t  i_product;
    uint8_t  i_serial_number;
    uint8_t  b_num_configurations;
} __attribute__((packed));

// Единый блок в физической странице для транзакции
struct uhci_control_block {
    uhci_qh_t qh;             // Смещение 0
    uhci_td_t tds[3];         // Смещение 16 (SETUP, DATA, STATUS)
    uint8_t   setup_data[8];  // Смещение 112
    uint8_t   response_data[64]; // Смещение 120
} __attribute__((packed, aligned(16)));

// --- ХЕЛПЕРЫ ДИАГНОСТИКИ ---

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

// --- УПРАВЛЯЮЩИЙ ТРАНСФЕР (Control Transfer) ---

void uhci_get_device_descriptor(uint32_t io_base) {
    term_print("[UHCI] Requesting Device Descriptor...\n");

    // Выделяем выровненную физическую страницу под структуры
    void *phys_block = pmm_alloc();
    if (!phys_block) {
        term_print("[UHCI] ERROR: Out of physical memory for Control Block!\n");
        return;
    }

    struct uhci_control_block *virt_block = (struct uhci_control_block *)VIRT(phys_block);
    uint32_t phys_addr = (uint32_t)(uintptr_t)phys_block;

    // Рассчитываем точные физические смещения внутри страницы
    uint32_t phys_qh            = phys_addr + 0;
    uint32_t phys_td_setup      = phys_addr + 16;
    uint32_t phys_td_data       = phys_addr + 48;
    uint32_t phys_td_status     = phys_addr + 80;
    uint32_t phys_setup_data    = phys_addr + 112;
    uint32_t phys_response_data = phys_addr + 120;

    // 1. Формируем USB SETUP-пакет GET_DESCRIPTOR (Device, index 0, length 18)
    struct usb_setup_packet *setup = (struct usb_setup_packet *)virt_block->setup_data;
    setup->request_type = 0x80; // Направление: Device-to-Host, Тип: Standard, Получатель: Device
    setup->request      = 0x06; // GET_DESCRIPTOR
    setup->value        = 0x0100; // Type = Device (01), Index = 0
    setup->index        = 0x0000;
    setup->length       = 18;   // Размер стандартного Device Descriptor

    // Базовый статус: Active (бит 23) + 3 попытки (биты 28-27)
    uint32_t base_ctrl_status = (1 << 23) | (3 << 27);
    if (dev_is_low_speed) {
        base_ctrl_status |= (1 << 26); // Если устройство низкоскоростное
    }

    // 2. Инициализируем TD 0 (SETUP Phase)
    virt_block->tds[0].link_ptr    = phys_td_data | 4; // Ссылка на DATA TD, бит 2 = Depth-First
    virt_block->tds[0].ctrl_status = base_ctrl_status;
    // PID 0x2D (SETUP), dev addr 0, endpoint 0, toggle 0 (DATA0), size 8 (7)
    virt_block->tds[0].token       = (7 << 21) | (0 << 19) | (0 << 15) | (0 << 8) | 0x2D;
    virt_block->tds[0].buffer_ptr  = phys_setup_data;

    // 3. Инициализируем TD 1 (DATA Phase — чтение дескриптора)
    virt_block->tds[1].link_ptr    = phys_td_status | 4; // Ссылка на STATUS TD, Depth-First
    virt_block->tds[1].ctrl_status = base_ctrl_status;
    // PID 0x69 (IN), dev addr 0, endpoint 0, toggle 1 (DATA1), size 18 (17)
    virt_block->tds[1].token       = (17 << 21) | (1 << 19) | (0 << 15) | (0 << 8) | 0x69;
    virt_block->tds[1].buffer_ptr  = phys_response_data;

    // 4. Инициализируем TD 2 (STATUS Phase — подтверждение отправки)
    virt_block->tds[2].link_ptr    = 1; // Terminate bit = 1 (конец цепочки)
    virt_block->tds[2].ctrl_status = base_ctrl_status;
    // PID 0xE1 (OUT), dev addr 0, endpoint 0, toggle 1 (DATA1), size 0 (0x7FF)
    virt_block->tds[2].token       = (0x7FF << 21) | (1 << 19) | (0 << 15) | (0 << 8) | 0xE1;
    virt_block->tds[2].buffer_ptr  = 0;

    // 5. Инициализируем Queue Head (QH)
    virt_block->qh.head    = 1; // Terminate горизонтальной ссылки
    virt_block->qh.element = phys_td_setup; // Вертикальная ссылка на SETUP TD

    // 6. Подключаем нашу очередь QH во все 1024 элемента расписания кадров
    for (int i = 0; i < 1024; i++) {
        virt_frame_list[i] = phys_qh | 2; // Бит 1 = 1 означает ссылку на QH
    }

    // 7. Опрашиваем статус завершающего STATUS TD (ждем, когда пропадет бит 23 / Active)
    volatile uint32_t *status_word = &virt_block->tds[2].ctrl_status;
    int timeout = 5000000;
    while (*status_word & (1 << 23)) {
        if (--timeout == 0) {
            term_print("[UHCI] Error: Control Transfer timed out!\n");
            break;
        }
        __asm__ volatile("pause");
    }

    // 8. Сразу убираем QH из списка кадров, переводя контроллер обратно в режим простоя
    for (int i = 0; i < 1024; i++) {
        virt_frame_list[i] = 1; // Terminate
    }

    // Вывод логов статуса выполнения
    uint32_t td0_status = virt_block->tds[0].ctrl_status;
    uint32_t td1_status = virt_block->tds[1].ctrl_status;
    uint32_t td2_status = virt_block->tds[2].ctrl_status;

    term_print("[UHCI] TD Statuses -> SETUP: ");
    print_hex_32(td0_status);
    term_print(" | DATA: ");
    print_hex_32(td1_status);
    term_print(" | STATUS: ");
    print_hex_32(td2_status);
    term_print("\n");

    // Если транзакция завершилась успешно (нет бита Active и нет флагов ошибок)
    // Успешный статус обычно равен 0x00000000 или содержит возвращенную длину
    if (!(*status_word & (1 << 23)) && !(*status_word & 0x00070000)) {
        struct usb_device_descriptor *desc = (struct usb_device_descriptor *)virt_block->response_data;
        
        term_print("[UHCI] >>> USB DEVICE DETECTED! <<<\n");
        term_print("       Vendor ID:  0x");
        print_hex_16(desc->id_vendor);
        term_print("\n       Product ID: 0x");
        print_hex_16(desc->id_product);
        term_print("\n       Class:      0x");
        print_hex_16(desc->device_class);
        term_print("\n       Subclass:   0x");
        print_hex_16(desc->device_subclass);
        term_print("\n       Protocol:   0x");
        print_hex_16(desc->device_protocol);
        term_print("\n       EP0 MaxPkt: ");
        print_hex_16(desc->max_packet_size0);
        term_print(" bytes\n");
    } else {
        term_print("[UHCI] Control Transfer failed!\n");
    }

    // Освобождаем физическую страницу
    pmm_free(phys_block);
}

void uhci_probe_ports(uint32_t io_base) {
    term_print("[UHCI] Probing ports...\n");
    dev_is_connected = 0;
    
    for (int port = 0; port < 2; port++) {
        uint16_t port_reg = io_base + UHCI_REG_PORTSC1 + (port * 2);
        uint16_t status = inw(port_reg);

        if (status & (1 << 0)) {
            term_print("[UHCI] Port ");
            char port_str[2] = { '1' + port, '\0' };
            term_print(port_str);
            term_print(": Device connected! Resetting port...\n");

            outw(port_reg, (status & ~((1 << 1) | (1 << 3))) | (1 << 9));
            sleep(50);

            status = inw(port_reg);
            outw(port_reg, status & ~((1 << 1) | (1 << 3) | (1 << 9)));
            sleep(20);

            status = inw(port_reg);
            uint16_t write_val = (status & ~((1 << 1) | (1 << 3))) | (1 << 2);
            outw(port_reg, write_val);
            sleep(10);

            status = inw(port_reg);
            outw(port_reg, status | (1 << 1) | (1 << 3));

            status = inw(port_reg);
            if (status & (1 << 2)) {
                term_print("[UHCI] Port ");
                term_print(port_str);
                term_print(" ENABLED! Speed: ");
                
                if (status & (1 << 8)) {
                    term_print("Low Speed (1.5 Mbps)\n");
                    dev_is_low_speed = 1;
                } else {
                    term_print("Full Speed (12 Mbps)\n");
                    dev_is_low_speed = 0;
                }
                dev_is_connected = 1;
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

    outw(io_base + UHCI_REG_USBCMD, USBCMD_HCRESET);
    
    int timeout = 10000;
    while (inw(io_base + UHCI_REG_USBCMD) & USBCMD_HCRESET) {
        if (--timeout == 0) {
            term_print("[UHCI] ERROR: Controller reset timeout!\n");
            return;
        }
    }
    term_print("[UHCI] Controller reset successful.\n");

    void *phys_frame_list = pmm_alloc();
    if (!phys_frame_list) {
        term_print("[UHCI] ERROR: Out of physical memory for Frame List!\n");
        return;
    }

    virt_frame_list = (uint32_t *)VIRT(phys_frame_list);

    for (int i = 0; i < 1024; i++) {
        virt_frame_list[i] = 1; 
    }

    outl(io_base + UHCI_REG_FRBASEADD, (uint32_t)(uintptr_t)phys_frame_list);
    outw(io_base + UHCI_REG_FRNUM, 0);

    outw(io_base + UHCI_REG_USBSTS, 0xFF); 
    outw(io_base + UHCI_REG_USBINTR, 0);   

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

    // Опрос портов
    uhci_probe_ports(io_base);

    // Если устройство подключено, запрашиваем дескриптор
    if (dev_is_connected) {
        uhci_get_device_descriptor(io_base);
    }
}