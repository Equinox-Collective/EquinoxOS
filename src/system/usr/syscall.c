#include "../drivers/devices/audio/ac97.h"
#include "../drivers/vesa/vesa.h"
#include "../fs/vfs.h"
#include "../../gui/gui.h"
#include "../drivers/devices/keyboard/keyboard.h"
#include "../drivers/hardware/net/dns.h"
#include "../drivers/hardware/net/net.h"
#include "../../syslibc/string.h"
#include "../../syslibc/stdio.h"
#include "../drivers/hardware/net/tcp.h"
#include "../drivers/hardware/net/socket.h"
#include "../mem/memory.h"
#include "../mem/pmm.h"
#include "../mem/shm.h"
#include "../usr/task.h"
#include "../mem/vmm.h"
#include "../shell/shellsyntx.h"
#include "../core/cpu.h"
#include "../misc/random.h"
#include "ipc.h"
#include <stdint.h>

extern volatile uint32_t tick;
extern void sys_draw_app_buffer(int x, int y, int w, int h, uint32_t *buffer);
extern uint8_t keyboard_pop();
extern void term_print(const char *str);

typedef struct {
  uint64_t rax; // syscall_number
  uint64_t r9;
  uint64_t r8;
  uint64_t rbx;
  uint64_t rcx;
  uint64_t rdx;
  uint64_t rsi;
  uint64_t rdi;
  uint64_t rbp;
  uint64_t rip, cs, rflags, rsp, ss;
} syscall_regs_t;

extern int mouse_x, mouse_y;
extern bool mouse_left_button;

uint64_t copy_to_user(void *kernel_buf, uint64_t size) {
  if (!kernel_buf || size == 0)
    return 0;

  uint64_t pages = (size + 4095) / 4096;
  static uint64_t user_dynamic_ptr = 0x60000000;
  uint64_t target_virt = user_dynamic_ptr;
  user_dynamic_ptr += (pages * 4096);

  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  page_table_t *pml4 = (page_table_t *)VIRT(cr3);

  for (uint64_t i = 0; i < pages; i++) {
    // Берем ЛЮБУЮ свободную страницу, не обязательно подряд!
    void *phys = pmm_alloc();
    if (!phys)
      return 0;

    vmm_map(pml4, target_virt + (i * 4096), (uint64_t)phys,
            PTE_PRESENT | PTE_USER | PTE_WRITABLE);

    // Копируем по кусочкам
    uint64_t to_copy = (size > 4096) ? 4096 : size;
    stac();
    memcpy((void *)(target_virt + (i * 4096)),
           (uint8_t *)kernel_buf + (i * 4096), to_copy);
    clac();
    size -= to_copy;
  }

  return target_virt;
}

void syscall_handler(syscall_regs_t *regs) {
  uint64_t num = regs->rax;

  switch (num) {
  case 1: // SYS_PRINT
    stac();
    term_print((const char *)regs->rdi);
    clac();
    break;
  case 2: { // SYS_READ_FILE (Now VFS-agnostic)
    const char *filename = (const char *)regs->rdi;
    uint32_t *out_size_ptr = (uint32_t *)regs->rsi;

    uint32_t size = 0;
    uint8_t *file_data = vfs_read_file(filename, &size);

    if (!file_data) {
      regs->rax = 0;
      break;
    }

    stac();
    if (out_size_ptr)
      *out_size_ptr = size;
    clac();

    uint32_t pages_needed = (size + 4095) / 4096;
    static uint64_t next_file_vaddr = 0xA0000000;
    uint64_t target_virt = next_file_vaddr;
    next_file_vaddr += (pages_needed * 4096);
    if (next_file_vaddr > 0xB0000000)
      next_file_vaddr = 0xA0000000;

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    page_table_t *pml4 = (page_table_t *)VIRT(cr3);

    // Map and copy
    for (uint32_t i = 0; i < pages_needed; i++) {
      uint64_t v = target_virt + (i * 4096);
      void *p = pmm_alloc();
      memset((void *)VIRT(p), 0, 4096);
      vmm_map(pml4, v, (uintptr_t)p, PTE_PRESENT | PTE_USER | PTE_WRITABLE);

      uint32_t to_copy =
          (size - (i * 4096) > 4096) ? 4096 : (size - (i * 4096));
      stac();
      memcpy((void *)v, file_data + (i * 4096), to_copy);
      clac();
    }

    kfree(file_data);
    regs->rax = target_virt;
    break;
  }
  case 3: { // SYS_WRITE_FILE (Теперь через VFS!)
    const char *filename = (const char *)regs->rdi;
    const uint8_t *data = (const uint8_t *)regs->rsi;
    uint32_t size = (uint32_t)regs->rdx;

    // Ищем устройство с поддержкой записи (первое попавшееся, обычно EXT2 или
    // FAT32)
    vfs_node_t *dev = vfs_root->next;
    while (dev) {
      if (dev->write) {
        vfs_node_t file_node;
        memset(&file_node, 0, sizeof(vfs_node_t));
        // Безопасное копирование имени из юзерспейса. Раньше использовался
        // strcpy, который при подаче длинной строки переполнял
        // file_node.name (фикс. 128 байт) → порча стека ядра.
        stac();
        size_t i = 0;
        for (; i < sizeof(file_node.name) - 1 && filename[i] != '\0'; i++) {
          file_node.name[i] = filename[i];
        }
        file_node.name[i] = '\0';
        clac();
        dev->write(&file_node, 0, size, (uint8_t *)data);
        regs->rax = size;
        break;
      }
      dev = dev->next;
    }
    break;
  }
  case 4: { // SYS_READ_DIR
    int idx = (int)regs->rdi;

    // Временная структура для безопасной записи в память пользователя
    struct {
      char name[128];
      uint32_t size;
      char dev[32];
    } *out = (void *)regs->rsi;

    int current_idx = 0;
    vfs_node_t *dev = vfs_root->next;
    bool found = false;

    while (dev) {
      if (dev->readdir) {
        for (int i = 0; i < 32; i++) {
          vfs_dirent_t *de = dev->readdir(dev, i);
          if (!de)
            break;

          if (current_idx == idx) {
            stac(); // Разрешаем доступ к памяти Ring 3
            strcpy(out->name, de->name);
            out->size = de->size;
            strcpy(out->dev, dev->name);
            clac(); // Закрываем доступ
            found = true;
            break;
          }
          current_idx++;
        }
      }
      if (found)
        break;
      dev = dev->next;
    }

    regs->rax = found ? 1 : 0; // Возвращаем 1, если файл найден, иначе 0
    break;
  }
  case 5: // SYS_DRAW_BUFFER
    stac();
    sys_draw_app_buffer(regs->rdi, regs->rsi, regs->rdx, regs->rcx,
                        (uint32_t *)regs->r8);
    clac();
    break;
  case 6: // SYS_GET_TIME
    // Возвращаем время в миллисекундах. PIT тикает на 1 кГц, поэтому tick
    // напрямую измеряется в мс с момента загрузки.
    regs->rax = tick;
    break;
  case 7: { // SYS_GET_MOUSE_FULL
    extern int mouse_x, mouse_y;
    extern bool mouse_left_button, mouse_right_button;
    extern volatile uint64_t fg_app_pid;
    // Если активно foreground-приложение (последний кто вызвал
    // SYS_DRAW_BUFFER), мышь видит только оно. Иначе — все подряд.
    if (fg_app_pid != 0 && current_task && current_task->id != fg_app_pid) {
      regs->rax = 0;
      regs->rbx = 0;
      regs->rcx = 0;
    } else {
      regs->rax = mouse_x;
      regs->rbx = mouse_y;
      regs->rcx = (mouse_left_button ? 1 : 0) | (mouse_right_button ? 2 : 0);
    }
    break;
  }
  case 9: // SYS_GET_SCANCODE
  {
    extern volatile uint64_t fg_app_pid;
    // Раньше тут была мёртвая ветка с `app_win`/`focused_window` (kernel-
    // композитор отключён). Без маршрутизации sysgui и приложение
    // одновременно дёргали `keyboard_pop()`, и нажатия рандомно
    // "съедал" тот, кто поллит чаще — обычно sysgui (16 ms цикл).
    // Теперь: пока есть foreground-app, sysgui получает 0; ввод идёт
    // только в активный процесс.
    if (fg_app_pid != 0 && current_task && current_task->id != fg_app_pid) {
      regs->rax = 0;
    } else {
      regs->rax = keyboard_pop();
    }
    break;
  }

  case 10: // SYS_EXIT
    term_print("[SYS] Killing process and freeing RAM...\n");

    // ВАЖНО: Останавливаем звук ДО очистки памяти,
    // чтобы драйвер не пытался читать из удаленных страниц
    ac97_stop();

    // 1. Освобождаем физическую память процесса!
    // Эту функцию мы написали в прошлом шаге (в vmm.c)
    extern void vmm_destroy_address_space(uint64_t cr3_phys);
    vmm_destroy_address_space(current_task->cr3);

    // 2. Убиваем задачу
    current_task->running = false;

    extern bool is_app_running;
    is_app_running = false;

    // Если выходит foreground-app — отдаём фокус ввода обратно sysgui.
    {
      extern volatile uint64_t fg_app_pid;
      if (fg_app_pid == current_task->id) {
        fg_app_pid = 0;
      }
    }

    if (app_win)
      app_win->active = false;

    yield(); // Уходим в планировщик
    break;
  case 11: // SYS_YIELD (Уступить процессор)
    yield(); // Фактический switch context через int $32
    break;
  case 12: { // SYS_GET_FONT
    extern void *vesa_get_font();
    extern uint64_t vesa_get_font_size(void);
    void *kfont = vesa_get_font();

    uint64_t font_addr = (uint64_t)kfont;
    if (font_addr < hhdm_offset) {
      font_addr = VIRT(font_addr);
    }

    /* Раньше тут было `copy_to_user(font_addr, 4096)`. Для PSF1 файла
     * 8x16 / 256 глифов реальный размер = 4 (header) + 4096 (glyphs)
     * = 4100 байт, что перешагивает первую 4 KiB страницу. Глифы
     * 0xFE / 0xFF при отрисовке (eid_draw_text / vesa_draw_string)
     * читают байты со смещений 4084..4099 — последние 4 байта попадают
     * на 0x60001000, который не был замаплен в user CR3, что вызывало
     * #PF (ERR=0x4) и BSOD. Маппим всю длину шрифта. */
    uint64_t size = vesa_get_font_size();
    if (size == 0)
      size = 8192; /* fallback на случай, если размер не выставили */
    regs->rax = copy_to_user((void *)font_addr, size);
    break;
  }
  case 13: { // SYS_SLEEP
    uint32_t ms = regs->rdi;
    uint32_t start = tick;

    /* Спим через yield() — каждую итерацию передаем CPU другим задачам,
     * а потом планировщик вернёт нас обратно. hlt здесь не нужен —
     * yield() сам обеспечивает пробуждение через IRQ0.
     * Важно: не делаем cli после выхода — это убивало планировщик
     * до следующего сисколла. */
    while (tick < start + ms) {
      yield();
    }
    break;
  }

  case 14: {
    uint64_t len = regs->rsi;
    if (len == 0) {
      regs->rax = 0;
      break;
    }
    uint64_t pages = (len + 4095) / 4096;
    void *phys = pmm_alloc_continuous(pages);
    if (!phys) {
      regs->rax = 0;
      break;
    }
    static uint64_t mmap_ptr = 0x700000000000;
    uint64_t virt = mmap_ptr;
    mmap_ptr += (pages * 4096);
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    for (uint64_t i = 0; i < pages; i++) {
      vmm_map((page_table_t *)VIRT(cr3), virt + (i * 4096),
              (uint64_t)phys + (i * 4096), PTE_USER | PTE_WRITABLE);
    }
    regs->rax = virt;
    break;
  }
  case 15: { // SYS_BRK
    if (current_task->brk == 0)
      current_task->brk = 0x40000000;

    uint64_t requested_brk = regs->rdi;
    if (requested_brk == 0) {
      regs->rax = current_task->brk;
      break;
    }

    if (requested_brk > current_task->brk) {
      // Округляем текущий brk вниз до страницы, а новый - вверх
      uint64_t start_map = (current_task->brk + 4095) & ~4095;
      uint64_t end_map = (requested_brk + 4095) & ~4095;

      uint64_t cr3_val;
      __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
      page_table_t *pml4 = (page_table_t *)VIRT(cr3_val);

      for (uint64_t addr = start_map; addr < end_map; addr += 4096) {
        void *phys = pmm_alloc();
        if (phys) {
          vmm_map(pml4, addr, (uintptr_t)phys,
                  PTE_PRESENT | PTE_USER | PTE_WRITABLE);
          memset((void *)VIRT(phys), 0, 4096);
        }
      }
    }
    current_task->brk = requested_brk;
    regs->rax = current_task->brk;
    break;
  }
  case 16: {
    if (regs->rdi == 1 || regs->rdi == 2) {
      term_print((const char *)regs->rsi);
      regs->rax = regs->rdx;
    } else {
      regs->rax = -1;
    }
    break;
  }
  case 17: {
    regs->rax = 0;
    break;
  }
  case 18: {
    regs->rax = -1;
    break;
  }
  case 19: {
    regs->rax = 0;
    break;
  }
  case 20: { // SYS_AUDIO_PLAY
    uintptr_t user_ptr = regs->rdi;
    uint32_t size = (uint32_t)regs->rsi;

    static void *s_bufs_phys[32] = {NULL};
    static void *s_bufs_virt[32] = {NULL};
    static int ring_ptr = -1;

    // Один раз выделяем 32 буфера
    if (!s_bufs_phys[0]) {
      for (int i = 0; i < 32; i++) {
        s_bufs_phys[i] = pmm_alloc_continuous(2);
        s_bufs_virt[i] = (void *)((uintptr_t)s_bufs_phys[i] + hhdm_offset);
        memset(s_bufs_virt[i], 0, 8192);
      }
    }

    extern uint8_t ac97_get_civ();

    // При самом первом звуке начинаем писать СРАЗУ ЗА текущим указателем карты
    if (ring_ptr == -1) {
      ring_ptr = (ac97_get_civ() + 1) % 32;
    }

    uint8_t civ = ac97_get_civ();

    // ВОТ ОН - ИДЕАЛЬНЫЙ ТОРМОЗ ДЛЯ ДУМА (Дистанция)
    // Считаем, на сколько слотов мы убежали вперед от играющего сейчас
    int dist = (ring_ptr - civ + 32) % 32;

    // Если мы оторвались больше чем на 3 буфера — Дум должен подождать!
    // Это дает задержку всего 85мс и НАМЕРТВО защищает от "наслаивания"
    while (dist > 3) {
      __asm__ volatile("pause"); // Ждем, пока карта проиграет звук
      civ = ac97_get_civ();
      dist = (ring_ptr - civ + 32) % 32;
    }

    // Копируем звук
    uint32_t to_copy = (size > 8192) ? 8192 : size;
    memset(s_bufs_virt[ring_ptr], 0, 8192);
    memcpy(s_bufs_virt[ring_ptr], (void *)user_ptr, to_copy);

    // Передаем карте ИНДЕКС и РЕАЛЬНЫЙ размер (to_copy)
    extern void ac97_play_at_idx(int idx, void *phys_addr, uint32_t len);
    ac97_play_at_idx(ring_ptr, s_bufs_phys[ring_ptr], to_copy);

    // Двигаем указатель дальше по кругу
    ring_ptr = (ring_ptr + 1) % 32;
    break;
  }
  case 21: { // SYS_AUDIO_SET_RATE
    extern void ac97_set_rate(uint32_t rate);
    ac97_set_rate((uint32_t)regs->rdi);
    break;
  }
  case 30: { // SYS_MAP_PHYS (Для Композитора: мапим VESA LFB в юзерспейс)
    uint64_t phys = regs->rdi;
    uint32_t size = regs->rsi;
    uint32_t pages = (size + 4095) / 4096;

    // ВНИМАНИЕ: Это опасный вызов. В будущем добавь проверку прав!
    uint64_t virt = 0x20000000000; // Фиксированный адрес для видеопамяти

    page_table_t *pml4 = (page_table_t *)VIRT(current_task->cr3);
    for (uint32_t i = 0; i < pages; i++) {
      vmm_map(pml4, virt + (i * 4096), phys + (i * 4096),
              PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    }
    regs->rax = virt;
    break;
  }

  case 31: { // SYS_SHM_GET
    regs->rax = sys_shm_get((uint32_t)regs->rdi, (uint32_t)regs->rsi);
    break;
  }

  case 32: {                       // SYS_GET_VESA_INFO
    extern uintptr_t fb_base_addr; // Виртуальный адрес от Limine (0xFFFF...)
    extern uint32_t screen_width, screen_height, screen_pitch;
    extern uint64_t hhdm_offset;

    // ПОЛУЧАЕМ ЧИСТЫЙ ФИЗИЧЕСКИЙ АДРЕС (например, 0xFD000000)
    uint64_t phys_fb = (uint64_t)fb_base_addr - hhdm_offset;

    regs->rax = phys_fb;
    regs->rbx = screen_width;
    regs->rcx = screen_height;
    regs->rdx = screen_pitch;
    break;
  }
  case 33: { // SYS_GET_WINDOW_POS
    if (app_win) {
      regs->rax = app_win->x;
      regs->rbx = app_win->y;
    } else {
      regs->rax = 0;
      regs->rbx = 0;
    }
    break;
  }
  case 40: { // SYS_NET_DNS_RESOLVE
    const char *hostname = (const char *)regs->rdi;

    // IPv4 literal short-circuit. Без этого URL вида http://10.0.2.2/
    // и любые тесты против host-only QEMU-сети (где у хоста просто IP, а DNS
    // не настроен) уходят в DNS-запрос к 8.8.8.8 и валятся таймаутом. Парсим
    // строго "a.b.c.d", a-d ∈ [0,255]. На любое отклонение — fallback в DNS.
    if (hostname) {
      uint32_t parts[4] = {0, 0, 0, 0};
      int part_idx = 0;
      int digits_in_part = 0;
      bool ok = true;
      for (const char *p = hostname;; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
          parts[part_idx] = parts[part_idx] * 10 + (uint32_t)(c - '0');
          if (parts[part_idx] > 255 || ++digits_in_part > 3) { ok = false; break; }
        } else if (c == '.') {
          if (digits_in_part == 0 || part_idx >= 3) { ok = false; break; }
          part_idx++;
          digits_in_part = 0;
        } else if (c == '\0') {
          if (part_idx != 3 || digits_in_part == 0) ok = false;
          break;
        } else {
          ok = false;
          break;
        }
      }
      if (ok) {
        // Сетевые драйверы EquinoxOS трактуют uint32_t IP как network-byte-order
        // (a в старшем байте). dns_get_result возвращает в той же форме.
        regs->rax = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
        break;
      }
    }

    net_interface_t *iface = net_get_primary_interface();
    if (!iface) {
      regs->rax = 0;
      break;
    }

    // Enable interrupts so network_thread can process packets
    __asm__ volatile("sti");

    stac();
    dns_query(iface, hostname);
    clac();

    // Wait for resolution — interrupts MUST be on for network_thread
    uint32_t timeout = 1000;
    while (timeout > 0) {
      uint32_t ip = dns_get_result(hostname);
      if (ip != 0) {
        __asm__ volatile("cli");
        regs->rax = ip;
        goto dns_done;
      }
      __asm__ volatile("hlt"); // sleep until next interrupt (timer)
      timeout--;
    }
    __asm__ volatile("cli");
    regs->rax = 0;
  dns_done:
    break;
  }
  case 41: { // SYS_NET_HTTP_GET
    uint32_t ip = (uint32_t)regs->rdi;
    net_interface_t *iface = net_get_primary_interface();
    if (!iface) {
      regs->rax = 0;
      break;
    }

    extern uint8_t *http_response_buf;
    extern uint32_t http_response_len;
    extern bool http_finished;

    http_finished = false;
    if (http_response_buf) {
      kfree(http_response_buf);
      http_response_buf = NULL;
    }
    http_response_len = 0;

    // Enable interrupts so network_thread can process TCP packets
    __asm__ volatile("sti");

    net_wget(iface, ip);

    // Wait for finish — interrupts MUST be on for network_thread
    uint32_t timeout = 2000;
    while (timeout > 0 && !http_finished) {
      __asm__ volatile("hlt"); // sleep until next interrupt (timer)
      timeout--;
    }

    __asm__ volatile("cli");

    if (http_finished && http_response_buf) {
      regs->rax = copy_to_user(http_response_buf, http_response_len);
      if (regs->rsi) {
        stac();
        *(uint32_t *)regs->rsi = http_response_len;
        clac();
      }
    } else {
      regs->rax = 0;
    }
    break;
  }
  case 34: { // SYS_GET_USED_MEM
    regs->rax = pmm_get_used_memory();
    break;
  }
  case 35: { // SYS_GET_TOTAL_MEM
    regs->rax = pmm_get_total_memory();
    break;
  }
  case 50: { // SYS_EXEC
    const char *cmd = (const char *)regs->rdi;
    char cmd_buf[256];
    stac();
    int idx = 0;
    while (idx < 255 && cmd[idx] != '\0') {
      cmd_buf[idx] = cmd[idx];
      idx++;
    }
    cmd_buf[idx] = '\0';
    clac();
    regs->rax = task_exec(cmd_buf) ? 1 : 0;
    break;
  }
  /* ---- IPC: pipes ----------------------------------------------------- */
  case 60: regs->rax = (uint64_t)(int64_t)pipe_create(); break;
  case 61:
    regs->rax = (uint64_t)(int64_t)pipe_read((int)regs->rdi,
                                             (void *)regs->rsi,
                                             (uint32_t)regs->rdx);
    break;
  case 62:
    regs->rax = (uint64_t)(int64_t)pipe_write((int)regs->rdi,
                                              (const void *)regs->rsi,
                                              (uint32_t)regs->rdx);
    break;
  case 63: pipe_close((int)regs->rdi); break;
  /* ---- IPC: message queues ------------------------------------------- */
  case 64:
    regs->rax = (uint64_t)(int64_t)mq_create((uint32_t)regs->rdi);
    break;
  case 65:
    regs->rax = (uint64_t)(int64_t)mq_send((int)regs->rdi,
                                           (const void *)regs->rsi,
                                           (uint32_t)regs->rdx);
    break;
  case 66:
    regs->rax = (uint64_t)(int64_t)mq_recv((int)regs->rdi,
                                           (void *)regs->rsi);
    break;
  case 67: mq_close((int)regs->rdi); break;
  /* ---- task introspection / control (ps / kill / killall) ------------ */
  case 70: { // SYS_TASK_INFO
    int idx = (int)regs->rdi;
    struct user_task_info {
      uint64_t pid;
      uint64_t cr3;
      uint64_t brk;
      uint32_t running;
      uint32_t _pad;
    } *uout = (void *)regs->rsi;
    task_snapshot_t snap;
    if (uout && task_snapshot_at(idx, &snap)) {
      stac();
      uout->pid     = snap.pid;
      uout->cr3     = snap.cr3;
      uout->brk     = snap.brk;
      uout->running = snap.running ? 1u : 0u;
      uout->_pad    = 0;
      clac();
      regs->rax = 1;
    } else {
      regs->rax = 0;
    }
    break;
  }
  case 71: { // SYS_TASK_KILL
    uint64_t pid = regs->rdi;
    regs->rax = task_terminate_by_pid(pid) ? 1 : 0;
    break;
  }
  case 72: { // SYS_TASK_KILLALL
    regs->rax = (uint64_t)task_kill_all_user_count();
    break;
  }
  case 73: { // SYS_SHELL_EXEC — выполнить строку ring-0 shell'а и
             // вернуть printed-вывод в user-buf. См. shellsyntx.h и
             // shell_capture_sink ниже.
    const char *user_line   = (const char *)regs->rdi;
    char       *user_outbuf = (char *)regs->rsi;
    uint64_t    out_cap     = regs->rdx;

    /* 1) Скопировать команду в kernel-память (даём шеллу свободно
     *    дергать sh_print без stac/clac на каждый чих). */
    char kline[256];
    {
      uint64_t i = 0;
      stac();
      while (i < sizeof(kline) - 1 && user_line && user_line[i] != '\0') {
        kline[i] = user_line[i];
        i++;
      }
      clac();
      kline[i] = '\0';
    }

    /* 2) Обнулить capture-буфер и натравить шелл на capture-sink. */
    extern char shell_capture_buf[];
    extern int  shell_capture_pos;
    extern void shell_capture_sink(const char *);

    shell_capture_pos = 0;
    shell_capture_buf[0] = '\0';
    shell_execute_line(kline, shell_capture_sink);

    /* 3) Скопировать результат юзеру, усечение допустимо. */
    int n = shell_capture_pos;
    if (out_cap == 0 || user_outbuf == NULL) {
      regs->rax = (uint64_t)n;
      break;
    }
    uint64_t to_copy = ((uint64_t)n + 1 < out_cap) ? (uint64_t)n + 1 : out_cap;
    stac();
    memcpy(user_outbuf, shell_capture_buf, to_copy);
    if (to_copy > 0) user_outbuf[to_copy - 1] = '\0';
    clac();

    regs->rax = (to_copy > 0) ? to_copy - 1 : 0;
    break;
  }
  case 74: { // SYS_GET_FG_APP — текущий PID foreground-приложения
    // 0 если нет (десктоп пустой → ввод и vram свободны для sysgui).
    // Используется sysgui чтобы не композитить vram, пока активна
    // полно­экранная игра вроде doom — иначе sysgui раз в 16 ms
    // memcpy'ит весь 1024×768×4 backbuffer поверх кадра doom'а, что
    // даёт жёсткое мерцание + просадку FPS.
    extern volatile uint64_t fg_app_pid;
    regs->rax = fg_app_pid;
    break;
  }

  /* ====================================================================
   * Socket API (80–85) — phase 1 of the HTTPS browser stack.
   *
   * All syscalls return a signed int packed into rax (negative = error,
   * see SOCK_ERR_* in net/socket.h). The kernel has full access to user
   * memory while CR0.WP is on, but the SMAP fence still needs explicit
   * stac()/clac() bracketing for the buffer transfers — same convention
   * as the read/write/dns paths above.
   * ================================================================== */
  case 80: { /* SYS_SOCKET () -> int fd */
    regs->rax = (uint64_t)(int64_t)sock_create();
    break;
  }
  case 81: { /* SYS_CONNECT (fd, ip_be, port) -> int rc */
    int      fd     = (int)regs->rdi;
    uint32_t ip_be  = (uint32_t)regs->rsi;
    uint16_t port   = (uint16_t)regs->rdx;
    /* sock_connect spins with sti/hlt internally; mirror DNS handler. */
    __asm__ volatile("sti");
    int rc = sock_connect(fd, ip_be, port);
    __asm__ volatile("cli");
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 82: { /* SYS_SEND (fd, buf, len) -> int sent */
    int            fd  = (int)regs->rdi;
    const uint8_t *buf = (const uint8_t *)regs->rsi;
    uint32_t       len = (uint32_t)regs->rdx;
    stac();
    int rc = sock_send(fd, buf, len);
    clac();
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 83: { /* SYS_RECV (fd, buf, len) -> int recvd */
    int       fd  = (int)regs->rdi;
    uint8_t  *buf = (uint8_t *)regs->rsi;
    uint32_t  len = (uint32_t)regs->rdx;
    /* recv may block — must keep interrupts on while we hlt-wait. */
    __asm__ volatile("sti");
    stac();
    int rc = sock_recv(fd, buf, len);
    clac();
    __asm__ volatile("cli");
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 84: { /* SYS_CLOSE_SOCK (fd) -> int rc */
    int fd = (int)regs->rdi;
    regs->rax = (uint64_t)(int64_t)sock_close(fd);
    break;
  }
  case 85: { /* SYS_SETSOCKOPT (fd, level, optname, val_ptr, vallen) -> int rc */
    int         fd      = (int)regs->rdi;
    int         level   = (int)regs->rsi;
    int         optname = (int)regs->rdx;
    const void *val     = (const void *)regs->rcx;
    uint32_t    vallen  = (uint32_t)regs->r8;
    stac();
    int rc = sock_setsockopt(fd, level, optname, val, vallen);
    clac();
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 86: { /* SYS_GETRANDOM (buf, len, flags) -> int rc
              *   rdi = void   *buf    — userspace destination
              *   rsi = uint32  len    — bytes to fill
              *   rdx = uint32  flags  — reserved (must be 0 for now)
              * Always returns 0 on success, -1 on bad args.
              * No partial fills: either len bytes are written or none. */
    void    *buf   = (void *)regs->rdi;
    uint32_t len   = (uint32_t)regs->rsi;
    uint32_t flags = (uint32_t)regs->rdx;
    if (!buf || flags != 0) {
      regs->rax = (uint64_t)(int64_t)-1;
      break;
    }
    if (len == 0) {
      regs->rax = 0;
      break;
    }
    stac();
    int rc = rdrand_bytes(buf, len);
    clac();
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }

  default:
    break;
  }
}

/* ----- capture-sink для SYS_SHELL_EXEC ------------------------------------
 * Один-единственный буфер, используемый и case 73 выше, и самим sink'ом.
 * Не thread-safe — но syscall_handler у нас и так не reentrant. */
char shell_capture_buf[2048];
int  shell_capture_pos = 0;
const int shell_capture_cap = (int)sizeof(shell_capture_buf);

void shell_capture_sink(const char *s) {
    if (!s) return;
    while (*s && shell_capture_pos < shell_capture_cap - 1) {
        shell_capture_buf[shell_capture_pos++] = *s++;
    }
    shell_capture_buf[shell_capture_pos] = '\0';
}