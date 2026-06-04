#include "uhci.h"
#include "../../../core/io.h"
#include "../../../mem/pmm.h"
#include "../../../mem/vmm.h"
#include "../../../misc/timer.h"
#include <stddef.h>
#include <stdint.h>

extern void term_print(const char* str);

// --- Графический вывод поверх фреймбуфера во время boot ---
// (объявлено в vesa.h / eqstart.h, но драйвер USB их не инклюдит — берём extern)
extern uint32_t screen_width;
extern uint32_t screen_height;
extern void vesa_draw_string_direct(const char* s, int x, int y, uint32_t fg);
extern void draw_rect_direct(int x, int y, int w, int h, uint32_t color);

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ УСТРОЙСТВА ---
static uint32_t *virt_frame_list = NULL;
static uint8_t dev_is_low_speed = 0;
static uint8_t dev_is_connected = 0;

// --- СТРУКТУРЫ UHCI ---

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

struct usb_setup_packet {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed));

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

struct uhci_control_block {
    uhci_qh_t qh;
    uhci_td_t tds[3];
    uint8_t   setup_data[8];
    uint8_t   response_data[64];
} __attribute__((packed, aligned(16)));

static void *uhci_poll_phys_block = NULL;
static uhci_td_t *uhci_poll_virt_td = NULL;
static uint8_t *uhci_poll_virt_buf = NULL;
static uint32_t uhci_poll_phys_td = 0;
static uint32_t uhci_poll_phys_buf = 0;
static uint8_t uhci_poll_toggle = 0;

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

// --- УНИВЕРСАЛЬНЫЙ СИСТЕМНЫЙ ВЫЗОВ USB CONTROL TRANSFER ---

int uhci_control_transfer(uint32_t io_base, uint8_t dev_addr, struct usb_setup_packet *setup, void *data, uint16_t size) {
    void *phys_block = pmm_alloc();
    if (!phys_block) return -1;

    struct uhci_control_block *virt_block = (struct uhci_control_block *)VIRT(phys_block);
    uint32_t phys_addr = (uint32_t)(uintptr_t)phys_block;

    uint32_t phys_qh            = phys_addr + 0;
    uint32_t phys_td_setup      = phys_addr + 16;
    uint32_t phys_td_data       = phys_addr + 48;
    uint32_t phys_td_status     = phys_addr + 80;
    uint32_t phys_setup_data    = phys_addr + 112;
    uint32_t phys_response_data = phys_addr + 120;

    // Копируем управляющий пакет
    for (int i = 0; i < 8; i++) {
        virt_block->setup_data[i] = ((uint8_t*)setup)[i];
    }

    uint32_t base_ctrl_status = (1 << 23) | (3 << 27); // Active + 3 retries
    if (dev_is_low_speed) {
        base_ctrl_status |= (1 << 26);
    }

    // TD 0: SETUP Phase
    virt_block->tds[0].ctrl_status = base_ctrl_status;
    virt_block->tds[0].token       = (7 << 21) | (0 << 19) | (0 << 15) | (dev_addr << 8) | 0x2D;
    virt_block->tds[0].buffer_ptr  = phys_setup_data;

    // TD 1: DATA Phase (если передаются/принимаются данные)
    if (size > 0 && data != NULL) {
        virt_block->tds[0].link_ptr = phys_td_data | 4; // Линк SETUP -> DATA
        
        virt_block->tds[1].link_ptr    = phys_td_status | 4; // Линк DATA -> STATUS
        virt_block->tds[1].ctrl_status = base_ctrl_status;
        
        uint8_t pid = (setup->request_type & 0x80) ? 0x69 : 0xE1; // IN или OUT
        if (pid == 0xE1) {
            // Если пишем, копируем входные данные в буфер отправки
            for (int i = 0; i < size; i++) {
                virt_block->response_data[i] = ((uint8_t*)data)[i];
            }
        }
        
        virt_block->tds[1].token       = ((size - 1) << 21) | (1 << 19) | (0 << 15) | (dev_addr << 8) | pid;
        virt_block->tds[1].buffer_ptr  = phys_response_data;
    } else {
        virt_block->tds[0].link_ptr = phys_td_status | 4; // Линк SETUP -> STATUS напрямую
    }

    // TD 2: STATUS Phase
    virt_block->tds[2].link_ptr    = 1; // Конец списка
    virt_block->tds[2].ctrl_status = base_ctrl_status;
    
    // Статус всегда имеет направление, противоположное фазе DATA
    uint8_t status_pid = 0x69; // По умолчанию IN
    if (size > 0 && !(setup->request_type & 0x80)) {
        status_pid = 0xE1; // OUT, если до этого была фаза записи DATA OUT
    }

    virt_block->tds[2].token       = (0x7FF << 21) | (1 << 19) | (0 << 15) | (dev_addr << 8) | status_pid;
    virt_block->tds[2].buffer_ptr  = 0;

    // QH
    virt_block->qh.head    = 1;
    virt_block->qh.element = phys_td_setup;

    // Подключаем QH во фрейм-лист
    for (int i = 0; i < 1024; i++) {
        virt_frame_list[i] = phys_qh | 2;
    }

    // Ждем выполнения
    volatile uint32_t *status_word = &virt_block->tds[2].ctrl_status;
    int timeout = 5000000;
    while (*status_word & (1 << 23)) {
        if (--timeout == 0) break;
        __asm__ volatile("pause");
    }

    // Сбрасываем фрейм-лист
    for (int i = 0; i < 1024; i++) {
        virt_frame_list[i] = 1;
    }

    int success = 0;
    if (!(*status_word & (1 << 23)) && !(*status_word & 0x00070000)) {
        success = 1;
        // Если была фаза чтения DATA IN, копируем данные обратно
        if (size > 0 && data != NULL && (setup->request_type & 0x80)) {
            for (int i = 0; i < size; i++) {
                ((uint8_t*)data)[i] = virt_block->response_data[i];
            }
        }
    }

    pmm_free(phys_block);
    return success ? 0 : -1;
}

// Получение дескриптора (для обратной совместимости логов kmain)
void uhci_get_device_descriptor(uint32_t io_base) {
    struct usb_setup_packet setup;
    setup.request_type = 0x80;
    setup.request      = 0x06;
    setup.value        = 0x0100;
    setup.index        = 0;
    setup.length       = 18;

    struct usb_device_descriptor desc;
    if (uhci_control_transfer(io_base, 0, &setup, &desc, 18) == 0) {
        term_print("[UHCI] >>> USB DEVICE DETECTED! <<<\n");
        term_print("       Vendor ID:  0x");
        print_hex_16(desc.id_vendor);
        term_print("\n       Product ID: 0x");
        print_hex_16(desc.id_product);
        term_print("\n       Class:      0x");
        print_hex_16(desc.device_class);
        term_print("\n       Subclass:   0x");
        print_hex_16(desc.device_subclass);
        term_print("\n       Protocol:   0x");
        print_hex_16(desc.device_protocol);
        term_print("\n       EP0 MaxPkt: ");
        print_hex_16(desc.max_packet_size0);
        term_print(" bytes\n");
    } else {
        term_print("[UHCI] Control Transfer failed!\n");
    }
}

// --- НАСТРОЙКА И КОНФИГУРАЦИЯ МЫШИ ---

void uhci_poll_mouse_step(uint32_t io_base) {
    if (!uhci_poll_phys_block) {
        uhci_poll_phys_block = pmm_alloc();
        uhci_poll_virt_td = (uhci_td_t *)VIRT(uhci_poll_phys_block);
        uhci_poll_virt_buf = (uint8_t *)uhci_poll_virt_td + 32;
        uhci_poll_phys_td = (uint32_t)(uintptr_t)uhci_poll_phys_block;
        uhci_poll_phys_buf = uhci_poll_phys_td + 32;
    }

    uhci_poll_virt_td->link_ptr = 1; // Terminate
    
    uint32_t base_status = (1 << 23) | (3 << 27);
    if (dev_is_low_speed) base_status |= (1 << 26);
    uhci_poll_virt_td->ctrl_status = base_status;

    // Endpoint 1, Device Addr 1, PID IN (0x69)
    uhci_poll_virt_td->token = (3 << 21) | (uhci_poll_toggle << 19) | (1 << 15) | (1 << 8) | 0x69;
    uhci_poll_virt_td->buffer_ptr = uhci_poll_phys_buf;

    // Прописываем TD во фрейм-лист
    for (int i = 0; i < 1024; i++) {
        virt_frame_list[i] = uhci_poll_phys_td;
    }

    // Ждем выполнения или таймаута NAK
    int timeout = 1000;
    while ((uhci_poll_virt_td->ctrl_status & (1 << 23)) && --timeout > 0) {
        __asm__ volatile("pause");
    }

    // Снимаем со фрейм-листа
    for (int i = 0; i < 1024; i++) {
        virt_frame_list[i] = 1;
    }

    uint32_t status = uhci_poll_virt_td->ctrl_status;
    if (!(status & (1 << 23)) && !(status & 0x00070000)) {
        uint8_t buttons = uhci_poll_virt_buf[0];
        int8_t dx = (int8_t)uhci_poll_virt_buf[1];
        int8_t dy = (int8_t)uhci_poll_virt_buf[2];

        // Пишем напрямую в систему!
        usb_mouse_update(dx, dy, buttons);

        uhci_poll_toggle ^= 1; // Переключаем Data Toggle
    }
}

void uhci_configure_mouse(uint32_t io_base) {
    term_print("[UHCI] Configuring USB Device...\n");

    struct usb_setup_packet setup;
    setup.request_type = 0x00;
    setup.request      = 0x05; 
    setup.value        = 1;    // Адрес мыши = 1
    setup.index        = 0;
    setup.length       = 0;

    if (uhci_control_transfer(io_base, 0, &setup, NULL, 0) < 0) {
        term_print("[UHCI] Error: SET_ADDRESS failed!\n");
        return;
    }
    sleep(10); 

    uint8_t config_buf[64];
    setup.request_type = 0x80;
    setup.request      = 0x06; 
    setup.value        = 0x0200; 
    setup.index        = 0;
    setup.length       = 9; 

    if (uhci_control_transfer(io_base, 1, &setup, config_buf, 9) < 0) {
        term_print("[UHCI] Error: GET_DESCRIPTOR (Config Header) failed!\n");
        return;
    }

    uint16_t total_len = config_buf[2] | (config_buf[3] << 8);
    if (total_len > sizeof(config_buf)) {
        total_len = sizeof(config_buf);
    }

    setup.length = total_len;
    if (uhci_control_transfer(io_base, 1, &setup, config_buf, total_len) < 0) {
        term_print("[UHCI] Error: GET_DESCRIPTOR (Full Config) failed!\n");
        return;
    }
    term_print("[UHCI] Configuration Descriptor retrieved.\n");

    setup.request_type = 0x00;
    setup.request      = 0x09; 
    setup.value        = 1;    // Конфигурация 1
    setup.index        = 0;
    setup.length       = 0;

    if (uhci_control_transfer(io_base, 1, &setup, NULL, 0) < 0) {
        term_print("[UHCI] Error: SET_CONFIGURATION failed!\n");
        return;
    }
    sleep(10);
    term_print("[UHCI] USB Mouse is CONFIGURED!\n");

    // БЛОКИРУЮЩИЙ ЦИКЛ ОПРОСА УДАЛЕН. 
    // Теперь опрос будет происходить параллельно через планировщик в ядре!
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

    uhci_probe_ports(io_base);

    // Если устройство обнаружено, настраиваем её
    if (dev_is_connected) {
        uhci_configure_mouse(io_base);
        
        // РЕГИСТРИРУЕМ В ДИСПЕТЧЕРЕ ЯДРА, ЧТО НАШЛИ USB МЫШЬ НА UHCI
        extern volatile int g_usb_mouse_found;
        extern volatile int g_usb_mouse_controller_type;
        extern volatile uint32_t g_usb_mouse_io_base;
        g_usb_mouse_found = 1;
        g_usb_mouse_controller_type = 1; // 1 = UHCI
        g_usb_mouse_io_base = io_base;
    }
}