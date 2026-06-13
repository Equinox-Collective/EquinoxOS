#include "task.h"
#include "../core/gdt.h"
#include "../mem/pmm.h"
#include "../mem/memory.h"
#include "../fs/elf.h"
#include "../mem/vmm.h"
#include "../../syslibc/stdio.h"
#include "../../syslibc/string.h"
#include "../fs/vfs.h"
#include "../fs/fd.h"
#include "../misc/random.h"   /* Этап 6b: rdrand_bytes для AT_RANDOM */
#include "signal.h"
#include <stdint.h>

extern void term_print(const char *str);

#define IA32_FS_BASE_MSR 0xC0000100
#define IA32_GS_BASE_MSR 0xC0000101

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

task_t* current_task = NULL;
static task_t* task_list = NULL;
static uint64_t next_pid = 1;
extern uint64_t hhdm_offset;
extern volatile uint32_t tick;
static uint64_t kernel_cr3 = 0;

/* --- Этап 9: FPU/SSE-контекст ---------------------------------------------
 * Эталонное чистое состояние (fninit + дефолтный MXCSR) снимается один раз
 * лениво: текущее состояние сохраняем, инициализируем FPU, снимаем слепок,
 * возвращаем как было. Новые задачи стартуют с копией слепка. */
static uint8_t fpu_clean_state[512] __attribute__((aligned(16)));
static bool    fpu_clean_ready = false;

void task_fpu_init_area(task_t* t) {
    if (!fpu_clean_ready) {
        uint8_t tmp[512 + 15];
        void* live = (void*)(((uint64_t)tmp + 15) & ~(uint64_t)15);
        __asm__ volatile("fxsave64 (%0)" :: "r"(live) : "memory");
        __asm__ volatile("fninit");
        uint32_t mxcsr = 0x1F80; /* default: все SSE-исключения замаскированы */
        __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
        __asm__ volatile("fxsave64 (%0)" :: "r"(fpu_clean_state) : "memory");
        __asm__ volatile("fxrstor64 (%0)" :: "r"(live) : "memory");
        fpu_clean_ready = true;
    }
    memcpy(task_fpu_area(t), fpu_clean_state, 512);
}

void task_init() {
    __asm__ volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
    current_task = (task_t*)kmalloc(sizeof(task_t));
    memset(current_task, 0, sizeof(task_t));
    task_fpu_init_area(current_task);
    current_task->cr3 = kernel_cr3;
    current_task->id = next_pid++;
    current_task->running = true;

    current_task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384; 
    /* Этап 2: таблица дескрипторов init-процесса (0/1/2 = консоль). */
    current_task->fdt = fd_table_create();
    /* Этап 3: рабочая директория по умолчанию — корень. */
    current_task->cwd[0] = '/'; current_task->cwd[1] = '\0';
    /* Этап 4: сигнальное состояние (всё SIG_DFL, маски пусты). */
    task_signal_init(current_task);

    current_task->next = current_task;
    task_list = current_task;
}

/* Этап 2: таблица дескрипторов текущего процесса (используется fs/fd.c). */
struct fd_table *task_current_fdt(void) {
    return current_task ? current_task->fdt : NULL;
}

/* ===== Этап 3: рабочая директория (cwd) ================================= */

const char *task_current_cwd(void) {
    if (current_task && current_task->cwd[0]) return current_task->cwd;
    return "/";
}

int task_chdir(const char *path) {
    if (!current_task || !path) return -1;
    char norm[256];
    if (vfs_normalize(current_task->cwd, path, norm, sizeof(norm)) < 0) return -1;
    if (!vfs_dir_exists(norm)) return -1;     /* нет такого каталога */
    int i = 0;
    for (; i < (int)sizeof(current_task->cwd) - 1 && norm[i]; i++)
        current_task->cwd[i] = norm[i];
    current_task->cwd[i] = '\0';
    return 0;
}

void task_resolve_fs_path(const char *in, char *out, int outsz) {
    if (!out || outsz < 1) return;
    const char *cwd = task_current_cwd();
    char norm[256];
    if (vfs_normalize(cwd, in, norm, sizeof(norm)) < 0) {
        /* фолбэк: копируем как есть (без ведущего '/') */
        int j = 0; const char *p = in ? in : "";
        while (*p == '/') p++;
        for (; p[j] && j < outsz - 1; j++) out[j] = p[j];
        out[j] = '\0';
        return;
    }
    /* norm — абсолютный ("/bin/foo"); для плоского ext2 убираем ведущий '/'. */
    const char *p = norm;
    while (*p == '/') p++;
    int j = 0;
    for (; p[j] && j < outsz - 1; j++) out[j] = p[j];
    out[j] = '\0';
}

task_t* task_create_full(void (*entry)(), uint64_t arg1, uint64_t arg2,
                         uint64_t cr3, const sysv_args_t *sv) {
  task_t *new_task = (task_t *)kmalloc(sizeof(task_t));
  memset(new_task, 0, sizeof(task_t));
  task_fpu_init_area(new_task); /* Этап 9: чистый FPU/SSE-контекст */
  void *top_stack_phys = 0; /* Этап 6b: phys верхней страницы user-стека */
  new_task->brk = 0x40000000;
  new_task->id = next_pid++;
  new_task->running = true;
  new_task->cr3 = cr3;
  /* Этап 2: своя таблица дескрипторов (0/1/2 = консоль). */
  new_task->fdt = fd_table_create();
  /* Этап 3: cwd по умолчанию — корень. */
  new_task->cwd[0] = '/'; new_task->cwd[1] = '\0';
  /* Этап 4: сигнальное состояние (всё SIG_DFL). */
  task_signal_init(new_task);

  // 1. Ядерный стек (для прерываний)
  void *kstack_phys = pmm_alloc_continuous(4);
  new_task->kstack_at_bottom = (uint64_t)kstack_phys + hhdm_offset + 16384;

  stack_frame_t *frame =
      (stack_frame_t *)(new_task->kstack_at_bottom - sizeof(stack_frame_t));
  memset(frame, 0, sizeof(stack_frame_t));

  // 2. Пользовательский стек (8 МБ)
  if (cr3 != 0) {
    uint64_t stack_top = 0x70000000000; // Верхушка стека
    uint64_t stack_pages = 2048;        // 8 МБ

    // Мапим страницы ПО ОДНОЙ. Так мы не зависим от фрагментации RAM.
    for (uint64_t i = 0; i < stack_pages; i++) {
      // Мапим страницы ПЕРЕД stack_top (т.к. стек растет вниз)
      uint64_t vaddr = stack_top - (stack_pages * 4096) + (i * 4096);
      void *phys = pmm_alloc(); // Берем любую свободную страницу

      if (!phys) {
        term_print("TASK: KERNEL OUT OF RAM DURING STACK ALLOC!\n");
        while (1)
          ;
      }

      vmm_map((page_table_t *)VIRT(cr3), vaddr, (uint64_t)phys,
              PTE_PRESENT | PTE_USER | PTE_WRITABLE);

      // Обнуляем страницу (через HHDM), чтобы не было мусора
      memset((void *)VIRT(phys), 0, 4096);

      // Этап 6b: запоминаем верхнюю страницу — на ней строим SysV-кадр.
      if (vaddr == stack_top - 4096) top_stack_phys = phys;
    }

    // TLS...
    uint64_t tls_virt = 0x60000000000;
    void *tls_phys = pmm_alloc();
    vmm_map((page_table_t *)VIRT(cr3), tls_virt, (uint64_t)tls_phys,
            PTE_USER | PTE_WRITABLE | PTE_PRESENT);
    memset((void *)VIRT(tls_phys), 0, 4096);
    ((uint64_t *)VIRT(tls_phys))[0] = tls_virt + 64;
    new_task->fs_base = tls_virt;

    // ВАЖНО: Выставляем RSP на самый верх выделенной области
    frame->rsp = stack_top - 16;
  }

  // 3. Настройка фрейма
  frame->rip = (uint64_t)entry;
  frame->rdi = arg1;
  frame->rsi = arg2;
  frame->rflags = 0x202; // Прерывания включены

  if (cr3 == 0) {
    frame->cs = 0x08;
    frame->ss = 0x10;
    frame->rsp = (uint64_t)kmalloc(16384) + 16384;
  } else {
    frame->cs = 0x23;
    frame->ss = 0x1B;
  }

  /* --- Этап 6b: System V AMD64 initial stack (для musl / Linux-ABI). --------
   * Строим ДО вставки задачи в кольцо планировщика, чтобы не было гонки с
   * переключением контекста. Кадр кладётся на ВЕРШИНУ user-стека:
   *   rsp -> argc, argv[0..n-1], NULL, envp[0..m-1], NULL, auxv..., AT_NULL.
   * Строки argv/envp НЕ копируем — argv_user/envp_user уже указывают на них
   * (argv-страница 0xB0000000). Родной crt0 читает argc/argv из регистров и
   * этот кадр игнорирует, поэтому существующие приложения не ломаются. */
  if (sv && cr3 != 0 && top_stack_phys) {
    uint64_t stack_top = 0x70000000000ULL;
    uint64_t base_user = stack_top - 4096;
    uint8_t *topk = (uint8_t *)VIRT(top_stack_phys);
    #define U2K(u) ((void *)(topk + ((uint64_t)(u) - base_user)))
    uint64_t sp = stack_top;
    int argc = sv->argc, envc = sv->envc;

    /* 16 случайных байт для AT_RANDOM (stack canary / TLS у musl). */
    sp -= 16;
    rdrand_bytes(U2K(sp), 16);
    uint64_t at_random = sp;
    sp &= ~0xFULL;

    uint64_t aux[32]; int an = 0;
    if (sv->at_phdr) {
      aux[an++] = 3;  aux[an++] = sv->at_phdr;     /* AT_PHDR  */
      aux[an++] = 4;  aux[an++] = sv->at_phent;    /* AT_PHENT */
      aux[an++] = 5;  aux[an++] = sv->at_phnum;    /* AT_PHNUM */
    }
    aux[an++] = 6;  aux[an++] = 4096;              /* AT_PAGESZ */
    aux[an++] = 9;  aux[an++] = sv->at_entry;      /* AT_ENTRY  */
    aux[an++] = 11; aux[an++] = 0;                 /* AT_UID    */
    aux[an++] = 12; aux[an++] = 0;                 /* AT_EUID   */
    aux[an++] = 13; aux[an++] = 0;                 /* AT_GID    */
    aux[an++] = 14; aux[an++] = 0;                 /* AT_EGID   */
    aux[an++] = 23; aux[an++] = 0;                 /* AT_SECURE */
    aux[an++] = 17; aux[an++] = 100;               /* AT_CLKTCK */
    aux[an++] = 25; aux[an++] = at_random;         /* AT_RANDOM */
    aux[an++] = 0;  aux[an++] = 0;                 /* AT_NULL   */

    int slots = 1 + (argc + 1) + (envc + 1) + an;
    if (slots & 1) sp -= 8;          /* финальный rsp кратен 16 */
    sp -= (uint64_t)slots * 8;
    if (sp >= base_user) {           /* кадр поместился в верхнюю страницу */
      uint64_t *w = (uint64_t *)U2K(sp);
      int idx = 0;
      w[idx++] = (uint64_t)argc;
      for (int i = 0; i < argc; i++) w[idx++] = sv->argv_user[i];
      w[idx++] = 0;
      for (int i = 0; i < envc; i++) w[idx++] = sv->envp_user[i];
      w[idx++] = 0;
      for (int i = 0; i < an; i++)   w[idx++] = aux[i];
      frame->rsp = sp;               /* ring-3 стартует уже с SysV-кадром */
    }
    #undef U2K
  }

  new_task->rsp = (uint64_t)frame;
  // Защита от поломанного ring-а: если task_list ещё не проинициализирован
  // (теоретически не должно случаться, т.к. task_init выполняется первым),
  // делаем новую задачу самоссылочной — так schedule() не уйдёт в NULL-deref.
  if (!task_list) {
    new_task->next = new_task;
    task_list = new_task;
  } else {
    new_task->next = task_list->next ? task_list->next : task_list;
    task_list->next = new_task;
  }
  return new_task;
}

/* Тонкая обёртка для существующих вызовов (без SysV-кадра). */
void task_create(void (*entry)(), uint64_t arg1, uint64_t arg2, uint64_t cr3) {
  task_create_full(entry, arg1, arg2, cr3, 0);
}

// task.c
uint64_t schedule(uint64_t current_rsp) {
    if (!current_task) return current_rsp;

    current_task->rsp = current_rsp;

    task_t* start = current_task;
    int hops = 0;
    do {
        task_t* next = current_task->next;
        if (!next) {
            current_task = task_list ? task_list : start;
        } else {
            current_task = next;
        }

        // Если время сна вышло, сбрасываем таймер сна
        if (current_task->sleep_until != 0 && tick >= current_task->sleep_until) {
            current_task->sleep_until = 0;
        }

        if (++hops > 4096) {
            current_task = (task_list && task_list->running) ? task_list : start;
            break;
        }
    // Крутимся в цикле, если задача не готова к работе ИЛИ всё ещё спит
    } while (!current_task->running || current_task->sleep_until != 0);

    /* Этап 9: переключение FPU/SSE-контекста. Без этого XMM/x87-регистры
     * «протекали» между задачами (см. комментарий к fpu_state в task.h). */
    if (current_task != start) {
        __asm__ volatile("fxsave64 (%0)"  :: "r"(task_fpu_area(start))        : "memory");
        __asm__ volatile("fxrstor64 (%0)" :: "r"(task_fpu_area(current_task)) : "memory");
    }

    uint64_t new_cr3 = (current_task->cr3 == 0) ? kernel_cr3 : current_task->cr3;
    __asm__ volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");

    gdt_set_tss_stack(current_task->kstack_at_bottom);
    
    if (current_task->fs_base != 0) {
        wrmsr(IA32_FS_BASE_MSR, current_task->fs_base);
    }
    
    return current_task->rsp;
}

void yield(void) {
    __asm__ volatile ("int $32");
}

bool task_exec(char* full_command) {
    int argc = 0;
    char* argv[16]; 
    
    char* cmd_copy = (char*)kmalloc(strlen(full_command) + 1);
    strcpy(cmd_copy, full_command);

    char* token = strtok(cmd_copy, " ");
    while (token != NULL && argc < 16) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }

    if (argc == 0) {
        kfree(cmd_copy);
        return false;
    }

    uint32_t elf_size = 0;
    uint8_t* elf_raw = vfs_read_file(argv[0], &elf_size);

    if (!elf_raw) {
        term_print("EXEC: File not found on any disk: ");
        term_print(argv[0]);
        term_print("\n");
        kfree(cmd_copy);
        return false;
    }

    Elf64_Ehdr* header = (Elf64_Ehdr*)elf_raw;
    if (memcmp(header->e_ident, "\x7f\x45\x4c\x46", 4) != 0) {
        term_print("EXEC: Not a valid ELF file\n");
        kfree(elf_raw);
        kfree(cmd_copy);
        return false;
    }

    // DEBUG: Dump first 16 bytes of ELF
    term_print("EXEC: ELF Header bytes: ");
    for(int i=0; i<16; i++) {
        char h[4];
        sprintf(h, "%x ", elf_raw[i]);
        term_print(h);
    }
    term_print("\n");
    page_table_t* proc_pml4 = vmm_create_address_space();
    uint64_t phys_pml4 = PHYS(proc_pml4);
    Elf64_Phdr* phdr = (Elf64_Phdr*)(elf_raw + header->e_phoff);
    /* Этап 6b: адрес таблицы Program Headers в памяти процесса (AT_PHDR для
     * musl — он ищет PT_TLS). Берём PT_LOAD, который перекрывает e_phoff. */
    uint64_t phdr_user = 0;
    uint64_t phsz = (uint64_t)header->e_phnum * header->e_phentsize;
    for (int i = 0; i < header->e_phnum; i++) {
        if (phdr[i].p_type == 1) { // PT_LOAD
            uint64_t vaddr = phdr[i].p_vaddr;
            uint64_t memsz = phdr[i].p_memsz;
            uint64_t filesz = phdr[i].p_filesz;
            uint64_t offset = phdr[i].p_offset;
            if (phdr_user == 0 &&
                offset <= header->e_phoff &&
                header->e_phoff + phsz <= offset + phdr[i].p_filesz)
                phdr_user = vaddr + (header->e_phoff - offset);

            // DEBUG: Segment details
            char log[128];
            sprintf(log, "EXEC: Segment %d: vaddr=%x, filesz=%x, memsz=%x, offset=%x\n", i, (uint32_t)vaddr, (uint32_t)filesz, (uint32_t)memsz, (uint32_t)offset);
            term_print(log);

            if (offset + filesz > elf_size) {
                term_print("EXEC: Segment offset/size out of bounds!\n");
                filesz = (offset < elf_size) ? (elf_size - offset) : 0;
            }

            // --- РАСЧЕТ СМЕЩЕНИЙ ---
            uint64_t page_offset = vaddr & 0xFFF;
            uint64_t base_vaddr = vaddr & ~0xFFF;
            uint64_t total_memsz = memsz + page_offset;
            uint64_t num_pages = (total_memsz + 4095) / 4096;

            // 1. Выделяем физическую память под нужной количество страниц
            void* phys_mem = pmm_alloc_continuous(num_pages);
            if (!phys_mem) {
                // OOM или фрагментация: без проверки мы бы замапили
                // юзеру physical 0..N (BIOS/IDT/etc) и обнулили низкую
                // память через HHDM — гарантированный BSOD при следующем IRQ.
                term_print("EXEC: pmm_alloc_continuous failed (OOM/fragmentation)\n");
                kfree(elf_raw);
                kfree(cmd_copy);
                return false;
            }

            // 2. Мапим страницы в пространство процесса
            for (uint64_t p = 0; p < num_pages; p++) {
                vmm_map(proc_pml4, 
                        base_vaddr + (p * 4096), 
                        (uint64_t)phys_mem + (p * 4096), 
                        PTE_PRESENT | PTE_USER | PTE_WRITABLE);
            }

            // 3. Очищаем ВСЮ выделенную физическую память (HHDM)
            // Это обнуляет .bss автоматически
            memset((void*)(VIRT(phys_mem)), 0, num_pages * 4096);

            // 4. КОПИРУЕМ ДАННЫЕ С УЧЕТОМ СМЕЩЕНИЯ (КРИТИЧНО!)
            // Данные должны лечь по адресу VIRT(phys_mem) + 0x9E0
            memcpy((void*)(VIRT(phys_mem) + page_offset), elf_raw + offset, filesz);
            
            term_print("EXEC: Segment loaded correctly.\n");
        }
    }

    term_print("EXEC: Starting Ring 3 process...\n");
    uint64_t user_argv_page = 0xB0000000; 
    void* phys_argv = pmm_alloc();
    if (!phys_argv) {
        term_print("EXEC: pmm_alloc for argv page failed\n");
        kfree(elf_raw);
        kfree(cmd_copy);
        return false;
    }
    vmm_map(proc_pml4, user_argv_page, (uint64_t)phys_argv, PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    
    // ВАЖНО: Обнуляем страницу аргументов!
    memset((void*)VIRT(phys_argv), 0, 4096);

    uint64_t* user_argv_array = (uint64_t*)VIRT(phys_argv); 
    char* user_string_area = (char*)VIRT(phys_argv) + 128; 
    uint64_t current_string_offset = 128;

    for (int i = 0; i < argc; i++) {
        user_argv_array[i] = user_argv_page + current_string_offset;
        strcpy(user_string_area, argv[i]);
    
        int len = strlen(argv[i]) + 1;
        user_string_area += len;
        current_string_offset += len;
    }
    user_argv_array[argc] = 0; 

    term_print("EXEC: Starting Ring 3 process with arguments...\n");

    /* Этап 6b: строим SysV initial stack (argc/argv/envp/auxv) для musl.
     * argv-указатели уже пользовательские (user_argv_array[i] в 0xB0000000),
     * строки на стек не копируем. envp пока пустой. Родной crt0 кадр игнорит. */
    sysv_args_t sv = {
        .argc      = argc,
        .argv_user = user_argv_array,
        .envc      = 0,
        .envp_user = 0,
        .at_phdr   = phdr_user,
        .at_phent  = header->e_phentsize,
        .at_phnum  = header->e_phnum,
        .at_entry  = header->e_entry,
    };
    task_create_full((void(*)())header->e_entry, (uint64_t)argc,
                     user_argv_page, phys_pml4, &sv);

    // FS base will be set by the scheduler when it switches to this task
    
    kfree(elf_raw);
    kfree(cmd_copy);
    return true;
}

/* ======================================================================== *
 *  Этап 1b — execve: замена образа текущего процесса
 *
 *  task_load_image() загружает ELF `path` в НОВОЕ адресное пространство,
 *  готовит пользовательский стек (8 МБ), TLS и страницу argv — но НИЧЕГО не
 *  переключает и не освобождает. Переключение cr3, обновление полей задачи и
 *  освобождение старого адресного пространства делает вызывающий
 *  (syscall.c, case 54) ПОСЛЕ успешного возврата — так замена образа атомарна:
 *  при ошибке загрузки старый процесс продолжает жить без изменений.
 *
 *  argv[] здесь уже скопирован в память ядра вызывающим (под stac/clac),
 *  поэтому к пользовательской памяти эта функция не обращается.
 * ======================================================================== */
bool task_load_image(const char *path, char *const argv[], int argc,
                     char *const envp[], int envc,
                     uint64_t *out_entry, uint64_t *out_user_rsp,
                     uint64_t *out_argv_ptr, uint64_t *out_envp_ptr,
                     uint64_t *out_cr3, uint64_t *out_fs_base) {
    if (!path || argc < 1) return false;

    /* VFS хранит файлы в корне под ПОЛНЫМ путём ("bin/foo.elf"), поэтому если
     * передано голое имя без '/', пробуем ещё раз с префиксом "bin/" — так
     * execve("foo.elf") и execve("bin/foo.elf") работают одинаково. */
    uint32_t elf_size = 0;
    uint8_t *elf_raw = vfs_read_file((char *)path, &elf_size);
    if (!elf_raw) {
        int has_slash = 0;
        for (const char *p = path; *p; p++) if (*p == '/') { has_slash = 1; break; }
        if (!has_slash) {
            char alt[256];
            alt[0] = 'b'; alt[1] = 'i'; alt[2] = 'n'; alt[3] = '/';
            int k = 0;
            for (; k < 251 && path[k]; k++) alt[4 + k] = path[k];
            alt[4 + k] = 0;
            elf_raw = vfs_read_file(alt, &elf_size);
        }
    }
    if (!elf_raw) {
        term_print("EXECVE: file not found: ");
        term_print((char *)path);
        term_print("\n");
        return false;
    }

    Elf64_Ehdr *header = (Elf64_Ehdr *)elf_raw;
    if (memcmp(header->e_ident, "\x7f\x45\x4c\x46", 4) != 0) {
        term_print("EXECVE: not a valid ELF\n");
        kfree(elf_raw);
        return false;
    }

    page_table_t *proc_pml4 = vmm_create_address_space();
    if (!proc_pml4) { kfree(elf_raw); return false; }
    uint64_t phys_pml4 = PHYS(proc_pml4);

    /* --- PT_LOAD сегменты --- */
    Elf64_Phdr *phdr = (Elf64_Phdr *)(elf_raw + header->e_phoff);
    /* Этап 6b: адрес таблицы Program Headers в памяти процесса (AT_PHDR).
     * Нужен musl, чтобы найти PT_TLS при инициализации TLS. Ищем PT_LOAD,
     * который физически перекрывает e_phoff в файле. */
    uint64_t phdr_user = 0;
    uint64_t phsz = (uint64_t)header->e_phnum * header->e_phentsize;
    for (int i = 0; i < header->e_phnum; i++) {
        if (phdr[i].p_type != 1) continue; /* PT_LOAD */
        uint64_t vaddr  = phdr[i].p_vaddr;
        uint64_t memsz  = phdr[i].p_memsz;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t offset = phdr[i].p_offset;
        if (phdr_user == 0 &&
            offset <= header->e_phoff &&
            header->e_phoff + phsz <= offset + phdr[i].p_filesz)
            phdr_user = vaddr + (header->e_phoff - offset);
        if (offset + filesz > elf_size)
            filesz = (offset < elf_size) ? (elf_size - offset) : 0;

        uint64_t page_offset = vaddr & 0xFFF;
        uint64_t base_vaddr  = vaddr & ~0xFFFULL;
        uint64_t num_pages   = ((memsz + page_offset) + 4095) / 4096;

        void *phys_mem = pmm_alloc_continuous(num_pages);
        if (!phys_mem) {
            term_print("EXECVE: OOM loading segment\n");
            vmm_destroy_address_space(phys_pml4);
            kfree(elf_raw);
            return false;
        }
        for (uint64_t p = 0; p < num_pages; p++)
            vmm_map(proc_pml4, base_vaddr + p * 4096,
                    (uint64_t)phys_mem + p * 4096,
                    PTE_PRESENT | PTE_USER | PTE_WRITABLE);
        memset((void *)VIRT(phys_mem), 0, num_pages * 4096);
        memcpy((void *)(VIRT(phys_mem) + page_offset), elf_raw + offset, filesz);
    }

    /* --- пользовательский стек (8 МБ) + TLS (как в task_create) --- */
    uint64_t stack_top   = 0x70000000000ULL;
    uint64_t stack_pages = 2048;
    void *top_stack_phys = 0;            /* Этап 6b: верхняя страница стека */
    for (uint64_t i = 0; i < stack_pages; i++) {
        uint64_t v = stack_top - (stack_pages * 4096) + (i * 4096);
        void *phys = pmm_alloc();
        if (!phys) {
            term_print("EXECVE: OOM stack\n");
            vmm_destroy_address_space(phys_pml4);
            kfree(elf_raw);
            return false;
        }
        vmm_map(proc_pml4, v, (uint64_t)phys,
                PTE_PRESENT | PTE_USER | PTE_WRITABLE);
        memset((void *)VIRT(phys), 0, 4096);
        if (v == stack_top - 4096) top_stack_phys = phys;
    }
    uint64_t tls_virt = 0x60000000000ULL;
    void *tls_phys = pmm_alloc();
    if (!tls_phys) { vmm_destroy_address_space(phys_pml4); kfree(elf_raw); return false; }
    vmm_map(proc_pml4, tls_virt, (uint64_t)tls_phys,
            PTE_USER | PTE_WRITABLE | PTE_PRESENT);
    memset((void *)VIRT(tls_phys), 0, 4096);
    ((uint64_t *)VIRT(tls_phys))[0] = tls_virt + 64;

    /* --- страница argv: [256 байт под массив указателей][строки] --- */
    uint64_t user_argv_page = 0xB0000000ULL;
    void *phys_argv = pmm_alloc();
    if (!phys_argv) { vmm_destroy_address_space(phys_pml4); kfree(elf_raw); return false; }
    vmm_map(proc_pml4, user_argv_page, (uint64_t)phys_argv,
            PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    memset((void *)VIRT(phys_argv), 0, 4096);

    if (argc > 16) argc = 16;
    uint64_t *argv_arr = (uint64_t *)VIRT(phys_argv);
    char *strarea = (char *)VIRT(phys_argv) + 256; /* 32 ptr * 8 = 256 */
    uint64_t soff = 256;
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i] ? argv[i] : "";
        int len = (int)strlen(s) + 1;
        if (soff + (uint64_t)len > 4096) { /* страница argv переполнена */
            argv_arr[i] = 0;
            argc = i;
            break;
        }
        argv_arr[i] = user_argv_page + soff;
        memcpy(strarea, s, len);
        strarea[len - 1] = 0;
        strarea += len;
        soff += len;
    }
    argv_arr[argc] = 0;

    /* --- страница envp: [256 байт под массив указателей][строки] (Этап 3) --- */
    uint64_t user_env_page = 0xB0001000ULL;
    void *phys_env = pmm_alloc();
    if (!phys_env) { vmm_destroy_address_space(phys_pml4); kfree(elf_raw); return false; }
    vmm_map(proc_pml4, user_env_page, (uint64_t)phys_env,
            PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    memset((void *)VIRT(phys_env), 0, 4096);

    if (envc < 0)  envc = 0;
    if (envc > 31) envc = 31;          /* массив указателей: 32 слота * 8 = 256 */
    uint64_t *env_arr = (uint64_t *)VIRT(phys_env);
    char *envstr = (char *)VIRT(phys_env) + 256;
    uint64_t eoff = 256;
    int eput = 0;
    for (int i = 0; i < envc && envp; i++) {
        const char *s = envp[i] ? envp[i] : "";
        int len = (int)strlen(s) + 1;
        if (eoff + (uint64_t)len > 4096) break;   /* страница env переполнена */
        env_arr[eput] = user_env_page + eoff;
        memcpy(envstr, s, len);
        envstr[len - 1] = 0;
        envstr += len;
        eoff += len;
        eput++;
    }
    env_arr[eput] = 0;

    /* ----------------------------------------------------------------------
     * Этап 6b — System V AMD64 initial stack (для musl / Linux-ABI бинарей).
     *
     * Родной crt0 читает argc/argv/envp из РЕГИСТРОВ (rdi/rsi/rdx) и стек при
     * входе не читает. musl же читает их из стека в _start. Поэтому мы СТРОИМ
     * полный SysV-кадр на вершине стека ДОПОЛНИТЕЛЬНО — детект ABI не нужен:
     *
     *   rsp -> argc
     *          argv[0..argc-1], NULL
     *          envp[0..n-1],    NULL
     *          auxv пары ...,   AT_NULL
     *          (выше) строки argv/envp + 16 байт AT_RANDOM
     *
     * Всё умещаем в верхнюю страницу стека [stack_top-4096, stack_top).
     * При переполнении страницы откатываемся к старому поведению (stack_top-16),
     * родные приложения от этого не страдают.
     * -------------------------------------------------------------------- */
    uint64_t sysv_rsp = stack_top - 16;
    if (top_stack_phys) {
        uint64_t base_user = stack_top - 4096;
        uint8_t *top = (uint8_t *)VIRT(top_stack_phys);
        #define U2K(u) ((void *)(top + ((u) - base_user)))
        uint64_t sp = stack_top;
        int ok = 1;

        /* 1. строки argv */
        uint64_t argv_u[17];
        for (int i = 0; i < argc; i++) {
            const char *s = argv[i] ? argv[i] : "";
            int len = (int)strlen(s) + 1;
            if (sp - (uint64_t)len < base_user + 512) { argc = i; break; }
            sp -= len;
            memcpy(U2K(sp), s, len);
            argv_u[i] = sp;
        }
        /* 2. строки envp (берём из исходного envp[], как и env_arr выше) */
        uint64_t envp_u[32];
        for (int i = 0; i < eput; i++) {
            const char *s = (envp && envp[i]) ? envp[i] : "";
            int len = (int)strlen(s) + 1;
            if (sp - (uint64_t)len < base_user + 512) { eput = i; break; }
            sp -= len;
            memcpy(U2K(sp), s, len);
            envp_u[i] = sp;
        }
        /* 3. 16 случайных байт для AT_RANDOM (canary/TLS у musl) */
        sp -= 16;
        rdrand_bytes(U2K(sp), 16);
        uint64_t at_random = sp;

        /* выравниваем вершину строк по 16 */
        sp &= ~0xFULL;

        /* 4. auxv (key, val) */
        uint64_t aux[32]; int an = 0;
        if (phdr_user) {
            aux[an++] = 3;  aux[an++] = phdr_user;              /* AT_PHDR  */
            aux[an++] = 4;  aux[an++] = header->e_phentsize;    /* AT_PHENT */
            aux[an++] = 5;  aux[an++] = header->e_phnum;        /* AT_PHNUM */
        }
        aux[an++] = 6;  aux[an++] = 4096;                      /* AT_PAGESZ */
        aux[an++] = 9;  aux[an++] = header->e_entry;           /* AT_ENTRY  */
        aux[an++] = 11; aux[an++] = 0;                         /* AT_UID    */
        aux[an++] = 12; aux[an++] = 0;                         /* AT_EUID   */
        aux[an++] = 13; aux[an++] = 0;                         /* AT_GID    */
        aux[an++] = 14; aux[an++] = 0;                         /* AT_EGID   */
        aux[an++] = 23; aux[an++] = 0;                         /* AT_SECURE */
        aux[an++] = 17; aux[an++] = 100;                       /* AT_CLKTCK */
        aux[an++] = 25; aux[an++] = at_random;                 /* AT_RANDOM */
        aux[an++] = 0;  aux[an++] = 0;                         /* AT_NULL   */

        /* 5. слоты: argc + (argv+NULL) + (envp+NULL) + auxv; финал rsp%16==0 */
        int slots = 1 + (argc + 1) + (eput + 1) + an;
        if (slots & 1) sp -= 8;                /* паддинг до чётного числа слотов */
        sp -= (uint64_t)slots * 8;
        if (sp < base_user) { ok = 0; }        /* не влезли — откат */

        if (ok) {
            uint64_t *w = (uint64_t *)U2K(sp);
            int idx = 0;
            w[idx++] = (uint64_t)argc;
            for (int i = 0; i < argc; i++) w[idx++] = argv_u[i];
            w[idx++] = 0;
            for (int i = 0; i < eput; i++)  w[idx++] = envp_u[i];
            w[idx++] = 0;
            for (int i = 0; i < an; i++)    w[idx++] = aux[i];
            sysv_rsp = sp;
        }
        #undef U2K
    }

    *out_entry    = header->e_entry;
    *out_user_rsp = sysv_rsp;
    *out_argv_ptr = user_argv_page;
    *out_envp_ptr = user_env_page;
    *out_cr3      = phys_pml4;
    *out_fs_base  = tls_virt;

    kfree(elf_raw);
    return true;
}

/* Выставить FS.base (TLS) для текущего процесса. Нужно execve: возврат в
 * новый образ идёт через iretq БЕЗ переключения планировщиком, поэтому MSR
 * надо обновить вручную. */
void task_set_fs_base(uint64_t v) {
    if (v) wrmsr(IA32_FS_BASE_MSR, v);
}

/* ======================================================================== *
 *  Этап 1 — Процессная модель: fork / exit-status / waitpid
 *
 *  Семантика поверх существующего кольцевого планировщика:
 *   - task_fork()        дублирует процесс (eager-copy адресного пространства),
 *                        ребёнок "возвращается" из int 0x80 с rax = 0.
 *   - task_exit_current() помечает процесс зомби + код выхода, освобождает
 *                        его память и будит родителя, ждущего в waitpid.
 *   - task_waitpid()     reaping зомби-детей, отдаёт код выхода.
 *
 *  ОГРАНИЧЕНИЯ (TODO след. этапов):
 *   - fd-таблица пока глобальная (fs/fd.c) — не дублируется при fork.
 *   - eager-copy, без COW (просто и надёжно; COW — оптимизация позже).
 *   - осиротевшие зомби (родитель умер раньше) не reaping'аются, остаются
 *     в кольце как !running — как и текущие "мёртвые" задачи. Подметёт
 *     будущий cleanup-проход планировщика / reparent к init.
 * ======================================================================== */

uint64_t task_fork(stack_frame_t* parent_frame) {
    task_t* parent = current_task;

    /* 1. Клонируем пользовательскую половину адресного пространства. */
    page_table_t* child_pml4 = vmm_clone_address_space(parent->cr3);
    if (!child_pml4) {
        term_print("FORK: vmm_clone_address_space failed (OOM)\n");
        return (uint64_t)-1;
    }

    /* 2. Структура задачи ребёнка. */
    task_t* child = (task_t*)kmalloc(sizeof(task_t));
    memset(child, 0, sizeof(task_t));
    /* Этап 9: ребёнок наследует ЖИВОЕ FPU/SSE-состояние родителя — мы сейчас
     * в его контексте (int 0x80/0x81 регистры XMM не трогает), поэтому
     * снимаем его прямо с CPU. */
    __asm__ volatile("fxsave64 (%0)" :: "r"(task_fpu_area(child)) : "memory");
    child->cr3       = PHYS(child_pml4);
    child->id        = next_pid++;
    child->parent_id = parent->id;
    child->pgid      = parent->pgid ? parent->pgid : parent->id; /* Этап 6e: наследуем группу родителя */
    child->running   = true;
    child->brk       = parent->brk;
    child->fs_base   = parent->fs_base; /* тот же TLS-vaddr (страница скопирована) */
    child->zombie    = false;
    /* Этап 2: ребёнок наследует таблицу дескрипторов — ofd'шки разделяются
     * (refcount++), так что пайпы/файлы остаются открытыми у обоих. */
    child->fdt       = fd_table_clone(parent->fdt);
    /* Этап 3: ребёнок наследует рабочую директорию родителя. */
    {
        int i = 0;
        for (; i < (int)sizeof(child->cwd) - 1 && parent->cwd[i]; i++)
            child->cwd[i] = parent->cwd[i];
        child->cwd[i] = '\0';
        if (child->cwd[0] == '\0') { child->cwd[0] = '/'; child->cwd[1] = '\0'; }
    }
    /* Этап 4: наследуем обработчики и маску блокировки; pending обнуляется. */
    task_signal_fork(child, parent);

    /* 3. Ядерный стек ребёнка (как в task_create: 4 страницы = 16 КБ). */
    void* kstack_phys = pmm_alloc_continuous(4);
    if (!kstack_phys) {
        vmm_destroy_address_space(child->cr3);
        kfree(child);
        term_print("FORK: kstack alloc failed (OOM)\n");
        return (uint64_t)-1;
    }
    child->kstack_at_bottom = (uint64_t)kstack_phys + hhdm_offset + 16384;

    /* 4. Кадр прерывания ребёнка = копия родительского, но rax = 0,
     *    чтобы fork() в ребёнке вернул 0. Все остальные регистры, rip,
     *    user-rsp, cs/ss/rflags идентичны → ребёнок продолжит ровно
     *    после своего int 0x80. */
    stack_frame_t* cf =
        (stack_frame_t*)(child->kstack_at_bottom - sizeof(stack_frame_t));
    *cf = *parent_frame;
    cf->rax = 0;
    child->rsp = (uint64_t)cf;

    /* 5. Вставляем в кольцо планировщика. */
    if (!task_list) {
        child->next = child;
        task_list = child;
    } else {
        child->next = task_list->next ? task_list->next : task_list;
        task_list->next = child;
    }

    return child->id; /* родитель получает pid ребёнка */
}

void task_exit_current(int code) {
    task_t* me = current_task;

    /* init (pid 1) убивать нельзя. */
    if (me->id == 1) {
        yield();
        return;
    }

    me->exit_code = code;

    /* Этап 2: закрываем все дескрипторы (flush файлов, закрытие концов пайпов —
     * читатель/писатель на другом конце получит EOF). Делается на нашем стеке,
     * до ухода в планировщик; освобождает только kheap-объекты, не kstack. */
    if (me->fdt) {
        fd_table_destroy(me->fdt);
        me->fdt = NULL;
    }

    /* Освобождаем пользовательскую память процесса. */
    if (me->cr3 != 0 && me->cr3 != kernel_cr3) {
        vmm_destroy_address_space(me->cr3);
    }

    me->running = false;
    me->zombie  = true;

    /* Будим родителя, если он спит в waitpid и ждёт именно нас (или любого). */
    if (me->parent_id != 0 && task_list) {
        task_t* p = task_list;
        do {
            if (p->id == me->parent_id) {
                /* Этап 4: родителю — SIGCHLD (по умолчанию игнорируется,
                 * но обработчик/ожидание могут на него среагировать). */
                p->sig_pending |= (1ULL << 17 /* SIGCHLD */);
                if (p->waiting &&
                    (p->wait_for == 0 || p->wait_for == me->id)) {
                    p->running = true;
                    p->waiting = false;
                }
                break;
            }
            p = p->next;
        } while (p && p != task_list);
    }

    /* Уходим в планировщик навсегда. Структуру task_t и kstack освободит
     * родитель в waitpid (task_reap) — нельзя освобождать свой kstack,
     * пока мы на нём. */
    yield();
    while (1)
        ;
}

/* Отвязывает зомби-задачу из кольца и освобождает её ресурсы.
 * Вызывается ТОЛЬКО из waitpid (т.е. с чужого, родительского стека). */
static void task_reap(task_t* z) {
    if (task_list) {
        task_t* prev = task_list;
        int guard = 0;
        while (prev->next != z && guard++ < 100000)
            prev = prev->next;
        if (prev->next == z) {
            prev->next = z->next;
            if (task_list == z)
                task_list = (z->next == z) ? NULL : z->next;
        }
    }

    /* Освобождаем ядерный стек (выделен pmm_alloc_continuous(4)). */
    if (z->kstack_at_bottom) {
        uint64_t base_phys = z->kstack_at_bottom - 16384 - hhdm_offset;
        for (int p = 0; p < 4; p++)
            pmm_free((void*)(base_phys + (uint64_t)p * 4096));
    }

    kfree(z);
}

/* Этап 6e: поиск задачи по pid в кольце планировщика (task_list — static,
 * поэтому доступ извне только через эту функцию). NULL если нет такой. */
task_t* task_by_id(uint64_t pid) {
    if (!task_list) return NULL;
    task_t* t = task_list;
    do {
        if (t->id == pid) return t;
        t = t->next;
    } while (t && t != task_list);
    return NULL;
}

/* Этап 6e-2: общая реализация. nohang!=0 => не блокируемся: вернуть 0, если
 * дети есть, но ни один ещё не зомби (поведение WNOHANG). Блокирующий
 * task_waitpid() = вызов с nohang=0. */
int64_t task_waitpid_ex(uint64_t pid, int* status_out, int nohang) {
    while (1) {
        bool have_child = false;
        task_t* found = NULL;

        if (task_list) {
            task_t* t = task_list;
            do {
                if (t->parent_id == current_task->id &&
                    (pid == 0 || t->id == pid)) {
                    have_child = true;
                    if (t->zombie) {
                        found = t;
                        break;
                    }
                }
                t = t->next;
            } while (t && t != task_list);
        }

        if (found) {
            uint64_t cid = found->id;
            if (status_out)
                *status_out = found->exit_code;
            task_reap(found);
            return (int64_t)cid;
        }

        if (!have_child)
            return -1; /* ECHILD: у процесса нет таких детей */

        if (nohang)
            return 0;  /* WNOHANG: дети есть, но готовых зомби нет */

        /* Этап 4: если есть доставляемый сигнал (кроме SIGCHLD — это наш
         * штатный «будильник»), прерываем ожидание с -1 (EINTR). Обработчик
         * запустится при возврате из сисколла (signal_deliver). Проверка
         * только перед блокировкой: готовый зомби обрабатывается выше. */
        if (current_task->sig_pending & ~current_task->sig_blocked &
            ~(1ULL << 17 /* SIGCHLD */))
            return -1;

        /* Блокируемся до тех пор, пока ребёнок не станет зомби. */
        current_task->waiting  = true;
        current_task->wait_for = pid;
        current_task->running  = false;
        yield();
        current_task->waiting  = false;
    }
}

int64_t task_waitpid(uint64_t pid, int* status_out) {
    return task_waitpid_ex(pid, status_out, 0);
}

// В task.c

void task_kill_self() {
  if (current_task->id == 1)
    return; // Нельзя убить idle/kernel процесс

  // Этап 2: закрываем дескрипторы (как в task_exit_current).
  if (current_task->fdt) {
    fd_table_destroy(current_task->fdt);
    current_task->fdt = NULL;
  }

  // 1. Освобождаем всю пользовательскую память (Ring 3)
  // Это сразу вернет мегабайты в монитор!
  if (current_task->cr3 != 0 && current_task->cr3 != kernel_cr3) {
    vmm_destroy_address_space(current_task->cr3);
  }

  // 2. Помечаем задачу как мертвую
  current_task->running = false;

  // 3. (Опционально) Освобождаем ядерный стек и саму структуру task_t
  // ВНИМАНИЕ: Это делать сложно, так как мы СЕЙЧАС на этом стеке.
  // Для "вылизанности" мы просто помечаем её, а планировщик (schedule)
  // сможет её удалить из списка в следующем цикле.

  printf("[TASK] Process %u terminated and memory reclaimed.\n",
         current_task->id);

  // 4. Уходим в планировщик навсегда
  yield();
  while (1)
    ; // Сюда мы никогда не вернемся
}

void task_list_all() {
  task_t *start = task_list;
  task_t *curr = start;

  term_print("\e[33m PID   STATE       CR3          MEM_BRK\e[0m\n");
  do {
    char buf[128];
    const char *state = curr->running ? "RUNNING" : "STOPPED";
    // PID 1 обычно idle или init
    sprintf(buf, " %d     %s     %x   %x\n", (uint32_t)curr->id, state,
            (uint32_t)curr->cr3, (uint32_t)curr->brk);
    term_print(buf);
    curr = curr->next;
  } while (curr != start);
}

// Убивает процесс по PID
bool task_terminate_by_pid(uint64_t pid) {
  if (pid == 1) {
    term_print("TASK: Cannot kill kernel init process!\n");
    return false;
  }

  if (!task_list) return false;
  task_t *curr = task_list;
  do {
    if (curr->id == pid) {
      curr->running = false;
      // Если у процесса был свой CR3 (не ядро), чистим память
      if (curr->cr3 != 0) {
        // vmm_destroy_address_space(curr->cr3); // Твоя функция очистки
      }
      // Если убитая задача держала фокус ввода — отдаём его обратно
      // sysgui, иначе клавиатура/мышь "залипнут" на мёртвом PID.
      {
        extern volatile uint64_t fg_app_pid;
        if (fg_app_pid == pid) fg_app_pid = 0;
      }
      char buf[64];
      sprintf(buf, "TASK: Process %d terminated.\n", (uint32_t)pid);
      term_print(buf);
      return true;
    }
    curr = curr->next;
    if (!curr) break; // защита от рваного ring-list
  } while (curr != task_list);

  term_print("TASK: PID not found.\n");
  return false;
}
// =============================================================================
//                       Публичные хелперы для оболочки
// =============================================================================

task_t* task_get_list_head(void) { return task_list; }

void task_kill_all_user(void) {
  (void)task_kill_all_user_count();
}

int task_kill_all_user_count(void) {
  if (!task_list) return 0;
  int n = 0;
  task_t *start = task_list;
  task_t *curr = start;
  do {
    // PID 1 — idle/init ядра, его нельзя убивать (см. task_kill_self).
    //
    // Также НЕ трогаем current_task. Раньше killall убивал и сам
    // вызывающий процесс — типичный сценарий: пользователь печатает
    // `killall` в ring-3 Lua-терминале (sysgui, PID 2), syscall 72
    // обнулял sysgui->running, syscall возвращался, Lua печатал
    // "terminated N tasks", но при следующем yield sysgui больше не
    // шедулился — экран замирал, ввод пропадал, со стороны выглядело
    // как "killall ничего не делает / система зависла". Семантика
    // killall теперь = "всё кроме меня и kernel-init", что совпадает
    // с ожиданием desktop-shell.
    //
    // Из emergency_kill_all_and_shell() current_task == idle (PID 1),
    // так что доп. проверка ничего не ломает: idle и так пропускается.
    if (curr->id != 1 && curr != current_task && curr->running) {
      curr->running = false;
      {
        extern volatile uint64_t fg_app_pid;
        if (fg_app_pid == curr->id) fg_app_pid = 0;
      }
      n++;
      // vmm_destroy_address_space(curr->cr3) умышленно НЕ вызываем
      // здесь: пользовательский процесс может быть прямо сейчас на
      // своих страницах; планировщик/будущий cleanup освободит их
      // когда задача окончательно слезет с CPU.
    }
    curr = curr->next;
  } while (curr && curr != start);
  return n;
}

bool task_snapshot_at(int idx, task_snapshot_t *out) {
  if (!task_list || !out || idx < 0) return false;
  task_t *start = task_list;
  task_t *curr = start;
  int i = 0;
  do {
    if (i == idx) {
      out->pid = curr->id;
      out->cr3 = curr->cr3;
      out->brk = curr->brk;
      out->running = curr->running;
      return true;
    }
    i++;
    curr = curr->next;
  } while (curr && curr != start);
  return false;
}

/* Этап 8 (отладка): pid текущей задачи для panic_handler. */
unsigned current_task_id_for_panic(void) {
    return current_task ? (unsigned)current_task->id : 0u;
}
