#include "uhci.h"
#include "../../../core/io.h"
#include "../../../mem/pmm.h"
#include "../../../mem/vmm.h"

extern void term_print(const char* str); 

void uhci_init(uint8_t bus, uint8_t slot, uint8_t func, uint32_t io_base) {
    char buf[128];
    // Форматирование строк через кастомный или стандартный вывод
    term_print("[UHCI] Initializing Controller...\n");

    // 1. Сброс контроллера (Host Controller Reset)
    outw(io_base + UHCI_REG_USBCMD, USBCMD_HCRESET);
    
    // Ждем окончания сброса (бит HCRESET должен очиститься аппаратно)
    int timeout = 10000;
    while (inw(io_base + UHCI_REG_USBCMD) & USBCMD_HCRESET) {
        if (--timeout == 0) {
            term_print("[UHCI] ERROR: Controller reset timeout!\n");
            return;
        }
    }
    term_print("[UHCI] Controller reset successful.\n");

    // 2. Выделение памяти под Frame List (Список кадров)
    // Frame List должен содержать 1024 32-битных указателя (4 КБ) и быть выровнен по границе 4 КБ.
    // pmm_alloc() возвращает ФИЗИЧЕСКИЙ адрес одной страницы (4 КБ), который идеально выровнен!
    void *phys_frame_list = pmm_alloc();
    if (!phys_frame_list) {
        term_print("[UHCI] ERROR: Out of physical memory for Frame List!\n");
        return;
    }

    // Для записи данных в ядре используем виртуальный адрес (HHDM-смещение)
    uint32_t *virt_frame_list = (uint32_t *)VIRT(phys_frame_list);

    // Заполняем Frame List "невалидными" указателями (бит 0 = T-бит (Terminate), 
    // сигнализирующий контроллеру, что очередей задач/трансферов пока нет)
    for (int i = 0; i < 1024; i++) {
        virt_frame_list[i] = 1; // Бит 0 установлен -> элемент пустой/невалидный
    }

    // 3. Записываем физический адрес Frame List в регистр FRBASEADD
    outl(io_base + UHCI_REG_FRBASEADD, (uint32_t)(uintptr_t)phys_frame_list);

    // Сбрасываем номер текущего кадра
    outw(io_base + UHCI_REG_FRNUM, 0);

    // 4. Очищаем статусы прерываний и выключаем их (пока работаем в режиме опроса/polling)
    outw(io_base + UHCI_REG_USBSTS, 0xFF); // Запись 1 в биты статуса сбрасывает их
    outw(io_base + UHCI_REG_USBINTR, 0);   // Отключаем все прерывания

    // 5. Запуск контроллера (Run)
    uint16_t cmd = inw(io_base + UHCI_REG_USBCMD);
    cmd |= USBCMD_RS | USBCMD_CF; // Запуск + флаг конфигурации
    outw(io_base + UHCI_REG_USBCMD, cmd);

    // Проверяем, что контроллер запустился (бит HALTED в статусе должен очиститься)
    timeout = 10000;
    while (inw(io_base + UHCI_REG_USBSTS) & USBSTS_HCHALTED) {
        if (--timeout == 0) {
            term_print("[UHCI] ERROR: Controller failed to start (Halted bit set)!\n");
            return;
        }
    }

    term_print("[UHCI] Controller is RUNNING!\n");
}