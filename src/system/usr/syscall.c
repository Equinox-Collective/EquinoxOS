#include "../../syslibc/stdio.h"
#include "../../syslibc/string.h"
#include "../core/cpu.h"
#include "../drivers/devices/audio/ac97.h"
#include "../drivers/devices/keyboard/keyboard.h"
#include "../drivers/hardware/net/dns.h"
#include "../drivers/hardware/net/net.h"
#include "../drivers/hardware/net/socket.h"
#include "../drivers/hardware/net/tcp.h"
#include "../drivers/vesa/vesa.h"
#include "../fs/vfs.h"
#include "../fs/fd.h"
#include "../mem/memory.h"
#include "../mem/pmm.h"
#include "../mem/shm.h"
#include "../mem/vmm.h"
#include "../misc/random.h"
#include "../misc/rtc.h"
#include "../misc/timer.h"
#include "../shell/shellsyntx.h"
#include "../usr/task.h"
#include "ipc.h"
#include <stdint.h>

extern volatile uint32_t tick;
extern void sys_draw_app_buffer(int x, int y, int w, int h, uint32_t *buffer);
extern uint8_t keyboard_pop();
extern void term_print(const char *str);

static volatile int k_app_win_x = 100;
static volatile int k_app_win_y = 100;
static volatile int k_app_win_w = 640;
static volatile int k_app_win_h = 400;
static volatile bool k_app_win_active = false;

/* syscall_regs_t вынесен в uregs.h, чтобы доставка сигналов (signal.c) могла
 * использовать тот же кадр. Поля/смещения по-прежнему обязаны совпадать с
 * порядком push в syscall_interrupt_asm (interrupt.asm). */
#include "uregs.h"
#include "signal.h"
#include "tty.h"
#include "../drivers/hardware/serial/serial.h"  /* Этап 7: FIONREAD + лог ENOSYS */

extern int mouse_x, mouse_y;
extern bool mouse_left_button;

uint64_t copy_to_user(void *kernel_buf, uint64_t size)
{
  if (!kernel_buf || size == 0)
    return 0;

  uint64_t pages = (size + 4095) / 4096;
  static uint64_t user_dynamic_ptr = 0x60000000;
  uint64_t target_virt = user_dynamic_ptr;
  user_dynamic_ptr += (pages * 4096);

  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  page_table_t *pml4 = (page_table_t *)VIRT(cr3);

  for (uint64_t i = 0; i < pages; i++)
  {
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

/* Этап 6e-2: foreground-группа управляющего терминала (job control).
 * 0 == ещё не задана → tcgetpgrp вернёт группу вызывающего (он «на переднем
 * плане» по умолчанию). tcsetpgrp задаёт явно. */
static uint64_t g_tty_fg_pgrp = 0;

void syscall_handler(syscall_regs_t *regs)
{
  uint64_t num = regs->rax;

  switch (num)
  {
  case 1: // SYS_PRINT
    stac();
    term_print((const char *)regs->rdi);
    clac();
    break;
  case 2:
  { // SYS_READ_FILE (Now VFS-agnostic)
    const char *filename = (const char *)regs->rdi;
    uint32_t *out_size_ptr = (uint32_t *)regs->rsi;

    // --- ДИАГНОСТИКА загрузки: имя файла + размер + время чтения ---
    extern volatile uint32_t tick;
    char _fn[64];
    stac();
    int _k = 0; while (_k < 63 && filename[_k]) { _fn[_k] = filename[_k]; _k++; }
    _fn[_k] = 0;
    clac();
    uint32_t _t0 = tick;

    uint32_t size = 0;
    uint8_t *file_data = vfs_read_file(filename, &size);

    {
      extern void serial_puts(uint16_t port, const char *str);
      char _b[160];
      sprintf(_b, "[T=%ums] READ_FILE '%s' size=%u dt=%ums\n",
              (unsigned)tick, _fn, (unsigned)size, (unsigned)(tick - _t0));
      serial_puts(0x3F8 /*COM1*/, _b);
    }

    if (!file_data)
    {
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
    for (uint32_t i = 0; i < pages_needed; i++)
    {
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
  case 3:
  { // SYS_WRITE_FILE (Теперь через VFS!)
    const char *filename = (const char *)regs->rdi;
    const uint8_t *data = (const uint8_t *)regs->rsi;
    uint32_t size = (uint32_t)regs->rdx;

    // Ищем устройство с поддержкой записи (первое попавшееся, обычно EXT2 или
    // FAT32)
    vfs_node_t *dev = vfs_root->next;
    while (dev)
    {
      if (dev->write)
      {
        vfs_node_t file_node;
        memset(&file_node, 0, sizeof(vfs_node_t));
        // Безопасное копирование имени из юзерспейса. Раньше использовался
        // strcpy, который при подаче длинной строки переполнял
        // file_node.name (фикс. 128 байт) → порча стека ядра.
        stac();
        size_t i = 0;
        for (; i < sizeof(file_node.name) - 1 && filename[i] != '\0'; i++)
        {
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
  case 4:
  { // SYS_READ_DIR
    int idx = (int)regs->rdi;

    // Временная структура для безопасной записи в память пользователя
    struct
    {
      char name[128];
      uint32_t size;
      char dev[32];
    } *out = (void *)regs->rsi;

    int current_idx = 0;
    vfs_node_t *dev = vfs_root->next;
    bool found = false;

    while (dev)
    {
      if (dev->readdir)
      {
        for (int i = 0; i < 32; i++)
        {
          vfs_dirent_t *de = dev->readdir(dev, i);
          if (!de)
            break;

          if (current_idx == idx)
          {
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
    if (!k_app_win_active) {
      break; // Мягкая пауза: просто пропускаем кадр, если игра ушла в фон
    }
    {
      extern volatile uint64_t fg_app_pid;
      if (current_task) {
        fg_app_pid = current_task->id;
      }
    }
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
  case 7:
  { // SYS_GET_MOUSE_FULL
    extern int mouse_x, mouse_y;
    extern bool mouse_left_button, mouse_right_button;

    // Всегда возвращаем реальные координаты, чтобы sysgui мог отрисовать курсор
    regs->rax = mouse_x;
    regs->rbx = mouse_y;
    regs->rcx = (mouse_left_button ? 1 : 0) | (mouse_right_button ? 2 : 0);
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
    if (fg_app_pid != 0 && current_task && current_task->id != fg_app_pid)
    {
      regs->rax = 0;
    }
    else
    {
      regs->rax = keyboard_pop();
    }
    break;
  }

  case 10: // SYS_EXIT
    term_print("[SYS] Killing process and freeing RAM...\n");

    // ВАЖНО: Останавливаем звук ДО очистки памяти,
    // чтобы драйвер не пытался читать из удаленных страниц
    ac97_stop();

    /* 0. Закрываем все сокеты этого процесса, иначе TCB остаются
     * "активными" и каждый ретрансмит сервера долбит kernel-лог
     * сообщениями "[TCP] segment for unknown port …". sock_close
     * мягко гасит TCB (FIN) — реальное освобождение слота произойдёт
     * в tcp_tick, когда удалённая сторона ACK'нет наш FIN или истечёт
     * TIME_WAIT. До тех пор пакеты для local_port всё ещё попадают в
     * нужный TCB и игнорируются (CLOSE_WAIT/TIME_WAIT) без спама. */
    {
      extern int sock_close_owned_by(uint64_t pid);
      int n = sock_close_owned_by(current_task->id);
      if (n > 0)
      {
        char mb[64];
        sprintf(mb, "[SYS] Reaped %d socket(s) on exit\n", n);
        term_print(mb);
      }
    }

    // Освобождение памяти процесса, запись кода выхода, пометка zombie и
    // пробуждение родителя в waitpid теперь выполняет task_exit_current()
    // (см. конец case). Здесь оставляем только сброс GUI-состояния.
    extern bool is_app_running;
    is_app_running = false;

    // Если выходит foreground-app — отдаём фокус ввода обратно sysgui.
    {
      extern volatile uint64_t fg_app_pid;
      if (fg_app_pid == current_task->id)
      {
        fg_app_pid = 0;
      }
    }

    k_app_win_active = false; // Просто сбрасываем флаг активности

    {
      extern bool is_app_running;
      is_app_running = false;
    }

    // Записываем код выхода (rdi), освобождаем память процесса, помечаем
    // zombie и будим родителя, если он спит в waitpid. Не возвращается.
    task_exit_current((int)regs->rdi);
    break;
  case 51: // SYS_FORK — клонирование текущего процесса
  {
    /* Транслируем syscall_regs_t -> stack_frame_t (кадр, который умеет
     * восстанавливать планировщик в irq0_handler_asm). Ребёнок "вернётся"
     * из int 0x80 с rax = 0; родителю возвращаем pid ребёнка. */
    stack_frame_t pf;
    pf.rax = 0;
    pf.rbx = regs->rbx;
    pf.rcx = regs->rcx;
    pf.rdx = regs->rdx;
    pf.rsi = regs->rsi;
    pf.rdi = regs->rdi;
    pf.rbp = regs->rbp;
    pf.r8 = regs->r8;
    pf.r9 = regs->r9;
    pf.r10 = regs->r10;
    pf.r11 = regs->r11;
    pf.r12 = regs->r12;
    pf.r13 = regs->r13;
    pf.r14 = regs->r14;
    pf.r15 = regs->r15;
    pf.interrupt_number = 32;
    pf.error_code = 0;
    pf.rip = regs->rip;
    pf.cs = regs->cs;
    pf.rflags = regs->rflags;
    pf.rsp = regs->rsp;
    pf.ss = regs->ss;
    regs->rax = task_fork(&pf);
    break;
  }
  case 52: // SYS_WAITPID (pid, int* status) -> pid / -1
  {
    int st = 0;
    int64_t r = task_waitpid(regs->rdi, &st);
    if (regs->rsi)
    {
      stac();
      *(int *)regs->rsi = st;
      clac();
    }
    regs->rax = (uint64_t)r;
    break;
  }
  case 53: // SYS_GETPID -> реальный pid текущего процесса
    regs->rax = current_task->id;
    break;
  case 54: // SYS_EXECVE (const char* path, char* const argv[], char* const envp[])
  {
    /* Заменяет образ ТЕКУЩЕГО процесса (pid сохраняется). Этап 3: envp
     * копируется и передаётся новому образу (getenv/environ). */
    const char *upath = (const char *)regs->rdi;
    char *const *uargv = (char *const *)regs->rsi;
    char *const *uenvp = (char *const *)regs->rdx;
    if (!upath) { regs->rax = (uint64_t)-1; break; }

    /* Копируем path/argv/envp из пользовательской памяти в ядро ДО любых
     * переключений адресного пространства (под stac/clac — SMAP). */
    char rawpath[256];
    char argstore[16][128];
    char envstore[20][160];
    char *kargv[17];
    char *kenvp[21];
    int kargc = 0, kenvc = 0;

    stac();
    int pi = 0;
    for (; pi < 255 && upath[pi]; pi++) rawpath[pi] = upath[pi];
    rawpath[pi] = 0;
    if (uargv) {
      for (kargc = 0; kargc < 16 && uargv[kargc]; kargc++) {
        const char *s = uargv[kargc];
        int j = 0;
        for (; j < 127 && s[j]; j++) argstore[kargc][j] = s[j];
        argstore[kargc][j] = 0;
        kargv[kargc] = argstore[kargc];
      }
    }
    if (uenvp) {
      for (kenvc = 0; kenvc < 20 && uenvp[kenvc]; kenvc++) {
        const char *s = uenvp[kenvc];
        int j = 0;
        for (; j < 159 && s[j]; j++) envstore[kenvc][j] = s[j];
        envstore[kenvc][j] = 0;
        kenvp[kenvc] = envstore[kenvc];
      }
    }
    clac();
    kenvp[kenvc] = 0;

    /* Этап 3: разрешаем путь относительно cwd процесса (плоский ext2). */
    char kpath[256];
    task_resolve_fs_path(rawpath, kpath, sizeof(kpath));
    if (kpath[0] == '\0') { /* "/" — без имени файла */
      int j = 0; for (; j < 255 && rawpath[j]; j++) kpath[j] = rawpath[j]; kpath[j] = 0;
    }

    if (kargc == 0) { /* argv пуст -> argv[0] = путь (как передан) */
      int j = 0;
      for (; j < 127 && rawpath[j]; j++) argstore[0][j] = rawpath[j];
      argstore[0][j] = 0;
      kargv[0] = argstore[0];
      kargc = 1;
    }
    kargv[kargc] = 0;

    uint64_t entry = 0, ursp = 0, uargvp = 0, uenvpp = 0, ncr3 = 0, nfs = 0;
    if (!task_load_image(kpath, kargv, kargc, kenvp, kenvc,
                         &entry, &ursp, &uargvp, &uenvpp, &ncr3, &nfs)) {
      regs->rax = (uint64_t)-1; /* образ не тронут — старый процесс жив */
      break;
    }

    /* Атомарная замена: переключаем cr3 на новое пространство (ядро отображено
     * в обеих половинах, kstack в HHDM — безопасно), обновляем поля задачи,
     * затем освобождаем старое адресное пространство. */
    uint64_t oldcr3 = current_task->cr3;
    __asm__ volatile("mov %0, %%cr3" : : "r"(ncr3) : "memory");
    current_task->cr3     = ncr3;
    current_task->fs_base = nfs;
    current_task->brk     = 0x40000000;
    /* Этап 4: при exec перехватываемые обработчики -> SIG_DFL (POSIX). */
    task_signal_exec(current_task);
    task_set_fs_base(nfs);
    if (oldcr3 && oldcr3 != ncr3)
      vmm_destroy_address_space(oldcr3);

    /* Перенаправляем возврат из int 0x80 в новый образ (syscall_regs_t
     * содержит iretq-кадр: rip/cs/rflags/rsp/ss). crt0 ждёт rdi=argc, rsi=argv. */
    regs->rip    = entry;
    regs->rsp    = ursp;
    regs->rdi    = (uint64_t)kargc;
    regs->rsi    = uargvp;
    regs->rdx    = uenvpp;   /* Этап 3: crt0 ждёт envp в rdx */
    regs->cs     = 0x23;
    regs->ss     = 0x1B;
    regs->rflags = 0x202;
    regs->rax    = 0;
    break;
  }
  case 11:   // SYS_YIELD (Уступить процессор)
    yield(); // Фактический switch context через int $32
    break;
  case 12:
  { // SYS_GET_FONT
    extern void *vesa_get_font();
    extern uint64_t vesa_get_font_size(void);
    void *kfont = vesa_get_font();

    uint64_t font_addr = (uint64_t)kfont;
    if (font_addr < hhdm_offset)
    {
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
  case 13:
  { // SYS_SLEEP
    uint32_t ms = regs->rdi;
    if (ms > 0)
    {
      current_task->sleep_until = tick + ms;
      yield();
    }
    break;
  }

  case 14:
  {
    uint64_t len = regs->rsi;
    if (len == 0)
    {
      regs->rax = 0;
      break;
    }
    uint64_t pages = (len + 4095) / 4096;
    void *phys = pmm_alloc_continuous(pages);
    if (!phys)
    {
      regs->rax = 0;
      break;
    }
    /* MAP_ANONYMOUS обязан возвращать ЗАНУЛЁННЫЕ страницы. pmm_alloc_continuous
     * переиспользует физические страницы умерших процессов (например, старый
     * текст bash после execve), и mallocng из musl проверяет инварианты своих
     * структур прямо в свежезамапленной памяти: мусор в ней приводил к
     * намеренному a_crash() (hlt в ring3 -> GPF) в ребёнке пайплайна
     * `ls | grep`. SYS_BRK страницы уже зануляет — теперь и mmap. */
    memset((void *)VIRT((uint64_t)phys), 0, pages * 4096);
    static uint64_t mmap_ptr = 0x700000000000;
    uint64_t virt = mmap_ptr;
    mmap_ptr += (pages * 4096);
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    for (uint64_t i = 0; i < pages; i++)
    {
      vmm_map((page_table_t *)VIRT(cr3), virt + (i * 4096),
              (uint64_t)phys + (i * 4096), PTE_USER | PTE_WRITABLE);
    }
    regs->rax = virt;
    break;
  }
  case 15:
  { // SYS_BRK
    if (current_task->brk == 0)
      current_task->brk = 0x40000000;

    uint64_t requested_brk = regs->rdi;
    if (requested_brk == 0)
    {
      regs->rax = current_task->brk;
      break;
    }

    if (requested_brk > current_task->brk)
    {
      // Округляем текущий brk вниз до страницы, а новый - вверх
      uint64_t start_map = (current_task->brk + 4095) & ~4095;
      uint64_t end_map = (requested_brk + 4095) & ~4095;

      uint64_t cr3_val;
      __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
      page_table_t *pml4 = (page_table_t *)VIRT(cr3_val);

      for (uint64_t addr = start_map; addr < end_map; addr += 4096)
      {
        void *phys = pmm_alloc();
        if (phys)
        {
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
  case 16:
  {
    if (regs->rdi == 1 || regs->rdi == 2)
    {
      term_print((const char *)regs->rsi);
      regs->rax = regs->rdx;
    }
    else
    {
      regs->rax = -1;
    }
    break;
  }
  case 17:
  {
    regs->rax = 0;
    break;
  }
  case 18:
  {
    regs->rax = -1;
    break;
  }
  case 19:
  {
    regs->rax = 0;
    break;
  }
  case 20:
  { // SYS_AUDIO_PLAY
    extern int ac97_is_ready(void);
    if (!ac97_is_ready()) break; // карта ещё не инициализирована — тихо выходим
    uintptr_t user_ptr = regs->rdi;
    uint32_t size = (uint32_t)regs->rsi;
    if (size == 0) {
      ac97_stop();
      break;
    }

    static void *s_bufs_phys[32] = {NULL};
    static void *s_bufs_virt[32] = {NULL};
    static int ring_ptr = -1;

    // Один раз выделяем 32 буфера
    if (!s_bufs_phys[0]) {
      for (int i = 0; i < 32; i++) {
        s_bufs_phys[i] = pmm_alloc_continuous(2);
        s_bufs_virt[i] = (void *)((uintptr_t)s_bufs_phys[i] + hhdm_offset);
        memset(s_bufs_virt[i], 0, 8192);
        
        // Привязываем буфер к BDL без старта воспроизведения
        extern void ac97_set_bdl_entry(int idx, void* phys_addr);
        ac97_set_bdl_entry(i, s_bufs_phys[i]);
      }
    }

    extern uint8_t ac97_get_civ();

    // При самом первом звуке начинаем писать СРАЗУ ЗА текущим указателем карты
    if (ring_ptr == -1)
    {
      ring_ptr = (ac97_get_civ() + 1) % 32;
    }

    uint8_t civ = ac97_get_civ();

    // ВОТ ОН - ИДЕАЛЬНЫЙ ТОРМОЗ ДЛЯ ДУМА (Дистанция)
    // Считаем, на сколько слотов мы убежали вперед от играющего сейчас
    int dist = (ring_ptr - civ + 32) % 32;

    int safety = 0;
    while (dist > 8 && safety < 100) {
      current_task->sleep_until = tick + 2; 
      yield();
      civ = ac97_get_civ();
      dist = (ring_ptr - civ + 32) % 32;
      safety++;
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
  case 21:
  { // SYS_AUDIO_SET_RATE
    extern int ac97_is_ready(void);
    if (!ac97_is_ready()) break;
    extern void ac97_set_rate(uint32_t rate);
    ac97_set_rate((uint32_t)regs->rdi);
    break;
  }
  case 22:
  { // SYS_AUDIO_READY — готова ли звуковая карта (AC'97 инициализирован)
    extern int ac97_is_ready(void);
    regs->rax = (uint64_t)ac97_is_ready();
    break;
  }
  case 30:
  { // SYS_MAP_PHYS (Для Композитора: мапим VESA LFB в юзерспейс)
    uint64_t phys = regs->rdi;
    uint32_t size = regs->rsi;
    uint32_t pages = (size + 4095) / 4096;

    uint64_t virt = 0x20000000000; // Фиксированный адрес для видеопамяти

    page_table_t *pml4 = (page_table_t *)VIRT(current_task->cr3);
    for (uint32_t i = 0; i < pages; i++)
    {
      // Добавляем флаги PCD и PWT для активации Write-Combining (Index 3 в PAT)
      vmm_map(pml4, virt + (i * 4096), phys + (i * 4096),
              PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_PCD | PTE_PWT);
    }
    regs->rax = virt;
    break;
  }

  case 31:
  { // SYS_SHM_GET
    regs->rax = sys_shm_get((uint32_t)regs->rdi, (uint32_t)regs->rsi);
    break;
  }

  case 32:
  {                                // SYS_GET_VESA_INFO
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
  case 33:
  { // SYS_GET_WINDOW_POS
    regs->rax = (uint64_t)k_app_win_x;
    regs->rbx = (uint64_t)k_app_win_y;
    break;
  }

  case 36:
  { // SYS_SET_WINDOW_POS (Вызывается из Lua)
    k_app_win_x = (int)regs->rdi;
    k_app_win_y = (int)regs->rsi;
    k_app_win_w = (int)regs->rdx;
    k_app_win_h = (int)regs->rcx;
    k_app_win_active = (k_app_win_w > 0 && k_app_win_h > 0);
    break;
  }
  case 40:
  { // SYS_NET_DNS_RESOLVE
    const char *hostname = (const char *)regs->rdi;

    // IPv4 literal short-circuit. Без этого URL вида http://10.0.2.2/
    // и любые тесты против host-only QEMU-сети (где у хоста просто IP, а DNS
    // не настроен) уходят в DNS-запрос к 8.8.8.8 и валятся таймаутом. Парсим
    // строго "a.b.c.d", a-d ∈ [0,255]. На любое отклонение — fallback в DNS.
    if (hostname)
    {
      uint32_t parts[4] = {0, 0, 0, 0};
      int part_idx = 0;
      int digits_in_part = 0;
      bool ok = true;
      for (const char *p = hostname;; p++)
      {
        char c = *p;
        if (c >= '0' && c <= '9')
        {
          parts[part_idx] = parts[part_idx] * 10 + (uint32_t)(c - '0');
          if (parts[part_idx] > 255 || ++digits_in_part > 3)
          {
            ok = false;
            break;
          }
        }
        else if (c == '.')
        {
          if (digits_in_part == 0 || part_idx >= 3)
          {
            ok = false;
            break;
          }
          part_idx++;
          digits_in_part = 0;
        }
        else if (c == '\0')
        {
          if (part_idx != 3 || digits_in_part == 0)
            ok = false;
          break;
        }
        else
        {
          ok = false;
          break;
        }
      }
      if (ok)
      {
        // Сетевые драйверы EquinoxOS трактуют uint32_t IP как
        // network-byte-order (a в старшем байте). dns_get_result возвращает в
        // той же форме.
        regs->rax =
            (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
        break;
      }
    }

    net_interface_t *iface = net_get_primary_interface();
    if (!iface)
    {
      regs->rax = 0;
      break;
    }

    // Enable interrupts so network_thread can process packets
    __asm__ volatile("sti");

    /* Multi-resolver retry. QEMU SLIRP routinely proxies 8.8.8.8 to a
     * host resolver that returns dead pool members (we kept seeing
     * 8.6.112.6 / 8.47.69.6 for example.com — they SYN-ACK but never
     * answer GETs). 1.1.1.1 and 9.9.9.9 reach different upstream
     * answers via the same SLIRP proxy, so failover actually helps
     * in practice. Each server gets ~400 timer ticks before we move
     * on; total worst-case latency ~1.2 s, same order as the old
     * single-server 1000-tick wait. */
    static const uint32_t dns_servers[] = {
        0x0A000203, /* 10.0.2.3 — встроенный DNS-форвардер QEMU SLIRP:
                     * резолвит через резолвер хоста (Windows), не через
                     * NAT-проксирование UDP на публичные DNS. Самый надёжный
                     * путь под whpx — пробуем его первым. */
        0x08080808, /* 8.8.8.8 — Google (фолбэк) */
        0x01010101, /* 1.1.1.1 — Cloudflare (фолбэк) */
        0x09090909, /* 9.9.9.9 — Quad9 (фолбэк) */
    };
    uint32_t resolved = 0;
    for (unsigned s = 0;
         s < sizeof(dns_servers) / sizeof(dns_servers[0]) && resolved == 0;
         s++)
    {
      stac();
      dns_query(iface, hostname, dns_servers[s]);
      clac();

      uint32_t timeout = 400;
      while (timeout > 0)
      {
        uint32_t ip = dns_get_result(hostname);
        if (ip != 0)
        {
          resolved = ip;
          break;
        }
        __asm__ volatile("hlt"); // sleep until next interrupt (timer)
        timeout--;
      }
    }
    __asm__ volatile("cli");
    regs->rax = resolved;
    break;
  }
  case 41:
  { // SYS_NET_HTTP_GET
    uint32_t ip = (uint32_t)regs->rdi;
    net_interface_t *iface = net_get_primary_interface();
    if (!iface)
    {
      regs->rax = 0;
      break;
    }

    extern uint8_t *http_response_buf;
    extern uint32_t http_response_len;
    extern bool http_finished;

    http_finished = false;
    if (http_response_buf)
    {
      kfree(http_response_buf);
      http_response_buf = NULL;
    }
    http_response_len = 0;

    // Enable interrupts so network_thread can process TCP packets
    __asm__ volatile("sti");

    net_wget(iface, ip);

    // Wait for finish — interrupts MUST be on for network_thread
    uint32_t timeout = 2000;
    while (timeout > 0 && !http_finished)
    {
      __asm__ volatile("hlt"); // sleep until next interrupt (timer)
      timeout--;
    }

    __asm__ volatile("cli");

    if (http_finished && http_response_buf)
    {
      regs->rax = copy_to_user(http_response_buf, http_response_len);
      if (regs->rsi)
      {
        stac();
        *(uint32_t *)regs->rsi = http_response_len;
        clac();
      }
    }
    else
    {
      regs->rax = 0;
    }
    break;
  }
  case 34:
  { // SYS_GET_USED_MEM
    regs->rax = pmm_get_used_memory();
    break;
  }
  case 35:
  { // SYS_GET_TOTAL_MEM
    regs->rax = pmm_get_total_memory();
    break;
  }
  case 50:
  { // SYS_EXEC
    const char *cmd = (const char *)regs->rdi;
    char cmd_buf[256];
    stac();
    int idx = 0;
    while (idx < 255 && cmd[idx] != '\0')
    {
      cmd_buf[idx] = cmd[idx];
      idx++;
    }
    cmd_buf[idx] = '\0';
    clac();
    regs->rax = task_exec(cmd_buf) ? 1 : 0;
    break;
  }
  /* ---- IPC: pipes ----------------------------------------------------- */
  case 60:
    regs->rax = (uint64_t)(int64_t)pipe_create();
    break;
  case 61:
    regs->rax = (uint64_t)(int64_t)pipe_read((int)regs->rdi, (void *)regs->rsi,
                                             (uint32_t)regs->rdx);
    break;
  case 62:
    regs->rax = (uint64_t)(int64_t)pipe_write(
        (int)regs->rdi, (const void *)regs->rsi, (uint32_t)regs->rdx);
    break;
  case 63:
    pipe_close((int)regs->rdi);
    break;
  /* ---- IPC: message queues ------------------------------------------- */
  case 64:
    regs->rax = (uint64_t)(int64_t)mq_create((uint32_t)regs->rdi);
    break;
  case 65:
    regs->rax = (uint64_t)(int64_t)mq_send(
        (int)regs->rdi, (const void *)regs->rsi, (uint32_t)regs->rdx);
    break;
  case 66:
    regs->rax = (uint64_t)(int64_t)mq_recv((int)regs->rdi, (void *)regs->rsi);
    break;
  case 67:
    mq_close((int)regs->rdi);
    break;
  /* ---- task introspection / control (ps / kill / killall) ------------ */
  case 70:
  { // SYS_TASK_INFO
    int idx = (int)regs->rdi;
    struct user_task_info
    {
      uint64_t pid;
      uint64_t cr3;
      uint64_t brk;
      uint32_t running;
      uint32_t _pad;
    } *uout = (void *)regs->rsi;
    task_snapshot_t snap;
    if (uout && task_snapshot_at(idx, &snap))
    {
      stac();
      uout->pid = snap.pid;
      uout->cr3 = snap.cr3;
      uout->brk = snap.brk;
      uout->running = snap.running ? 1u : 0u;
      uout->_pad = 0;
      clac();
      regs->rax = 1;
    }
    else
    {
      regs->rax = 0;
    }
    break;
  }
  case 71: { // SYS_TASK_KILL
    uint64_t pid = regs->rdi;
    
    // ЗАЩИТА ОТ САМОУБИЙСТВА: Процесс не может завершить сам себя через этот вызов!
    if (current_task && pid == current_task->id) {
      regs->rax = 0;
      break;
    }
    
    regs->rax = task_terminate_by_pid(pid) ? 1 : 0;
    break;
  }
  case 72:
  { // SYS_TASK_KILLALL
    regs->rax = (uint64_t)task_kill_all_user_count();
    break;
  }
  case 73:
  { // SYS_SHELL_EXEC — выполнить строку ring-0 shell'а и
    // вернуть printed-вывод в user-buf. См. shellsyntx.h и
    // shell_capture_sink ниже.
    const char *user_line = (const char *)regs->rdi;
    char *user_outbuf = (char *)regs->rsi;
    uint64_t out_cap = regs->rdx;

    /* 1) Скопировать команду в kernel-память (даём шеллу свободно
     *    дергать sh_print без stac/clac на каждый чих). */
    char kline[256];
    {
      uint64_t i = 0;
      stac();
      while (i < sizeof(kline) - 1 && user_line && user_line[i] != '\0')
      {
        kline[i] = user_line[i];
        i++;
      }
      clac();
      kline[i] = '\0';
    }

    /* 2) Обнулить capture-буфер и натравить шелл на capture-sink. */
    extern char shell_capture_buf[];
    extern int shell_capture_pos;
    extern void shell_capture_sink(const char *);

    shell_capture_pos = 0;
    shell_capture_buf[0] = '\0';
    shell_execute_line(kline, shell_capture_sink);

    /* 3) Скопировать результат юзеру, усечение допустимо. */
    int n = shell_capture_pos;
    if (out_cap == 0 || user_outbuf == NULL)
    {
      regs->rax = (uint64_t)n;
      break;
    }
    uint64_t to_copy = ((uint64_t)n + 1 < out_cap) ? (uint64_t)n + 1 : out_cap;
    stac();
    memcpy(user_outbuf, shell_capture_buf, to_copy);
    if (to_copy > 0)
      user_outbuf[to_copy - 1] = '\0';
    clac();

    regs->rax = (to_copy > 0) ? to_copy - 1 : 0;
    break;
  }
  case 74:
  { // SYS_GET_FG_APP — текущий PID foreground-приложения
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
  case 80:
  { /* SYS_SOCKET () -> int fd */
    regs->rax = (uint64_t)(int64_t)sock_create();
    break;
  }
  case 81:
  { /* SYS_CONNECT (fd, ip_be, port) -> int rc */
    int fd = (int)regs->rdi;
    uint32_t ip_be = (uint32_t)regs->rsi;
    uint16_t port = (uint16_t)regs->rdx;
    /* sock_connect spins with sti/hlt internally; mirror DNS handler. */
    __asm__ volatile("sti");
    int rc = sock_connect(fd, ip_be, port);
    __asm__ volatile("cli");
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 82:
  { /* SYS_SEND (fd, buf, len) -> int sent */
    int fd = (int)regs->rdi;
    const uint8_t *buf = (const uint8_t *)regs->rsi;
    uint32_t len = (uint32_t)regs->rdx;
    stac();
    int rc = sock_send(fd, buf, len);
    clac();
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 83:
  { /* SYS_RECV (fd, buf, len) -> int recvd */
    int fd = (int)regs->rdi;
    uint8_t *buf = (uint8_t *)regs->rsi;
    uint32_t len = (uint32_t)regs->rdx;
    /* recv may block — must keep interrupts on while we hlt-wait. */
    __asm__ volatile("sti");
    stac();
    int rc = sock_recv(fd, buf, len);
    clac();
    __asm__ volatile("cli");
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 84:
  { /* SYS_CLOSE_SOCK (fd) -> int rc */
    int fd = (int)regs->rdi;
    regs->rax = (uint64_t)(int64_t)sock_close(fd);
    break;
  }
  case 85:
  { /* SYS_SETSOCKOPT (fd, level, optname, val_ptr, vallen) -> int rc
     */
    int fd = (int)regs->rdi;
    int level = (int)regs->rsi;
    int optname = (int)regs->rdx;
    const void *val = (const void *)regs->rcx;
    uint32_t vallen = (uint32_t)regs->r8;
    stac();
    int rc = sock_setsockopt(fd, level, optname, val, vallen);
    clac();
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 86:
  { /* SYS_GETRANDOM (buf, len, flags) -> int rc
     *   rdi = void   *buf    — userspace destination
     *   rsi = uint32  len    — bytes to fill
     *   rdx = uint32  flags  — reserved (must be 0 for now)
     * Always returns 0 on success, -1 on bad args.
     * No partial fills: either len bytes are written or none. */
    void *buf = (void *)regs->rdi;
    uint32_t len = (uint32_t)regs->rsi;
    uint32_t flags = (uint32_t)regs->rdx;
    if (!buf || flags != 0)
    {
      regs->rax = (uint64_t)(int64_t)-1;
      break;
    }
    if (len == 0)
    {
      regs->rax = 0;
      break;
    }
    stac();
    int rc = rdrand_bytes(buf, len);
    clac();
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 87:
  { /* SYS_GET_WALL_TIME (uint64_t *out_unix_secs) -> int rc
     *   rdi = uint64_t *out — must be non-NULL, user-mapped.
     * Writes the current UTC time (whole seconds since the
     * Unix epoch) read from the CMOS RTC. The kernel-side
     * helper rtc_unix_time() does its own UIP/recheck loop,
     * so the result is consistent across the seconds tick. */
    uint64_t *out = (uint64_t *)regs->rdi;
    if (!out)
    {
      regs->rax = (uint64_t)(int64_t)-1;
      break;
    }
    uint64_t now = rtc_unix_time();
    stac();
    *out = now;
    clac();
    regs->rax = 0;
    break;
  }

  case 88:
  { // SYS_BOOT_ANIM_DONE — sysgui сообщает, что первый кадр рабочего стола
    // уже на экране. Гасим kernel-side boot-анимацию Nyan Cat: дальше
    // фреймбуфером владеет GUI, и подрисовка гифки из PIT-таймера затирала
    // бы его. Вызывается ровно при первом present (см. enGUI main.c,
    // copy_dirty_to_vram). Делать это раньше (в kmain или на GET_VESA_INFO)
    // нельзя — гифка замёрзнет в зазоре до первого кадра GUI.
    extern volatile int nyan_boot_active;
    extern void idt_set_syscall_trap_gate(int on);
    // Засекаем РЕАЛЬНОЕ время загрузки (tick == мс, PIT 1 кГц) в момент
    // первого кадра GUI. kmain сохранит его в /boottime для самоподстройки
    // прогресс-бара на следующем запуске.
    extern volatile uint32_t tick;
    extern volatile uint32_t boot_measured_ms;
    if (boot_measured_ms == 0) {
      boot_measured_ms = tick;
      extern void klog_t(const char *tag);
      klog_t("GUI first frame (syscall 88)");
    }
    nyan_boot_active = 0;
    // Анимация закончилась — возвращаем int 0x80 в режим interrupt gate,
    // чтобы после загрузки модель параллелизма ядра была прежней (IF=0 во
    // время сисколлов). На время boot-анимации он был trap gate, чтобы PIT
    // мог крутить гифку даже во время блокирующих сисколлов.
    idt_set_syscall_trap_gate(0);
    regs->rax = 0;
    break;
  }

  /* ====================================================================
   * POSIX fd API  (90–95)
   *
   * These syscalls give userspace a proper open/read/write/close/seek/stat
   * interface backed by the kernel fd table (src/system/fs/fd.c).
   * They are the foundation that Bash, mlibc ports, and any POSIX app need.
   *
   * Calling convention (mirrors Linux x86_64 where possible):
   *   90  SYS_OPEN      (path, flags)            -> int fd / -1
   *   91  SYS_CLOSE_FD  (fd)                     -> int 0 / -1
   *   92  SYS_READ_FD   (fd, buf, size)           -> int bytes_read / -1
   *   93  SYS_WRITE_FD  (fd, buf, size)           -> int bytes_written / -1
   *   94  SYS_SEEK      (fd, offset, whence)      -> int new_pos / -1
   *   95  SYS_FSTAT     (fd, uint32_t *out_size)  -> int 0 / -1
   *   96  SYS_STAT_PATH (path, uint32_t *out_sz)  -> int 0 / -1
   * ================================================================== */
  case 90:
  { /* SYS_OPEN (path, flags) -> fd */
    const char *path  = (const char *)regs->rdi;
    int         oflags = (int)regs->rsi;
    char kpath[256];
    stac();
    size_t i = 0;
    while (i < sizeof(kpath) - 1 && path && path[i] != '\0') {
      kpath[i] = path[i]; i++;
    }
    clac();
    kpath[i] = '\0';
    /* Этап 3: относительные пути -> относительно cwd процесса. */
    char rpath[256];
    task_resolve_fs_path(kpath, rpath, sizeof(rpath));
    regs->rax = (uint64_t)(int64_t)fd_open(rpath[0] ? rpath : kpath, oflags);
    break;
  }
  case 91:
  { /* SYS_CLOSE_FD (fd) -> 0 / -1 */
    regs->rax = (uint64_t)(int64_t)fd_close((int)regs->rdi);
    break;
  }
  case 92:
  { /* SYS_READ_FD (fd, buf, size) -> bytes_read / -1 */
    int      fd   = (int)regs->rdi;
    void    *buf  = (void *)regs->rsi;
    uint32_t size = (uint32_t)regs->rdx;
    stac();
    int rc = fd_read(fd, buf, size);
    clac();
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 93:
  { /* SYS_WRITE_FD (fd, buf, size) -> bytes_written / -1
     * Этап 2: fd 1/2 больше НЕ спец-кейсятся здесь — консоль обрабатывает
     * сам fd_write (kind == OFD_CONSOLE). Иначе перенаправление stdout в
     * пайп/файл (dup2) не работало бы. */
    int      fd   = (int)regs->rdi;
    const void *buf = (const void *)regs->rsi;
    uint32_t size  = (uint32_t)regs->rdx;
    stac();
    int rc = fd_write(fd, buf, size);
    clac();
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 94:
  { /* SYS_SEEK (fd, offset, whence) -> new_pos / -1 */
    int     fd     = (int)regs->rdi;
    int32_t offset = (int32_t)regs->rsi;
    int     whence = (int)regs->rdx;
    regs->rax = (uint64_t)(int64_t)fd_seek(fd, offset, whence);
    break;
  }
  case 95:
  { /* SYS_FSTAT (fd, uint32_t *out_size) -> 0 / -1 */
    int       fd  = (int)regs->rdi;
    uint32_t *out = (uint32_t *)regs->rsi;
    uint32_t  sz  = 0;
    int rc = fd_stat(fd, &sz);
    if (out) {
      stac();
      *out = sz;
      clac();
    }
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 96:
  { /* SYS_STAT_PATH (path, uint32_t *out_size) -> 0 / -1 */
    const char *path  = (const char *)regs->rdi;
    uint32_t   *out   = (uint32_t *)regs->rsi;
    char kpath[256];
    stac();
    size_t i = 0;
    while (i < sizeof(kpath) - 1 && path && path[i] != '\0') {
      kpath[i] = path[i]; i++;
    }
    clac();
    kpath[i] = '\0';
    /* Этап 3: относительные пути -> относительно cwd процесса. */
    char rpath[256];
    task_resolve_fs_path(kpath, rpath, sizeof(rpath));
    uint32_t sz = 0;
    int rc = fd_stat_path(rpath[0] ? rpath : kpath, &sz);
    if (out) {
      stac();
      *out = sz;
      clac();
    }
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }

  /* ---- Этап 2: pipe / dup / dup2 ------------------------------------- */
  case 97:
  { /* SYS_PIPE (int fds[2]) -> 0 / -1 */
    int *user_fds = (int *)regs->rdi;
    int kfds[2] = { -1, -1 };
    int rc = fd_make_pipe(kfds);
    if (rc == 0 && user_fds) {
      stac();
      user_fds[0] = kfds[0];
      user_fds[1] = kfds[1];
      clac();
    }
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 98:
  { /* SYS_DUP (oldfd) -> newfd / -1 */
    regs->rax = (uint64_t)(int64_t)fd_dup((int)regs->rdi);
    break;
  }
  case 99:
  { /* SYS_DUP2 (oldfd, newfd) -> newfd / -1 */
    regs->rax = (uint64_t)(int64_t)fd_dup2((int)regs->rdi, (int)regs->rsi);
    break;
  }

  /* ---- Этап 3: cwd ---------------------------------------------------- */
  case 100:
  { /* SYS_GETCWD (char *buf, uint32_t size) -> len / -1 */
    char     *ubuf = (char *)regs->rdi;
    uint32_t  usize = (uint32_t)regs->rsi;
    const char *cwd = task_current_cwd();
    int len = 0; while (cwd[len]) len++;
    if (!ubuf || usize == 0 || (uint32_t)(len + 1) > usize) {
      regs->rax = (uint64_t)-1; break;
    }
    stac();
    int k = 0; for (; k < len; k++) ubuf[k] = cwd[k];
    ubuf[k] = '\0';
    clac();
    regs->rax = (uint64_t)(int64_t)len;
    break;
  }
  case 101:
  { /* SYS_CHDIR (const char *path) -> 0 / -1 */
    const char *upath = (const char *)regs->rdi;
    if (!upath) { regs->rax = (uint64_t)-1; break; }
    char kpath[256];
    stac();
    int i = 0;
    while (i < (int)sizeof(kpath) - 1 && upath[i]) { kpath[i] = upath[i]; i++; }
    clac();
    kpath[i] = '\0';
    regs->rax = (uint64_t)(int64_t)task_chdir(kpath);
    break;
  }

  case 102:
  { // SYS_KILL (pid, sig) -> 0 / -1
    regs->rax = (uint64_t)(int64_t)signal_send(regs->rdi, (int)regs->rsi);
    break;
  }
  case 103:
  { // SYS_SIGACTION (sig, handler, restorer, &old) -> 0 / -1
    uint64_t old = 0;
    int rc = signal_setaction((int)regs->rdi, regs->rsi, regs->rdx, &old);
    if (regs->rcx) { stac(); *(uint64_t *)regs->rcx = old; clac(); }
    regs->rax = (uint64_t)(int64_t)rc;
    break;
  }
  case 104:
    // SYS_SIGRETURN — восстановить контекст после обработчика (не «возвращает»).
    signal_sigreturn(regs);
    break;
  case 105:
  { // SYS_SIGPROCMASK (how, set, &old)
    uint64_t old = 0;
    signal_procmask((int)regs->rdi, regs->rsi, &old);
    if (regs->rdx) { stac(); *(uint64_t *)regs->rdx = old; clac(); }
    regs->rax = 0;
    break;
  }

  case 106:
  { /* SYS_IOCTL (fd, request, argp) -> 0 / -1 (Этап 5: termios/winsize)
     *
     * Поддерживаются только консольные fd (0/1/2). Для остальных -> -1
     * (ENOTTY) — этим же isatty() в libc понимает «это не терминал». */
    int       fd  = (int)regs->rdi;
    uint64_t  req = regs->rsi;
    void     *arg = (void *)regs->rdx;

    /* Этап 7 (bash): консольный fd не обязан быть 0/1/2 — bash двигает свой
     * tty-fd наверх через fcntl(F_DUPFD). Проверяем ВИД дескриптора. */
    {
      int kind = 0; uint32_t sz_ = 0;
      if (fd_statx(fd, &kind, &sz_) != 0 || kind != OFD_CONSOLE) {
        regs->rax = (uint64_t)(int64_t)-1;  /* не tty -> ENOTTY для isatty() */
        break;
      }
    }

    switch (req) {
    case K_TCGETS: {
      ktermios_t kt;
      if (tty_get_termios(&kt) != 0 || !arg) { regs->rax = (uint64_t)(int64_t)-1; break; }
      stac(); *(ktermios_t *)arg = kt; clac();
      regs->rax = 0;
      break;
    }
    case K_TCSETS:
    case K_TCSETSW:
    case K_TCSETSF: {
      if (!arg) { regs->rax = (uint64_t)(int64_t)-1; break; }
      ktermios_t kt;
      stac(); kt = *(ktermios_t *)arg; clac();
      regs->rax = (uint64_t)(int64_t)tty_set_termios(&kt);
      break;
    }
    case K_TIOCGWINSZ: {
      if (!arg) { regs->rax = (uint64_t)(int64_t)-1; break; }
      struct kwinsize ws;
      tty_get_winsize(&ws);
      stac(); *(struct kwinsize *)arg = ws; clac();
      regs->rax = 0;
      break;
    }
    case K_TIOCGPGRP: {  /* tcgetpgrp(fd): foreground-группа терминала */
      if (!arg) { regs->rax = (uint64_t)(int64_t)-1; break; }
      uint64_t fg = g_tty_fg_pgrp ? g_tty_fg_pgrp
                  : (current_task->pgid ? current_task->pgid : current_task->id);
      stac(); *(int *)arg = (int)fg; clac();
      regs->rax = 0;
      break;
    }
    case K_TIOCSPGRP: {  /* tcsetpgrp(fd, pgrp): задать foreground-группу */
      if (!arg) { regs->rax = (uint64_t)(int64_t)-1; break; }
      int pg; stac(); pg = *(int *)arg; clac();
      if (pg <= 0) { regs->rax = (uint64_t)(int64_t)-1; break; }
      g_tty_fg_pgrp = (uint64_t)pg;
      regs->rax = 0;
      break;
    }
    case 0x541B: {  /* FIONREAD (Этап 7, readline): байт во входном буфере tty */
      if (!arg) { regs->rax = (uint64_t)(int64_t)-1; break; }
      extern int serial_received(uint16_t port);
      int avail = serial_received(COM1) ? 1 : 0;   /* UART без FIFO-учёта: 0/1 */
      stac(); *(int *)arg = avail; clac();
      regs->rax = 0;
      break;
    }
    default:
      regs->rax = (uint64_t)(int64_t)-1;  /* неизвестный ioctl */
      break;
    }
    break;
  }

  default:
    break;
  }

  /* Этап 4: перед возвратом в ring3 — доставка ожидающих сигналов.
   * Может перенаправить regs в обработчик или завершить процесс
   * (task_exit_current не возвращается). */
  signal_deliver(regs);
}

/* =========================================================================
 * Этап 6a: Linux-ABI шлюз (int 0x81)
 *
 * musl собран под Linux: инструкция `syscall`, линуксовые номера сисколлов
 * (read=0/write=1/...), 4-й аргумент в r10. Чтобы не трогать MSR/swapgs и не
 * конфликтовать с нашими номерами 1..106, musl патчится в ОДНОМ файле
 * (arch/x86_64/syscall_arch.h): инструкция `syscall` -> `int $0x81`. `int` не
 * клобберит rcx/r11, а 4-й аргумент остаётся в r10 (стаб его уже сохраняет).
 *
 * linux_syscall_handler принимает тот же кадр syscall_regs_t. Стратегия:
 *   - для сисколлов с совпадающей семантикой — переписываем regs->rax на наш
 *     родной номер (и при нужде перекладываем аргументы) и ДЕЛЕГИРУЕМ в
 *     syscall_handler (он же доставит сигналы);
 *   - для остальных — обрабатываем здесь и сами зовём signal_deliver.
 *
 * Возврат — по соглашению Linux: при ошибке отрицательный -errno. Наши родные
 * сисколлы возвращают -1; musl трактует любой возврат в [-4095,-1] как ошибку
 * (errno = -ret), поэтому -1 «работает» (errno будет 1/EPERM — уточним в 6d).
 *
 * 6d (done): настоящий rt_sigaction/rt_sigprocmask (linux struct sigaction).
 * TODO: utsname/uname, clock_gettime, точные errno по остальным сисколлам.
 * ========================================================================= */

/* Линуксовые аргументы (x86-64): a1=rdi a2=rsi a3=rdx a4=r10 a5=r8 a6=r9. */
struct l_iovec { const void *iov_base; uint64_t iov_len; };
struct l_timespec { int64_t tv_sec; int64_t tv_nsec; };

#define LERR(e) ((uint64_t)(int64_t)(-(e)))
#define L_ENOSYS 38
#define L_EINVAL 22
#define L_ENOMEM 12
#define L_ENOENT 2
#define L_EBADF  9
#define L_EINTR  4
#define L_ECHILD 10
#define L_ESRCH  3
#define L_EPERM  1
#define L_EMFILE 24

/* ---- Этап 6d: Linux x86-64 struct sigaction (kernel-ABI, как его передаёт
 * musl __libc_sigaction). Поля/смещения фиксированы ядром Linux:
 *   sa_handler@0, sa_flags@8, sa_restorer@16, sa_mask@24 (sigsetsize байт).
 * musl всегда ставит SA_RESTORER и кладёт адрес трамплина в sa_restorer. */
struct l_sigaction {
  uint64_t sa_handler;   /* 0  : 0=SIG_DFL, 1=SIG_IGN, иначе адрес ring3 */
  uint64_t sa_flags;     /* 8  */
  uint64_t sa_restorer;  /* 16 : трамплин sigreturn (при SA_RESTORER) */
  uint64_t sa_mask;      /* 24 : младшие 64 бита маски (sigsetsize=8) */
};
#define L_SA_RESTORER 0x04000000ULL

/* ---- Этап 6c: Linux x86-64 struct stat (== struct kstat в musl, 144 байта).
 * На x86_64 musl заполняет ИМЕННО этот layout через SYS_fstat/stat/lstat/
 * fstatat (statx пропускается, т.к. st_atime_sec == time_t). Поля/смещения
 * взяты из musl arch/x86_64/kstat.h — менять нельзя. */
struct l_stat {
  uint64_t st_dev;        /* 0  */
  uint64_t st_ino;        /* 8  */
  uint64_t st_nlink;      /* 16 */
  uint32_t st_mode;       /* 24 */
  uint32_t st_uid;        /* 28 */
  uint32_t st_gid;        /* 32 */
  uint32_t __pad0;        /* 36 */
  uint64_t st_rdev;       /* 40 */
  int64_t  st_size;       /* 48 */
  int64_t  st_blksize;    /* 56 */
  int64_t  st_blocks;     /* 64 */
  int64_t  st_atime_sec, st_atime_nsec;  /* 72, 80  */
  int64_t  st_mtime_sec, st_mtime_nsec;  /* 88, 96  */
  int64_t  st_ctime_sec, st_ctime_nsec;  /* 104,112 */
  int64_t  __unused[3];                  /* 120     */
};
_Static_assert(sizeof(struct l_stat) == 144, "l_stat must match x86_64 kstat");

#define L_S_IFREG 0x8000u
#define L_S_IFDIR 0x4000u
#define L_S_IFCHR 0x2000u
#define L_S_IFIFO 0x1000u
#define L_AT_EMPTY_PATH 0x1000
#define L_O_DIRECTORY 0200000   /* musl O_DIRECTORY (x86_64), восьмеричное */

/* Скопировать путь из юзерспейса (stac/clac) и нормализовать к cwd процесса. */
static void lx_copy_path(uint64_t uptr, char *out, int outsz)
{
  char kpath[256];
  const char *p = (const char *)uptr;
  int i = 0;
  if (p) {
    stac();
    while (i < (int)sizeof(kpath) - 1 && p[i]) { kpath[i] = p[i]; i++; }
    clac();
  }
  kpath[i] = '\0';
  char rpath[256];
  task_resolve_fs_path(kpath, rpath, sizeof(rpath));
  const char *src = rpath[0] ? rpath : kpath;
  int j = 0;
  for (; src[j] && j < outsz - 1; j++) out[j] = src[j];
  out[j] = '\0';
}

/* Заполнить пользовательский struct l_stat (mode уже включает тип S_IF*). */
static void lx_fill_stat(uint64_t uptr, uint32_t mode, uint64_t size)
{
  if (!uptr) return;
  struct l_stat st;
  memset(&st, 0, sizeof(st));
  st.st_dev     = 1;
  st.st_ino     = 1;
  st.st_nlink   = 1;
  st.st_mode    = mode;
  st.st_size    = (int64_t)size;
  st.st_blksize = 4096;
  st.st_blocks  = (int64_t)((size + 511) / 512);
  int64_t now   = (int64_t)rtc_unix_time();
  st.st_atime_sec = st.st_mtime_sec = st.st_ctime_sec = now;
  stac();
  memcpy((void *)uptr, &st, sizeof(st));
  clac();
}

/* stat по пути: 0 + mode/size если существует (файл/каталог), иначе -1. */
static int lx_path_stat(const char *path, uint32_t *out_mode, uint64_t *out_size)
{
  uint32_t sz = 0;
  if (fd_stat_path(path, &sz) == 0) { *out_mode = L_S_IFREG | 0644u; *out_size = sz; return 0; }
  if (vfs_dir_exists(path))         { *out_mode = L_S_IFDIR | 0755u; *out_size = 0;  return 0; }
  return -1;
}

/* stat по fd: вид ofd -> mode (+ size для обычных файлов). */
static int lx_fd_stat(int fd, uint32_t *out_mode, uint64_t *out_size)
{
  int kind = 0; uint32_t sz = 0;
  if (fd_statx(fd, &kind, &sz) != 0) return -1;
  switch (kind) {
    case OFD_CONSOLE: *out_mode = L_S_IFCHR | 0620u; *out_size = 0;  break;
    case OFD_PIPE_R:
    case OFD_PIPE_W:  *out_mode = L_S_IFIFO | 0600u; *out_size = 0;  break;
    case OFD_DIR:     *out_mode = L_S_IFDIR | 0755u; *out_size = 0;  break;
    default:          *out_mode = L_S_IFREG | 0644u; *out_size = sz; break;
  }
  return 0;
}

/* Этап 6c-2: если path — каталог (или задан O_DIRECTORY и это не файл),
 * открыть его как OFD_DIR (для opendir/getdents64). Используется и open(2),
 * и openat(257) — musl на x86_64 для open() шлёт именно SYS_open=2.
 * Возвращает 1, если обработано (regs->rax уже выставлен), иначе 0. */
static int lx_open_dir_maybe(syscall_regs_t *regs, uint64_t path_uptr, int lflags)
{
  char kpath[256];
  int ci = 0;
  const char *upath = (const char *)path_uptr;
  if (upath) {
    stac();
    while (ci < (int)sizeof(kpath) - 1 && upath[ci]) { kpath[ci] = upath[ci]; ci++; }
    clac();
  }
  kpath[ci] = '\0';

  char rpath[256];
  task_resolve_fs_path(kpath, rpath, sizeof(rpath));  /* плоский путь без '/' */

  uint32_t fsz = 0;
  int is_file = (fd_stat_path(rpath, &fsz) == 0);

  char logical[260];
  logical[0] = '/';
  int lj = 0;
  for (; rpath[lj] && lj < (int)sizeof(logical) - 2; lj++) logical[lj + 1] = rpath[lj];
  logical[lj + 1] = '\0';

  if (!is_file && ((lflags & L_O_DIRECTORY) || vfs_dir_exists(logical))) {
    int fd = fd_opendir(rpath);
    regs->rax = (fd < 0) ? LERR(L_ENOENT) : (uint64_t)fd;
    return 1;
  }
  return 0;
}

void linux_syscall_handler(syscall_regs_t *regs)
{
  uint64_t n = regs->rax;

  switch (n)
  {
  /* ---- совпадающая семантика: переписать номер и делегировать ---------- */
  case 0:  regs->rax = 92; break;            /* read(fd,buf,cnt) -> SYS_READ_FD */
  case 1:  regs->rax = 93; break;            /* write -> SYS_WRITE_FD */
  case 2:                                    /* open(path,flags,mode) */
    /* Этап 6c-2: каталог -> OFD_DIR; иначе -> SYS_OPEN(path,flags). */
    if (lx_open_dir_maybe(regs, regs->rdi, (int)regs->rsi)) { signal_deliver(regs); return; }
    /* Этап 7: несуществующий файл без O_CREAT -> честный ENOENT (родной
     * SYS_OPEN отдаёт -1, что в Linux-errno читалось бы как EPERM). */
    if (!((int)regs->rsi & 0x40 /* O_CREAT */)) {
      char p7[256]; uint32_t m7; uint64_t s7;
      lx_copy_path(regs->rdi, p7, sizeof(p7));
      if (lx_path_stat(p7, &m7, &s7) != 0) { regs->rax = LERR(L_ENOENT); signal_deliver(regs); return; }
    }
    regs->rax = 90; break;
  case 3:  regs->rax = 91; break;            /* close -> SYS_CLOSE_FD */
  case 8:  regs->rax = 94; break;            /* lseek(fd,off,whence) -> SYS_SEEK */
  case 12: regs->rax = 15; break;            /* brk(addr) -> SYS_BRK */
  case 16: regs->rax = 106; break;           /* ioctl(fd,req,argp) -> SYS_IOCTL */
  case 32: regs->rax = 98; break;            /* dup(oldfd) -> SYS_DUP */
  case 33: regs->rax = 99; break;            /* dup2(old,new) -> SYS_DUP2 */
  case 292: regs->rax = 99; break;           /* dup3(old,new,flags) -> SYS_DUP2 (flags игнор) */
  case 22: regs->rax = 97; break;            /* pipe(fds) -> SYS_PIPE */
  case 293: regs->rax = 97; break;           /* pipe2(fds,flags) -> SYS_PIPE (flags игнор) */
  case 39: regs->rax = 53; break;            /* getpid -> SYS_GETPID */
  case 57: regs->rax = 51; break;            /* fork -> SYS_FORK */
  case 58: regs->rax = 51; break;            /* vfork -> SYS_FORK (без COW-семантики) */
  case 59: regs->rax = 54; break;            /* execve(path,argv,envp) -> SYS_EXECVE */
  case 60: regs->rax = 10; break;            /* exit -> SYS_EXIT */
  case 231: regs->rax = 10; break;           /* exit_group -> SYS_EXIT */

  case 62: regs->rax = 102; break;           /* kill(pid,sig) -> SYS_KILL */
  case 200: regs->rax = 102; break;          /* tkill(tid,sig) -> SYS_KILL (tid==pid; raise() в musl) */
  case 79: regs->rax = 100; break;           /* getcwd(buf,size) -> SYS_GETCWD */
  case 80: regs->rax = 101; break;           /* chdir(path) -> SYS_CHDIR */
  case 15: regs->rax = 104; break;           /* rt_sigreturn -> SYS_SIGRETURN */
  case 318: regs->rax = 86; break;           /* getrandom(buf,len,flags) -> SYS_GETRANDOM */

  /* ---- требуют перетасовки аргументов, затем делегирование ------------- */
  case 234:                                  /* tgkill(tgid, tid, sig) -> SYS_KILL(tid,sig) */
    regs->rdi = regs->rsi;                    /* pid = tid */
    regs->rsi = regs->rdx;                    /* sig */
    regs->rax = 102;
    break;

  case 257:                                  /* openat(dirfd,path,flags,mode) */
    /* Этап 6c-2: каталог -> OFD_DIR; иначе делегируем родному SYS_OPEN. */
    if (lx_open_dir_maybe(regs, regs->rsi, (int)regs->rdx)) { signal_deliver(regs); return; }
    if (!((int)regs->rdx & 0x40 /* O_CREAT */)) {   /* Этап 7: честный ENOENT */
      char p7[256]; uint32_t m7; uint64_t s7;
      lx_copy_path(regs->rsi, p7, sizeof(p7));
      if (lx_path_stat(p7, &m7, &s7) != 0) { regs->rax = LERR(L_ENOENT); signal_deliver(regs); return; }
    }
    regs->rdi = regs->rsi;                    /* path */
    regs->rsi = regs->rdx;                    /* flags */
    regs->rax = 90;                           /* -> SYS_OPEN (dirfd=AT_FDCWD игнор) */
    break;

  case 217:                                  /* getdents64(fd, void *dirp, size_t count) */
  {
    int fd = (int)regs->rdi;
    uint8_t *dirp = (uint8_t *)regs->rsi;
    uint64_t count = regs->rdx;
    uint64_t filled = 0;
    int err = 0;
    char name[160];
    int isd;
    stac();
    for (;;) {
      int saved = fd_dir_tell(fd);
      if (saved < 0) { err = 1; break; }       /* не каталог / плохой fd */
      int rc = fd_readdir(fd, name, sizeof(name), &isd);
      if (rc < 0) { err = 1; break; }
      if (rc == 0) break;                       /* конец каталога */

      int nlen = 0;
      while (name[nlen]) nlen++;
      /* struct linux_dirent64: d_ino@0, d_off@8, d_reclen@16, d_type@18,
       * d_name@19 (NUL-terminated). reclen выровнен на 8 байт. */
      uint64_t reclen = (uint64_t)(19 + nlen + 1);
      reclen = (reclen + 7) & ~7ULL;
      if (filled + reclen > count) {
        fd_dir_seek(fd, saved);                 /* откат: запись не влезла */
        break;
      }
      uint8_t *rec = dirp + filled;
      *(uint64_t *)(rec + 0)  = (uint64_t)(filled + 1);    /* d_ino (синтет.) */
      *(int64_t  *)(rec + 8)  = (int64_t)(filled + reclen);/* d_off */
      *(uint16_t *)(rec + 16) = (uint16_t)reclen;          /* d_reclen */
      rec[18] = isd ? 4 : 8;                                /* d_type: DT_DIR/DT_REG */
      for (int k = 0; k < nlen; k++) rec[19 + k] = (uint8_t)name[k];
      rec[19 + nlen] = '\0';
      filled += reclen;
    }
    clac();
    regs->rax = err ? LERR(L_EBADF) : filled;
    signal_deliver(regs);
    return;
  }

  /* ---- mmap: длина в rsi совпадает; родной возвращает 0 при ошибке ------ */
  case 9:                                     /* mmap(addr,len,prot,flags,fd,off) */
    regs->rax = 14;                           /* -> SYS_MMAP (использует только len) */
    syscall_handler(regs);
    if (regs->rax == 0) regs->rax = LERR(L_ENOMEM); /* Linux: MAP_FAILED как -errno */
    return;                                   /* сигналы уже доставлены */
  case 11:                                    /* munmap — родного освобождения нет */
    regs->rax = 0;
    signal_deliver(regs);
    return;

  /* ---- writev / readv: цикл по iovec ----------------------------------- */
  case 20:                                    /* writev(fd, iov, iovcnt) */
  {
    int fd = (int)regs->rdi;
    const struct l_iovec *iov = (const struct l_iovec *)regs->rsi;
    int cnt = (int)regs->rdx;
    int64_t total = 0; int had_err = 0;
    stac();
    for (int i = 0; i < cnt; i++) {
      const void *b = iov[i].iov_base;
      uint64_t l = iov[i].iov_len;
      if (l == 0) continue;
      int rc = fd_write(fd, b, (uint32_t)l);
      if (rc < 0) { had_err = 1; break; }
      total += rc;
      if ((uint64_t)rc < l) break;            /* частичная запись */
    }
    clac();
    regs->rax = (had_err && total == 0) ? LERR(L_EINVAL) : (uint64_t)total;
    signal_deliver(regs);
    return;
  }
  case 19:                                    /* readv(fd, iov, iovcnt) */
  {
    int fd = (int)regs->rdi;
    const struct l_iovec *iov = (const struct l_iovec *)regs->rsi;
    int cnt = (int)regs->rdx;
    int64_t total = 0; int had_err = 0;
    stac();
    for (int i = 0; i < cnt; i++) {
      void *b = (void *)iov[i].iov_base;
      uint64_t l = iov[i].iov_len;
      if (l == 0) continue;
      int rc = fd_read(fd, b, (uint32_t)l);
      if (rc < 0) { had_err = 1; break; }
      total += rc;
      if ((uint64_t)rc < l) break;            /* короткое чтение/EOF */
    }
    clac();
    regs->rax = (had_err && total == 0) ? LERR(L_EINVAL) : (uint64_t)total;
    signal_deliver(regs);
    return;
  }

  /* ---- nanosleep(req, rem): timespec -> миллисекунды -> SYS_SLEEP ------- */
  case 35:
  {
    const struct l_timespec *req = (const struct l_timespec *)regs->rdi;
    uint64_t ms = 0;
    if (req) { stac(); ms = (uint64_t)req->tv_sec * 1000 + (uint64_t)req->tv_nsec / 1000000ULL; clac(); }
    regs->rdi = ms;
    regs->rax = 13;                           /* SYS_SLEEP */
    syscall_handler(regs);                    /* доставит сигналы */
    regs->rax = 0;
    return;
  }

  /* ---- arch_prctl(code, addr): TLS через FS_BASE ----------------------- */
  case 158:
  {
    int code = (int)regs->rdi;
    uint64_t addr = regs->rsi;
    if (code == 0x1002) {                     /* ARCH_SET_FS */
      uint32_t lo = (uint32_t)addr, hi = (uint32_t)(addr >> 32);
      __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"(lo), "d"(hi));
      /* Этап 6b: ОБЯЗАТЕЛЬНО сохраняем FS_BASE в задаче, иначе schedule()
       * на следующем же тике перезапишет MSR старым current_task->fs_base
       * (значением с момента создания задачи) и затрёт TLS-указатель musl —
       * errno/TLS развалятся после первого переключения контекста. */
      current_task->fs_base = addr;
      regs->rax = 0;
    } else if (code == 0x1003) {              /* ARCH_GET_FS */
      uint32_t lo, hi;
      __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
      uint64_t v = ((uint64_t)hi << 32) | lo;
      if (addr) { stac(); *(uint64_t *)addr = v; clac(); }
      regs->rax = 0;
    } else {
      regs->rax = LERR(L_EINVAL);
    }
    signal_deliver(regs);
    return;
  }

  /* ---- clock_gettime(clk_id, struct timespec*) — Этап 6b-2 ------------- */
  case 228:
  {
    int clk = (int)regs->rdi;
    uint64_t uts = regs->rsi;                 /* user struct timespec {long sec; long nsec} */
    uint64_t ms = (uint64_t)tick;             /* PIT @ 1 kHz => мс с загрузки */
    int64_t sec, nsec = (int64_t)(ms % 1000) * 1000000;
    if (clk == 0 || clk == 5)                 /* CLOCK_REALTIME / _COARSE */
      sec = (int64_t)rtc_unix_time();
    else                                      /* MONOTONIC(1)/RAW(4)/COARSE(6)/BOOTTIME(7) */
      sec = (int64_t)(ms / 1000);
    if (uts) { stac(); ((int64_t *)uts)[0] = sec; ((int64_t *)uts)[1] = nsec; clac(); }
    regs->rax = 0;
    signal_deliver(regs);
    return;
  }

  /* ---- uname(struct utsname*) — Этап 6b-2 ------------------------------ */
  case 63:
  {
    uint64_t up = regs->rdi;
    if (up) {
      /* Linux struct utsname: 6 полей по 65 байт
       * (sysname, nodename, release, version, machine, domainname). */
      static const char *const uf[6] = {
        "EquinoxOS", "equinox", "1.0", "EquinoxOS 6b musl", "x86_64", "(none)"
      };
      stac();
      char *p = (char *)up;
      for (int f = 0; f < 6; f++) {
        char *dst = p + f * 65;
        const char *s = uf[f];
        int i = 0;
        for (; s[i] && i < 64; i++) dst[i] = s[i];
        for (; i < 65; i++) dst[i] = 0;
      }
      clac();
    }
    regs->rax = 0;
    signal_deliver(regs);
    return;
  }

  /* ---- stat-семейство (Этап 6c): Linux struct stat (kstat layout) ------ */
  case 5:                                     /* fstat(fd, statbuf) */
  {
    uint32_t mode; uint64_t size;
    if (lx_fd_stat((int)regs->rdi, &mode, &size) == 0) {
      lx_fill_stat(regs->rsi, mode, size); regs->rax = 0;
    } else regs->rax = LERR(L_EBADF);
    signal_deliver(regs);
    return;
  }
  case 4:                                     /* stat(path, statbuf) */
  case 6:                                     /* lstat — символ. ссылок нет, как stat */
  {
    char path[256];
    lx_copy_path(regs->rdi, path, sizeof(path));
    uint32_t mode; uint64_t size;
    if (lx_path_stat(path, &mode, &size) == 0) {
      lx_fill_stat(regs->rsi, mode, size); regs->rax = 0;
    } else regs->rax = LERR(L_ENOENT);
    signal_deliver(regs);
    return;
  }
  case 262:                                   /* newfstatat(dirfd, path, statbuf, flag) */
  {
    const char *pp = (const char *)regs->rsi;
    char c = 0;
    if (pp) { stac(); c = pp[0]; clac(); }
    int empty = (pp == 0 || c == '\0');
    uint32_t mode; uint64_t size;
    if (((int)regs->r10 & L_AT_EMPTY_PATH) || empty) {  /* AT_EMPTY_PATH -> fstat(dirfd) */
      if (lx_fd_stat((int)regs->rdi, &mode, &size) == 0) {
        lx_fill_stat(regs->rdx, mode, size); regs->rax = 0;
      } else regs->rax = LERR(L_EBADF);
    } else {
      char path[256];
      lx_copy_path(regs->rsi, path, sizeof(path));
      if (lx_path_stat(path, &mode, &size) == 0) {
        lx_fill_stat(regs->rdx, mode, size); regs->rax = 0;
      } else regs->rax = LERR(L_ENOENT);
    }
    signal_deliver(regs);
    return;
  }

  /* ---- access / faccessat (Этап 6c): 0 если существует, иначе -ENOENT --- */
  case 21:                                    /* access(path, mode) */
  {
    char path[256];
    lx_copy_path(regs->rdi, path, sizeof(path));
    uint32_t m; uint64_t s;
    regs->rax = (lx_path_stat(path, &m, &s) == 0) ? 0 : LERR(L_ENOENT);
    signal_deliver(regs);
    return;
  }
  case 89:                                    /* readlink(path, buf, sz) — Этап 8 */
  case 267:                                   /* readlinkat(dirfd, path, buf, sz) */
  {
    /* Симлинков у ext2-драйвера пока нет: существующий путь -> EINVAL
     * («не симлинк», как в Linux), несуществующий -> ENOENT. musl/bash
     * корректно переживают оба ответа (realpath, canonicalize). */
    char path[256];
    lx_copy_path((n == 89) ? regs->rdi : regs->rsi, path, sizeof(path));
    uint32_t m; uint64_t s;
    regs->rax = (lx_path_stat(path, &m, &s) == 0) ? LERR(L_EINVAL) : LERR(L_ENOENT);
    signal_deliver(regs);
    return;
  }

  case 269:                                   /* faccessat(dirfd, path, mode, flag) */
  case 439:                                   /* faccessat2 — то же + flags (Этап 7: bash) */
  {
    char path[256];
    lx_copy_path(regs->rsi, path, sizeof(path));
    uint32_t m; uint64_t s;
    regs->rax = (lx_path_stat(path, &m, &s) == 0) ? 0 : LERR(L_ENOENT);
    signal_deliver(regs);
    return;
  }

  /* ---- мелкие совместимостные заглушки --------------------------------- */
  case 218: regs->rax = (uint64_t)current_task->id; signal_deliver(regs); return; /* set_tid_address */
  case 102: case 104: case 107: case 108:     /* getuid/getgid/geteuid/getegid */
    regs->rax = 0; signal_deliver(regs); return;                                  /* «root» */
  case 110: regs->rax = 1; signal_deliver(regs); return;                          /* getppid */
  case 186: regs->rax = (uint64_t)current_task->id; signal_deliver(regs); return; /* gettid (==pid, однопоточно) */

  /* ---- Этап 7 (bash): честный fcntl ------------------------------------ */
  case 72: { /* fcntl(fd, cmd, arg) */
    int fd  = (int)regs->rdi;
    int cmd = (int)regs->rsi;
    long arg = (long)regs->rdx;
    int kind = 0; uint32_t fsz = 0;
    int valid = (fd_statx(fd, &kind, &fsz) == 0);
    switch (cmd) {
      case 0:     /* F_DUPFD */
      case 1030: { /* F_DUPFD_CLOEXEC (CLOEXEC не отслеживаем — exec'и наши) */
        if (!valid) { regs->rax = LERR(L_EBADF); break; }
        int nfd = fd_dup_from(fd, (int)arg);
        regs->rax = (nfd < 0) ? LERR(L_EMFILE) : (uint64_t)nfd;
        break;
      }
      case 1:  /* F_GETFD */
        regs->rax = valid ? 0 : LERR(L_EBADF);   /* FD_CLOEXEC не храним */
        break;
      case 2:  /* F_SETFD — принять и забыть (наш execve fd не наследует «лишних») */
        regs->rax = valid ? 0 : LERR(L_EBADF);
        break;
      case 3:  /* F_GETFL */
        if (!valid) { regs->rax = LERR(L_EBADF); break; }
        switch (kind) {
          case OFD_CONSOLE: regs->rax = 2; break;  /* O_RDWR */
          case OFD_PIPE_W:  regs->rax = 1; break;  /* O_WRONLY */
          default:          regs->rax = 0; break;  /* O_RDONLY */
        }
        break;
      case 4:  /* F_SETFL — O_NONBLOCK/O_APPEND игнорируем (документировано) */
        regs->rax = valid ? 0 : LERR(L_EBADF);
        break;
      default:
        regs->rax = LERR(L_EINVAL);
        break;
    }
    signal_deliver(regs); return;
  }

  /* ---- Этап 7 (bash): umask / rlimit / times / группы ------------------- */
  case 95: { /* umask(mask) — глобально (процессную маску пока не различаем) */
    static uint32_t k_umask = 022;
    uint32_t old = k_umask;
    k_umask = (uint32_t)regs->rdi & 0777u;
    regs->rax = old;
    signal_deliver(regs); return;
  }

  case 97:   /* getrlimit(res, rlim*) */
  case 302: { /* prlimit64(pid, res, new*, old*) — new игнорируем */
    int res = (int)((n == 97) ? regs->rdi : regs->rsi);
    uint64_t uptr = (n == 97) ? regs->rsi : regs->r10;
    if (uptr) {
      uint64_t cur, max;
      if (res == 7 /* RLIMIT_NOFILE */) { cur = max = FD_MAX; }
      else { cur = max = ~0ULL; }      /* RLIM_INFINITY */
      stac();
      ((uint64_t *)uptr)[0] = cur;
      ((uint64_t *)uptr)[1] = max;
      clac();
    }
    regs->rax = 0;
    signal_deliver(regs); return;
  }

  case 160:  /* setrlimit — принять и игнорировать */
    regs->rax = 0; signal_deliver(regs); return;

  case 100: { /* times(struct tms*) -> clock ticks (PIT 1 кГц == CLK_TCK 100? нет:
               * musl __SC_CLK_TCK = 100, поэтому отдаём тики/10). Поля tms —
               * нули: учёта cpu-времени у ядра пока нет, честный минимум. */
    uint64_t t10 = (uint64_t)tick / 10;
    if (regs->rdi) {
      stac();
      ((int64_t *)regs->rdi)[0] = 0; ((int64_t *)regs->rdi)[1] = 0;
      ((int64_t *)regs->rdi)[2] = 0; ((int64_t *)regs->rdi)[3] = 0;
      clac();
    }
    regs->rax = t10;
    signal_deliver(regs); return;
  }

  case 115:  /* getgroups(size, list*) — дополнительных групп нет */
    regs->rax = 0; signal_deliver(regs); return;

  case 96: { /* gettimeofday(tv*, tz*) */
    if (regs->rdi) {
      int64_t sec  = (int64_t)rtc_unix_time();
      int64_t usec = (int64_t)((uint64_t)tick % 1000) * 1000;
      stac();
      ((int64_t *)regs->rdi)[0] = sec;
      ((int64_t *)regs->rdi)[1] = usec;
      clac();
    }
    regs->rax = 0;
    signal_deliver(regs); return;
  }

  /* ---- Этап 7 (bash/readline): poll / select / pselect6 ----------------- */
  case 7: { /* poll(pollfd*, nfds, timeout_ms) */
    uint64_t ufds = regs->rdi;
    int nfds = (int)regs->rsi;
    int64_t timeout = (int64_t)(int32_t)regs->rdx;   /* мс; <0 = бесконечно */
    uint64_t deadline = (timeout >= 0) ? (uint64_t)tick + (uint64_t)timeout : 0;
    int nready;
    for (;;) {
      nready = 0;
      stac();
      for (int i = 0; i < nfds; i++) {
        /* struct pollfd { int fd; short events; short revents; } — 8 байт */
        int32_t  pfd = *(int32_t *)(ufds + (uint64_t)i * 8);
        int16_t  ev  = *(int16_t *)(ufds + (uint64_t)i * 8 + 4);
        int16_t  rev = 0;
        if (pfd >= 0) {
          int rin = fd_ready_in(pfd);
          if (rin < 0) rev |= 0x020;                       /* POLLNVAL */
          else {
            if ((ev & 0x001) && rin) rev |= 0x001;          /* POLLIN  */
            if (ev & 0x004) rev |= 0x004;                   /* POLLOUT: пишем без блокировки */
          }
        }
        *(int16_t *)(ufds + (uint64_t)i * 8 + 6) = rev;
        if (rev) nready++;
      }
      clac();
      if (nready > 0 || timeout == 0) break;
      if (timeout > 0 && (uint64_t)tick >= deadline) break;
      uint64_t deliverable = current_task->sig_pending & ~current_task->sig_blocked;
      if (deliverable) { regs->rax = LERR(L_EINTR); signal_deliver(regs); return; }
      yield();
    }
    regs->rax = (uint64_t)nready;
    signal_deliver(regs); return;
  }

  case 23:   /* select(nfds, r*, w*, e*, timeval*) */
  case 270: { /* pselect6(nfds, r*, w*, e*, timespec*, sigmask) — маску игнорируем */
    int nfds = (int)regs->rdi;
    uint64_t urd = regs->rsi, uwr = regs->rdx, uex = regs->r10, uts = regs->r8;
    if (nfds < 0 || nfds > FD_MAX) nfds = (nfds < 0) ? 0 : FD_MAX;
    int64_t timeout = -1;                            /* мс; <0 = бесконечно */
    if (uts) {
      stac();
      int64_t a = ((int64_t *)uts)[0];
      int64_t b = ((int64_t *)uts)[1];
      clac();
      timeout = (n == 23) ? a * 1000 + b / 1000      /* timeval: usec */
                          : a * 1000 + b / 1000000;  /* timespec: nsec */
      if (timeout < 0) timeout = 0;
    }
    uint64_t deadline = (timeout >= 0) ? (uint64_t)tick + (uint64_t)timeout : 0;
    uint64_t rd = 0, wr = 0, ex = 0;
    stac();
    if (urd) rd = *(uint64_t *)urd;                  /* fd < 64 => одно слово */
    if (uwr) wr = *(uint64_t *)uwr;
    if (uex) ex = *(uint64_t *)uex;
    clac();
    (void)ex;
    uint64_t ord, owr;
    int nready;
    for (;;) {
      ord = 0; owr = 0; nready = 0;
      for (int fd = 0; fd < nfds && fd < 64; fd++) {
        uint64_t bit = 1ULL << fd;
        if ((rd & bit) && fd_ready_in(fd) > 0) { ord |= bit; nready++; }
        if (wr & bit)                           { owr |= bit; nready++; }
      }
      if (nready > 0 || timeout == 0) break;
      if (timeout > 0 && (uint64_t)tick >= deadline) break;
      uint64_t deliverable = current_task->sig_pending & ~current_task->sig_blocked;
      if (deliverable) { regs->rax = LERR(L_EINTR); signal_deliver(regs); return; }
      yield();
    }
    stac();
    if (urd) { *(uint64_t *)urd = ord; for (int w = 1; w < 16; w++) ((uint64_t *)urd)[w] = 0; }
    if (uwr) { *(uint64_t *)uwr = owr; for (int w = 1; w < 16; w++) ((uint64_t *)uwr)[w] = 0; }
    if (uex) { for (int w = 0; w < 16; w++) ((uint64_t *)uex)[w] = 0; }
    clac();
    regs->rax = (uint64_t)nready;
    signal_deliver(regs); return;
  }

  /* ---- Этап 6d: настоящие сигналы (rt_sigaction / rt_sigprocmask) ------- */
  case 13: {  /* rt_sigaction(sig, act*, oldact*, sigsetsize) */
    int sig = (int)regs->rdi;
    /* act==NULL → только прочитать прежний обработчик (SIG_QUERY). */
    uint64_t handler = 0xFFFFFFFFFFFFFFFFULL, restorer = 0;
    if (regs->rsi) {
      struct l_sigaction act;
      stac(); act = *(const struct l_sigaction *)regs->rsi; clac();
      handler  = act.sa_handler;
      restorer = (act.sa_flags & L_SA_RESTORER) ? act.sa_restorer : 0;
    }
    uint64_t old = 0;
    int rc = signal_setaction(sig, handler, restorer, &old);
    if (rc != 0) { regs->rax = LERR(L_EINVAL); signal_deliver(regs); return; }
    if (regs->rdx) {  /* вернуть прежний обработчик в oldact */
      struct l_sigaction oa;
      oa.sa_handler  = old;
      oa.sa_flags    = L_SA_RESTORER;
      oa.sa_restorer = current_task ? current_task->sig_restorer : 0;
      oa.sa_mask     = 0;
      stac(); *(struct l_sigaction *)regs->rdx = oa; clac();
    }
    regs->rax = 0; signal_deliver(regs); return;
  }
  case 14: {  /* rt_sigprocmask(how, set*, oldset*, sigsetsize) — Linux передаёт
               * set/oldset УКАЗАТЕЛЯМИ (натив ждёт значение), поэтому inline.
               * ВАЖНО: Linux sigset кладёт сигнал N в бит N-1, а наше ядро
               * (signal.c SIGBIT=1<<s) — в бит N. Конвертируем сдвигом. */
    uint64_t old = 0;
    int how = (int)regs->rdi;
    if (regs->rsi) {  /* есть новый набор — применить */
      uint64_t lset;
      stac(); lset = *(const uint64_t *)regs->rsi; clac();
      signal_procmask(how, lset << 1 /* linux->native */, &old);
    } else {          /* set==NULL — только прочитать текущую маску */
      signal_procmask(0 /*BLOCK*/, 0, &old);
    }
    if (regs->rdx) {
      uint64_t lold = old >> 1;  /* native->linux */
      stac(); *(uint64_t *)regs->rdx = lold; clac();
    }
    regs->rax = 0; signal_deliver(regs); return;
  }

  /* ---- Этап 6e: ожидание потомков + группы процессов (job control) ----- */
  case 61: {  /* wait4(pid, int *status, options, rusage*) */
    int raw = 0;
    int nohang = (int)(regs->rdx & 1 /* WNOHANG */);
    int64_t r = task_waitpid_ex(regs->rdi, &raw, nohang);
    if (r == 0 && nohang) {
      /* WNOHANG: дети есть, но ни один не завершился → 0, status не трогаем. */
      regs->rax = 0;
    } else if (r < 0) {
      /* task_waitpid отдаёт -1 и при «нет детей», и при прерывании сигналом.
       * Разбираем как Linux: есть доставляемый сигнал (кроме SIGCHLD) → EINTR,
       * иначе ECHILD. (rusage игнорируется.) */
      uint64_t deliverable = current_task->sig_pending &
                             ~current_task->sig_blocked & ~(1ULL << 17 /*SIGCHLD*/);
      regs->rax = deliverable ? LERR(L_EINTR) : LERR(L_ECHILD);
    } else {
      if (regs->rsi) {
        /* Кодируем Linux wait-status. Нормальный выход: WIFEXITED → биты 0..6
         * == 0, WEXITSTATUS == (status>>8)&0xff. (Сигнальную смерть ядро пока
         * не различает — отдаём как обычный выход с кодом.) */
        int wstatus = (raw & 0xff) << 8;
        stac(); *(int *)regs->rsi = wstatus; clac();
      }
      regs->rax = (uint64_t)r;
    }
    signal_deliver(regs); return;
  }

  case 111:   /* getpgrp() -> группа текущего процесса */
    regs->rax = current_task->pgid ? current_task->pgid : current_task->id;
    signal_deliver(regs); return;

  case 121: { /* getpgid(pid) -> группа процесса pid (0 == текущий) */
    uint64_t pid = regs->rdi;
    task_t *t = pid ? task_by_id(pid) : current_task;
    if (!t) { regs->rax = LERR(L_ESRCH); signal_deliver(regs); return; }
    regs->rax = t->pgid ? t->pgid : t->id;
    signal_deliver(regs); return;
  }

  case 124:   /* getsid(pid) — сессий не различаем, отдаём группу как прокси */
    regs->rax = current_task->pgid ? current_task->pgid : current_task->id;
    signal_deliver(regs); return;

  case 109: { /* setpgid(pid, pgid): pid 0=текущий, pgid 0=сделать pid лидером */
    uint64_t pid  = regs->rdi ? regs->rdi : current_task->id;
    uint64_t pgid = regs->rsi;
    task_t *t = (pid == current_task->id) ? current_task : task_by_id(pid);
    if (!t) { regs->rax = LERR(L_ESRCH); signal_deliver(regs); return; }
    /* Менять можно только себя или своего ребёнка (POSIX). */
    if (t != current_task && t->parent_id != current_task->id) {
      regs->rax = LERR(L_EPERM); signal_deliver(regs); return;
    }
    t->pgid = pgid ? pgid : t->id;
    regs->rax = 0; signal_deliver(regs); return;
  }

  case 112:   /* setsid(): новая сессия/группа — текущий становится лидером */
    current_task->pgid = current_task->id;
    regs->rax = current_task->id;
    signal_deliver(regs); return;

  default: {
    /* Этап 7: каждый незнакомый Linux-сисколл светим в COM1 — так дыры под
     * bash/новые порты видны сразу, а не как «тихий» сбой юзерспейса. */
    char eb[64];
    sprintf(eb, "[lx] ENOSYS syscall %u (pid %u)\n",
            (unsigned)n, (unsigned)(current_task ? current_task->id : 0));
    serial_puts(COM1, eb);
    regs->rax = LERR(L_ENOSYS);
    signal_deliver(regs);
    return;
  }
  }

  /* делегируем в родной диспетчер (он доставит сигналы и вернётся в ring3) */
  syscall_handler(regs);
}

/* ----- capture-sink для SYS_SHELL_EXEC ------------------------------------
 * Один-единственный буфер, используемый и case 73 выше, и самим sink'ом.
 * Не thread-safe — но syscall_handler у нас и так не reentrant. */
char shell_capture_buf[2048];
int shell_capture_pos = 0;
const int shell_capture_cap = (int)sizeof(shell_capture_buf);

void shell_capture_sink(const char *s)
{
  if (!s)
    return;
  while (*s && shell_capture_pos < shell_capture_cap - 1)
  {
    shell_capture_buf[shell_capture_pos++] = *s++;
  }
  shell_capture_buf[shell_capture_pos] = '\0';
}