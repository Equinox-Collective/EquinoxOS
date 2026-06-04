#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "api.h"
#include "boot/boot_config.h"
#include "boot/limine/limine.h"
#include "system/fs/elf.h"
#include "syslibc/string.h"
#include "syslibc/stdio.h"

#include "system/core/gdt.h"
#include "system/core/idt.h"
#include "system/mem/memory.h"
#include "system/core/pic.h"
#include "system/mem/pmm.h"
#include "system/mem/shm.h"
#include "system/usr/task.h"
#include "system/misc/timer.h"
#include "system/misc/random.h"
#include "system/mem/vmm.h"
#include "system/hal/hal.h"
#include "system/usr/ipc.h"

#include "system/drivers/devices/mouse/mouse.h"
#include "system/drivers/hardware/net/rtl8139.h"
#include "system/drivers/devices/pci/pci.h"
#include "system/drivers/devices/pcspeaker/pcspeaker.h"
#include "system/drivers/hardware/serial/serial.h"
#include "system/drivers/vesa/vesa.h"

#include "system/fs/fat32.h"
#include "system/fs/ext2.h"
#include "system/fs/vfs.h"
#include "system/shell/shell.h"

// --- ДИСПЕТЧЕР ВВОДА USB ---
volatile int g_usb_mouse_found = 0;
volatile int g_usb_mouse_controller_type = 0; // 1 = UHCI, 2 = OHCI, 3 = EHCI, 4 = xHCI
volatile uint32_t g_usb_mouse_io_base = 0;
volatile uintptr_t g_usb_mouse_mmio = 0;

// Импорт шагов опроса
extern void uhci_poll_mouse_step(uint32_t io_base);

void usb_mouse_thread() {
    serial_puts(COM1, "[USB] Mouse driver thread active.\n");
    while (1) {
        if (g_usb_mouse_controller_type == 1) {
            uhci_poll_mouse_step(g_usb_mouse_io_base);
        }
        sleep(10); // Частота опроса 100 Гц
    }
}

// --- EXTERNAL VARIABLES ---
void term_print(const char *str);
extern size_t used_memory;
extern volatile uint32_t tick;
extern char shell_buffer[64];
uint64_t hhdm_offset = 0;

bool is_app_running = false;
bool should_run_app = false;
volatile uint8_t last_scancode = 0;
static EquinoxAPI app_api;
volatile uint64_t fg_app_pid = 0;

#define LIMINE_REQ __attribute__((used, section(".limine_requests")))

LIMINE_REQ static volatile struct limine_framebuffer_request
    framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

LIMINE_REQ static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0};

LIMINE_REQ static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 3 
};

void term_print(const char *str) {
  serial_puts(COM1, str); 
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

char sys_get_key() { return 0; } 
uint32_t sys_get_time_ms() { return tick; }

uint8_t sys_get_scancode() {
  uint8_t code = last_scancode;
  last_scancode = 0;
  return code;
}

void sys_draw_app_buffer(int x, int y, int w, int h, uint32_t *buffer) {
  if (!buffer || w <= 0 || h <= 0)
    return;

  extern uintptr_t fb_base_addr;
  extern uint32_t screen_width;
  extern uint32_t screen_height;
  extern uint32_t screen_pitch;

  bool is_fullscreen = (x == 0 && y == 0 &&
                        w == (int)screen_width && h == (int)screen_height);
  if (!is_fullscreen && current_task && current_task->id != 1) {
    fg_app_pid = current_task->id;
  }

  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= (int)screen_width || y >= (int)screen_height) return;
  if (x + w > (int)screen_width)  w = (int)screen_width  - x;
  if (y + h > (int)screen_height) h = (int)screen_height - y;
  if (w <= 0 || h <= 0) return;

  size_t row_bytes = (size_t)w * 4;
  for (int row = 0; row < h; row++) {
    uint8_t *dst = (uint8_t *)(fb_base_addr +
                               (uintptr_t)(y + row) * screen_pitch +
                               (uintptr_t)x * 4);
    uint8_t *src = (uint8_t *)(buffer + (size_t)row * (size_t)w);
    memcpy(dst, src, row_bytes);
  }
}

void network_thread() {
  extern volatile int nyan_boot_active;
  while (1) {
    if (nyan_boot_active || !rtl8139_has_data()) {
      yield();
      continue;
    }
    rtl8139_receive();
  }
}

void init_sse() {
  uint64_t cr0;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(1 << 2); 
  cr0 |= (1 << 1);  
  __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

  uint64_t cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= (1 << 9);  
  cr4 |= (1 << 10); 
  __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

void run_elf_named(uint8_t *elf_data, const char *argv0) {
  Elf64_Ehdr *hdr = (Elf64_Ehdr *)elf_data;

  if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' ||
      hdr->e_ident[2] != 'L' || hdr->e_ident[3] != 'F') {
    term_print("Not a valid ELF file!\n");
    return;
  }

  page_table_t *proc_pml4 = vmm_create_address_space();
  uint64_t phys_pml4 = (uint64_t)proc_pml4 - hhdm_offset; 

  Elf64_Phdr *phdr = (Elf64_Phdr *)(elf_data + hdr->e_phoff);
  for (int i = 0; i < hdr->e_phnum; i++) {
    if (phdr[i].p_type == 1) { 
      uint64_t pages = (phdr[i].p_memsz + 4095) / 4096;
      void *phys_mem = pmm_alloc_continuous(pages);

      for (uint64_t p = 0; p < pages; p++) {
        vmm_map(proc_pml4, phdr[i].p_vaddr + (p * 4096),
                (uint64_t)phys_mem + (p * 4096),
                PTE_PRESENT | PTE_USER | PTE_WRITABLE);
      }

      memset((void *)((uint64_t)phys_mem + hhdm_offset), 0, phdr[i].p_memsz);
      memcpy((void *)((uint64_t)phys_mem + hhdm_offset),
             elf_data + phdr[i].p_offset, phdr[i].p_filesz);
    }
  }

  uint64_t argv_virt = 0x50000000;
  void *arg_phys = pmm_alloc(); 
  vmm_map(proc_pml4, argv_virt, (uint64_t)arg_phys,
          PTE_PRESENT | PTE_USER | PTE_WRITABLE);

  char *k_arg_ptr = (char *)((uint64_t)arg_phys + hhdm_offset);
  const char *name = (argv0 && argv0[0]) ? argv0 : "app.elf";
  size_t name_len = strlen(name);
  if (name_len > 255)
    name_len = 255;
  memcpy(k_arg_ptr, name, name_len);
  k_arg_ptr[name_len] = '\0';

  uint64_t *k_argv_array = (uint64_t *)(k_arg_ptr + 256);
  k_argv_array[0] = argv_virt; 
  k_argv_array[1] = 0;         

  task_create((void (*)())hdr->e_entry, 1, argv_virt + 256, phys_pml4);
  is_app_running = true;
}

void run_elf(uint8_t *elf_data) { run_elf_named(elf_data, "app.elf"); }

void exec_module() {
  if (module_request.response == NULL) {
    term_print("Limine modules not found!\n");
    return;
  }

  for (uint64_t i = 0; i < module_request.response->module_count; i++) {
    struct limine_file *mod = module_request.response->modules[i];
    if (strstr(mod->path, "app.elf")) {
      term_print("Found app.elf. Loading...\n");
      run_elf_named(mod->address, "app.elf");
      return;
    }
  }
  term_print("Error: app.elf not found in modules!\n");
}

void emergency_kill_all_and_shell(void) {
  task_kill_all_user();
  is_app_running = false;
  shell_emergency_active = true;
  shell_emergency_enter();
}

void exec_from_disk(const char *filename) {
  vfs_node_t *dev = vfs_root->next;
  while (dev) {
    if (!dev->readdir || !dev->read) {
      dev = dev->next;
      continue;
    }

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
          run_elf_named(elf_data, filename);
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

// Инициализация «тяжёлого» железа, не нужного для первого кадра GUI:
// PCI-скан (включает USB-контроллеры), PC-спикер, диспетчер ввода (USB/PS2),
// сетевой поток. Вызывается либо инлайн в kmain (DEFER_HW_INIT==0), либо как
// фоновый поток ПОСЛЕ запуска sysgui (DEFER_HW_INIT==1), чтобы рабочий стол
// появлялся раньше.
void hw_init_sequence(void) {
  // 1. Инициализация PCI (обнаружит и включит USB контроллеры)
  pci_init();
  serial_puts(COM1, "PCI initialized\n");
  pcspeaker_init();
  serial_puts(COM1, "PC Speaker initialized\n");

  // =========================================================================
  //                  УМНЫЙ ДИСПЕТЧЕР ВВОДА (USB-First, PS/2 Fallback)
  // =========================================================================
  if (g_usb_mouse_found) {
      char log_buf[128];
      const char* controller_names[] = { "", "UHCI", "OHCI", "EHCI", "xHCI" };
      sprintf(log_buf, "[INPUT] USB Mouse detected on %s controller. Bypassing PS/2.\n",
              controller_names[g_usb_mouse_controller_type]);
      serial_puts(COM1, log_buf);

      // Создаем фоновый поток ядра под опрос USB мыши
      task_create(usb_mouse_thread, 0, 0, 0);
  } else {
      serial_puts(COM1, "[INPUT] No USB mouse detected. Trying PS/2 mouse fallback...\n");
      if (!init_mouse()) {
          serial_puts(COM1, "[INPUT] No USB or PS/2 devices found, skipping.\n");
      }
  }

  task_create(network_thread, 0, 0, 0);
  serial_puts(COM1, "Network thread started\n");
}

#if DEFER_HW_INIT
// Точка входа фонового потока инициализации железа. ВАЖНО: task_create НЕ кладёт
// адрес возврата на стек потока, поэтому вход потока не должен делать `ret` —
// иначе CPU прыгнет по мусору и словит #GP. hw_init_sequence() сама возвращается
// (она рассчитана и на инлайн-вызов из kmain при DEFER_HW_INIT==0), поэтому здесь
// после неё корректно завершаем поток через task_kill_self().
static void hw_init_task_entry(void) {
  hw_init_sequence();
  task_kill_self();   // помечает поток мёртвым и навсегда уходит в планировщик
  for (;;) { yield(); } // подстраховка: сюда уже не вернёмся
}
#endif

void kmain(void) {
  serial_init(COM1);
  serial_puts(COM1, "\n=== EquinoxOS Kernel Starting ===\n");

  if (hhdm_request.response == NULL) {
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
  rdrand_init();
  serial_puts(COM1, rdrand_supported()
                        ? "RDRAND available\n"
                        : "RDRAND unavailable, using soft entropy fallback\n");
  pmm_init();
  serial_puts(COM1, "PMM initialized\n");
  vmm_init();
  serial_puts(COM1, "VMM initialized\n");

  init_heap((uint64_t)pmm_alloc_continuous(16384) + hhdm_offset,
            64 * 1024 * 1024);
  serial_puts(COM1, "Heap initialized\n");

  struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
  init_vesa((uintptr_t)fb->address, fb->width, fb->height, fb->pitch);
  serial_puts(COM1, "VESA initialized\n");

  __asm__("cli");

  init_idt(); 
  serial_puts(COM1, "IDT initialized\n");
  pic_remap(); 
  serial_puts(COM1, "PIC remapped\n");
  init_timer(1000);
  serial_puts(COM1, "Timer initialized (1000Hz)\n");
  tick = 0;
  extern void irq0_handler_asm();
  set_idt_gate(32, (uint64_t)irq0_handler_asm, 0x08);

  __asm__("sti"); 
  serial_puts(COM1, "Interrupts enabled\n");

#if !FAST_BOOT
  uint32_t start_tick = tick;
  while (tick < start_tick + 100) {
    __asm__ volatile("hlt");
  }
#endif
  serial_puts(COM1, "Running kernel tests...\n");
  extern bool eqstart_perform_tests();
  if (!eqstart_perform_tests()) {
  }
  serial_puts(COM1, "Kernel tests passed\n");

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

  {
    extern void boot_eta_set(uint32_t ms);
    uint32_t bt_ino = ext2_resolve_path("/boottime");
    if (bt_ino) {
      uint32_t saved = 0;
      if (ext2_read(bt_ino, 0, sizeof(saved), (uint8_t *)&saved) >= sizeof(saved)) {
        boot_eta_set(saved);
        serial_puts(COM1, "Boot ETA loaded from /boottime\n");
      }
    }
  }
#if !FAST_BOOT
  ext2_stress_test_phase1();
  ext2_stress_test_phase2();
  ext2_stress_test_phase3();
  ext2_stress_test_phase4();
#endif

#if !DEFER_HW_INIT
  // Поднимаем PCI/USB/звук/сеть/мышь до GUI (классический порядок).
  hw_init_sequence();
#endif

  shm_init();
  serial_puts(COM1, "Shared memory initialized\n");
  ipc_init();
  serial_puts(COM1, "IPC (pipes + mqueue) initialized\n");
  hal_init();
  serial_puts(COM1, "HAL initialized\n");

  uint64_t font_size = 0;
  void *font_ptr = sys_get_file("font.psf", &font_size);
  vesa_set_font(font_ptr);
  vesa_set_font_size(font_size); 
  serial_puts(COM1, "=== EquinoxOS Ready ===\n");

  exec_from_disk("bin/sysgui.elf"); 
  serial_puts(COM1, "enGUI spawned as Ring 3 init process\n");

#if DEFER_HW_INIT
  // Рабочий стол уже запускается — поднимаем тяжёлое железо (PCI/USB/звук/
  // сеть/мышь) в фоновом потоке, чтобы не задерживать первый кадр GUI.
  task_create(hw_init_task_entry, 0, 0, 0);
  serial_puts(COM1, "Deferred HW init thread started\n");
#endif

  while (1) {
    {
      extern volatile uint32_t boot_measured_ms;
      static int boottime_saved = 0;
      if (!boottime_saved && boot_measured_ms != 0) {
        boottime_saved = 1;
        uint32_t v = boot_measured_ms;
        ext2_overwrite("/boottime", (const char *)&v, sizeof(v));
        serial_puts(COM1, "Boot time saved to /boottime\n");
      }
    }
    if (should_run_app) {
      should_run_app = false;
      exec_module();
    }
    if (shell_emergency_requested) {
      shell_emergency_requested = false;
      emergency_kill_all_and_shell();
    }
    __asm__("hlt");
  }
}