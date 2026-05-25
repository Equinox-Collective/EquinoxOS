#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --- ЗАГОЛОВКИ СИСТЕМЫ И БИБЛИОТЕК ---
#include "api.h"
#include "boot/limine/limine.h"
#include "system/fs/elf.h"
#include "gui/gui.h"
#include "syslibc/string.h"

// --- СИСТЕМНЫЕ ПОДСИСТЕМЫ ---
#include "system/core/gdt.h"
#include "system/core/idt.h"
#include "system/mem/memory.h"
#include "system/core/pic.h"
#include "system/mem/pmm.h"
#include "system/mem/shm.h"
#include "system/usr/task.h"
#include "system/misc/timer.h"
#include "system/mem/vmm.h"
#include "system/hal/hal.h"
#include "system/usr/ipc.h"

// --- ДРАЙВЕРЫ ---
#include "system/drivers/devices/mouse/mouse.h"
#include "system/drivers/hardware/net/rtl8139.h"
#include "system/drivers/devices/pci/pci.h"
#include "system/drivers/devices/pcspeaker/pcspeaker.h"
#include "system/drivers/hardware/serial/serial.h"
#include "system/drivers/vesa/vesa.h"

// --- ФАЙЛОВАЯ СИСТЕМА И ОБОЛОЧКА ---
#include "system/fs/fat32.h"
#include "system/fs/ext2.h"
#include "system/fs/vfs.h"
#include "gui/terminal.h"
#include "system/shell/shell.h"
#include "gui/gui_apps.h"

// --- EXTERNAL VARIABLES ---
void term_print(const char *str);
extern size_t used_memory;
extern volatile uint32_t tick;
extern char shell_buffer[64];
uint64_t hhdm_offset = 0;

// --- GLOBAL STATE ---
bool is_app_running = false;
bool should_run_app = false;
volatile uint8_t last_scancode = 0;
static EquinoxAPI app_api;

// --- LIMINE REQUESTS ---
#define LIMINE_REQ __attribute__((used, section(".limine_requests")))

LIMINE_REQ static volatile struct limine_framebuffer_request
    framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

LIMINE_REQ static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0};

LIMINE_REQ static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 3 
};

// =========================================================================
//                              SYSTEM API
// =========================================================================

void term_print(const char *str) {
  serial_puts(COM1, str); // Оставляем для логов в QEMU
  terminal_print(str);    // Вызываем новый крутой терминал
}

void *sys_get_file(const char *name, uint64_t *size) {
  if (module_request.response == NULL)
    return NULL;
  for (uint64_t i = 0; i < module_request.response->module_count; i++) {
    struct limine_file *module = module_request.response->modules[i];
    if (strstr(module->path, name) != NULL) {
      *size = module->size;
      return module->address;
    }
  }
  return NULL;
}

char sys_get_key() { return 0; } // Заглушка
uint32_t sys_get_time_ms() { return tick * 10; }

uint8_t sys_get_scancode() {
  uint8_t code = last_scancode;
  last_scancode = 0;
  return code;
}

// Вызывается приложением для отрисовки
void sys_draw_app_buffer(int x, int y, int w, int h, uint32_t *buffer) {
  if (!app_win)
    return;

  // Автоматически подстраиваем размер окна под приложение!
  if (app_win->w != w || app_win->h != h) {
    window_resize(app_win, w, h);
  }

  if (!app_win->active) {
    app_win->active = true;
    window_bring_to_front(app_win);
    focused_window = app_win;
  }

  // Копируем кадр целиком (теперь размеры точно совпадают)
  memcpy(app_win->buffer, buffer, w * h * 4);
}
// =========================================================================
//                              MAIN LOOPS & INIT
// =========================================================================

void network_thread() {
  while (1) {
    if (!rtl8139_has_data()) { // Если есть такая проверка
      yield();
      continue;
    }
    rtl8139_receive();
  }
}
void init_sse() {
  // 1. Включаем SSE (стандартно)
  uint64_t cr0;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(1 << 2); // Сбросить EM (Emulation)
  cr0 |= (1 << 1);  // Установить MP (Monitor Coprocessor)
  __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

  // 2. Включаем поддержку расширений в CR4
  uint64_t cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= (1 << 9);  // OSFXSR (SSE support)
  cr4 |= (1 << 10); // OSXMMEXCPT (SSE exceptions)

  // !!! УДАЛИ ИЛИ ЗАКОММЕНТИРУЙ СТРОКУ НИЖЕ !!!
  // cr4 |= (1ULL << 21); // SMAP - УБИЙЦА СТАРЫХ ПРОЦЕССОРОВ

  __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

void run_elf(uint8_t *elf_data) {
  Elf64_Ehdr *hdr = (Elf64_Ehdr *)elf_data;

  if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' ||
      hdr->e_ident[2] != 'L' || hdr->e_ident[3] != 'F') {
    term_print("Not a valid ELF file!\n");
    return;
  }

  term_print("VMM: Creating address space for Ring 3...\n");

  // 1. Создаем новые таблицы страниц для процесса
  page_table_t *proc_pml4 = vmm_create_address_space();
  uint64_t phys_pml4 =
      (uint64_t)proc_pml4 - hhdm_offset; // Переводим в физический адрес

  // 2. Загружаем сегменты ELF в НОВОЕ пространство
  Elf64_Phdr *phdr = (Elf64_Phdr *)(elf_data + hdr->e_phoff);
  for (int i = 0; i < hdr->e_phnum; i++) {
    if (phdr[i].p_type == 1) { // PT_LOAD
      // Выделяем физические страницы
      uint64_t pages = (phdr[i].p_memsz + 4095) / 4096;
      void *phys_mem = pmm_alloc_continuous(pages);

      // Мапим их в виртуальное пространство процесса с флагом USER
      for (uint64_t p = 0; p < pages; p++) {
        vmm_map(proc_pml4, phdr[i].p_vaddr + (p * 4096),
                (uint64_t)phys_mem + (p * 4096),
                PTE_PRESENT | PTE_USER | PTE_WRITABLE);
      }

      // Копируем данные из ELF в эти физические страницы через HHDM
      memset((void *)((uint64_t)phys_mem + hhdm_offset), 0, phdr[i].p_memsz);
      memcpy((void *)((uint64_t)phys_mem + hhdm_offset),
             elf_data + phdr[i].p_offset, phdr[i].p_filesz);
    }
  }

  term_print("Task: Jumping to Ring 3...\n");

  // 3. Создаем задачу с указанием её CR3 (phys_pml4)
  // Передаем адрес API (arg1) - только учти, что адрес app_api должен быть
  // доступен юзеру! Пока передадим просто 0, чтобы проверить сам прыжок.
  uint64_t argv_virt = 0x50000000;
  void *arg_phys = pmm_alloc(); // Выделяем страницу под аргументы
  vmm_map(proc_pml4, argv_virt, (uint64_t)arg_phys,
          PTE_PRESENT | PTE_USER | PTE_WRITABLE);

  char *k_arg_ptr = (char *)((uint64_t)arg_phys + hhdm_offset);
  strcpy(k_arg_ptr, "doom.elf"); // argv[0]

  uint64_t *k_argv_array = (uint64_t *)(k_arg_ptr + 256);
  k_argv_array[0] = argv_virt; // Указатель на строку "doom.elf"
  k_argv_array[1] = 0;         // Конец массива

  // Передаем argc=1 и адрес массива argv
  task_create((void (*)())hdr->e_entry, 1, argv_virt + 256, phys_pml4);

  is_app_running = true;
}

void exec_module() {
  if (module_request.response == NULL) {
    term_print("Limine modules not found!\n");
    return;
  }

  for (uint64_t i = 0; i < module_request.response->module_count; i++) {
    struct limine_file *mod = module_request.response->modules[i];

    // УБРАЛИ \n ИЗ ПОИСКА!
    if (strstr(mod->path, "app.elf")) {
      term_print("Found app.elf. Loading...\n");
      run_elf(mod->address);
      return;
    }
  }
  term_print("Error: app.elf not found in modules!\n");
}

// Загрузка и запуск ELF-файла через VFS (теперь работает и с EXT2, и с FAT32!)
void exec_from_disk(const char *filename) {
  vfs_node_t *dev = vfs_root->next;
  while (dev) {
    if (!dev->readdir || !dev->read) {
      dev = dev->next;
      continue;
    }

    // Search for file in this device
    for (int i = 0; i < 64; i++) {
      vfs_dirent_t *de = dev->readdir(dev, i);
      if (!de)
        break;

      if (strcmp(de->name, filename) == 0) {
        term_print("EXEC: Found ");
        term_print(filename);
        term_print(" on ");
        term_print(dev->name);
        term_print("\n");

        uint8_t *elf_data = kmalloc(de->size);
        vfs_node_t file_node;
        memset(&file_node, 0, sizeof(vfs_node_t));
        file_node.inode = de->inode;
        strcpy(file_node.name, de->name);

        if (dev->read(&file_node, 0, de->size, elf_data) > 0) {
          run_elf(elf_data);
          kfree(elf_data);
          return;
        }
        kfree(elf_data);
      }
    }
    dev = dev->next;
  }
  term_print("EXEC: File not found on any VFS device!\n");
}

void kmain(void) {
  // Initialize serial port first for early debugging
  serial_init(COM1);
  serial_puts(COM1, "\n=== EquinoxOS Kernel Starting ===\n");

  if (hhdm_request.response == NULL) {
    // Если Limine не ответил, мы не можем работать
    serial_puts(COM1, "ERROR: Limine HHDM not available!\n");
    draw_rect_direct(0, 0, 100, 100, 0xFF0000);
    while (1)
      __asm__("cli; hlt");
  }
  hhdm_offset = hhdm_request.response->offset;
  serial_puts(COM1, "HHDM offset initialized\n");

  init_gdt();
  serial_puts(COM1, "GDT initialized\n");
  init_sse();
  serial_puts(COM1, "SSE initialized\n");
  pmm_init();
  serial_puts(COM1, "PMM initialized\n");
  vmm_init();
  serial_puts(COM1, "VMM initialized\n");

  // Инициализация кучи
  init_heap((uint64_t)pmm_alloc_continuous(16384) + hhdm_offset,
            64 * 1024 * 1024);
  serial_puts(COM1, "Heap initialized\n");

  // 2. Видео (чтобы видеть лог Цербера)
  struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
  init_vesa((uintptr_t)fb->address, fb->width, fb->height, fb->pitch);
  serial_puts(COM1, "VESA initialized\n");

  // 3. ПРЕРЫВАНИЯ (КРИТИЧЕСКИЙ ПОРЯДОК)
  __asm__("cli");

  init_idt(); // Загружает базовую таблицу
  serial_puts(COM1, "IDT initialized\n");
  pic_remap(); // Перенаправляет PIC на 0x20+
  serial_puts(COM1, "PIC remapped\n");
  init_timer(50); // Настраивает PIT на 50Гц (компенсирует двоение частоты в QEMU)
  serial_puts(COM1, "Timer initialized (100Hz)\n");
  tick = 0;
  // !!! ВАЖНО: Ставим АСЕМБЛЕРНЫЙ обработчик СРАЗУ, до включения прерываний !!!
  extern void irq0_handler_asm();
  set_idt_gate(32, (uint64_t)irq0_handler_asm, 0x08);

  __asm__("sti"); // Включаем прерывания
  serial_puts(COM1, "Interrupts enabled\n");

  // Даем таймеру "прокашляться" (ждем 10 тиков = 100мс)
  uint32_t start_tick = tick;
  while (tick < start_tick + 10) {
    __asm__ volatile("hlt");
  }
   shm_init();
  // 4. ЗАПУСКАЕМ ТЕСТЫ (Теперь Цербер увидит тикающий таймер)
  serial_puts(COM1, "Running kernel tests...\n");
  extern bool eqstart_perform_tests();
  if (!eqstart_perform_tests()) {
    // Если тесты не прошли, Цербер сам повесит систему внутри.
  }
  serial_puts(COM1, "Kernel tests passed\n");

  // 5. Если дошли сюда — всё зашибись, запускаем остальное
  task_init();
  serial_puts(COM1, "Task system initialized\n");
  vfs_init();
  serial_puts(COM1, "VFS initialized\n");
  fat32_init();
  serial_puts(COM1, "FAT32 initialized\n");
  ext2_init();
  vfs_register_device(ext2_get_root_node());
  vfs_register_device(fat32_get_root_node());
  serial_puts(COM1, "EXT2 initialized\n");
  ext2_stress_test_phase1();
  ext2_stress_test_phase2();
  ext2_stress_test_phase3();
  ext2_stress_test_phase4();
  pci_init();
  serial_puts(COM1, "PCI initialized\n");
  pcspeaker_init();
  serial_puts(COM1, "PC Speaker initialized\n");
  init_mouse();
  serial_puts(COM1, "Mouse initialized\n");
  // gui_init();
  // serial_puts(COM1, "GUI initialized\n");
  shm_init();
  serial_puts(COM1, "Shared memory initialized\n");
  ipc_init();
  serial_puts(COM1, "IPC (pipes + mqueue) initialized\n");
  hal_init();
  serial_puts(COM1, "HAL initialized\n");
  // shell_init();
  // serial_puts(COM1, "Shell initialized\n");

  // Запускаем сетевой поток в фоновом режиме
  task_create(network_thread, 0, 0, 0);
  serial_puts(COM1, "Network thread started\n");
  uint64_t font_size = 0;
  void *font_ptr = sys_get_file("font.psf", &font_size);
  vesa_set_font(font_ptr);
  serial_puts(COM1, "=== EquinoxOS Ready ===\n");
  exec_from_disk("bin/sysgui.elf"); // Загружаем ELF с диска и отдаем планировщику
  serial_puts(COM1, "enGUI spawned as Ring 3 init process\n");
  while (1) {
    // update_gui();
    if (should_run_app) {
      should_run_app = false;
      exec_module();
    }
    __asm__("hlt");
  }
}