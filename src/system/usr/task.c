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
#include "../misc/random.h"
#include "signal.h"
#include "sched.h"
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

static uint8_t fpu_clean_state[512] __attribute__((aligned(16)));
static bool    fpu_clean_ready = false;

void task_fpu_init_area(task_t* t) {
    if (!fpu_clean_ready) {
        uint8_t tmp[512 + 15];
        void* live = (void*)(((uint64_t)tmp + 15) & ~(uint64_t)15);
        __asm__ volatile("fxsave64 (%0)" :: "r"(live) : "memory");
        __asm__ volatile("fninit");
        uint32_t mxcsr = 0x1F80;
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
    
    // Создаем структуру процесса ядра (init)
    process_t *proc = (process_t*)kmalloc(sizeof(process_t));
    memset(proc, 0, sizeof(process_t));
    proc->pid = next_pid++;
    proc->cr3 = kernel_cr3;
    proc->brk = 0x40000000;
    proc->fdt = fd_table_create();
    proc->cwd[0] = '/'; proc->cwd[1] = '\0';
    
    current_task->process = proc;
    current_task->id = proc->pid; // TID = PID для главного потока
    current_task->running = true;
    current_task->state = TASK_STATE_RUNNABLE;

    current_task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384; 
    task_signal_init(current_task);

    // Добавляем в глобальный двусвязный список
    current_task->next = current_task;
    current_task->prev = current_task;
    task_list = current_task;
}

struct fd_table *task_current_fdt(void) {
    return (current_task && current_task->process) ? current_task->process->fdt : NULL;
}

const char *task_current_cwd(void) {
    if (current_task && current_task->process && current_task->process->cwd[0]) {
        return current_task->process->cwd;
    }
    return "/";
}

int task_chdir(const char *path) {
    if (!current_task || !current_task->process || !path) return -1;
    char norm[256];
    if (vfs_normalize(current_task->process->cwd, path, norm, sizeof(norm)) < 0) return -1;
    if (!vfs_dir_exists(norm)) return -1;
    int i = 0;
    for (; i < (int)sizeof(current_task->process->cwd) - 1 && norm[i]; i++)
        current_task->process->cwd[i] = norm[i];
    current_task->process->cwd[i] = '\0';
    return 0;
}

void task_resolve_fs_path(const char *in, char *out, int outsz) {
    if (!out || outsz < 1) return;
    const char *cwd = task_current_cwd();
    char norm[256];
    if (vfs_normalize(cwd, in, norm, sizeof(norm)) < 0) {
        int j = 0; const char *p = in ? in : "";
        while (*p == '/') p++;
        for (; p[j] && j < outsz - 1; j++) out[j] = p[j];
        out[j] = '\0';
        return;
    }
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
  task_fpu_init_area(new_task);
  
  // Создаем PCB (структуру процесса) для этого потока
  process_t *proc = (process_t *)kmalloc(sizeof(process_t));
  memset(proc, 0, sizeof(process_t));
  
  proc->pid = next_pid++; // PID теперь относится к процессу
  proc->cr3 = cr3;
  proc->brk = 0x40000000;
  proc->fdt = fd_table_create();
  proc->cwd[0] = '/'; proc->cwd[1] = '\0';
  
  new_task->process = proc;
  new_task->id = proc->pid; // TID по умолчанию равен PID
  
  new_task->running = true;
  new_task->state = TASK_STATE_RUNNABLE;
  task_signal_init(new_task);

  // 1. Ядерный стек (для прерываний)
  void *kstack_phys = pmm_alloc_continuous(4);
  new_task->kstack_at_bottom = (uint64_t)kstack_phys + hhdm_offset + 16384;

  stack_frame_t *frame =
      (stack_frame_t *)(new_task->kstack_at_bottom - sizeof(stack_frame_t));
  memset(frame, 0, sizeof(stack_frame_t));

  // 2. Пользовательский стек (8 МБ)
  void *top_stack_phys = NULL;
  if (cr3 != 0) {
    uint64_t stack_top = 0x70000000000;
    uint64_t stack_pages = 2048;

    for (uint64_t i = 0; i < stack_pages; i++) {
      uint64_t vaddr = stack_top - (stack_pages * 4096) + (i * 4096);
      void *phys = pmm_alloc();

      if (!phys) {
        term_print("TASK: KERNEL OUT OF RAM DURING STACK ALLOC!\n");
        while (1)
          ;
      }

      vmm_map((page_table_t *)VIRT(cr3), vaddr, (uint64_t)phys,
              PTE_PRESENT | PTE_USER | PTE_WRITABLE);

      memset((void *)VIRT(phys), 0, 4096);

      if (vaddr == stack_top - 4096) top_stack_phys = phys;
    }

    uint64_t tls_virt = 0x60000000000;
    void *tls_phys = pmm_alloc();
    vmm_map((page_table_t *)VIRT(cr3), tls_virt, (uint64_t)tls_phys,
            PTE_USER | PTE_WRITABLE | PTE_PRESENT);
    memset((void *)VIRT(tls_phys), 0, 4096);
    ((uint64_t *)VIRT(tls_phys))[0] = tls_virt + 64;
    new_task->fs_base = tls_virt;

    frame->rsp = stack_top - 16;
  }

  // 3. Настройка фрейма
  frame->rip = (uint64_t)entry;
  frame->rdi = arg1;
  frame->rsi = arg2;
  frame->rflags = 0x202;

  if (cr3 == 0) {
    frame->cs = 0x08;
    frame->ss = 0x10;
    frame->rsp = (uint64_t)kmalloc(16384) + 16384;
  } else {
    frame->cs = 0x23;
    frame->ss = 0x1B;
  }

  if (sv && cr3 != 0 && top_stack_phys) {
    uint64_t stack_top = 0x70000000000ULL;
    uint64_t base_user = stack_top - 4096;
    uint8_t *topk = (uint8_t *)VIRT(top_stack_phys);
    #define U2K(u) ((void *)(topk + ((uint64_t)(u) - base_user)))
    uint64_t sp = stack_top;
    int argc = sv->argc, envc = sv->envc;

    sp -= 16;
    rdrand_bytes(U2K(sp), 16);
    uint64_t at_random = sp;
    sp &= ~0xFULL;

    uint64_t aux[32]; int an = 0;
    if (sv->at_phdr) {
      aux[an++] = 3;  aux[an++] = sv->at_phdr;
      aux[an++] = 4;  aux[an++] = sv->at_phent;
      aux[an++] = 5;  aux[an++] = sv->at_phnum;
    }
    aux[an++] = 6;  aux[an++] = 4096;
    aux[an++] = 9;  aux[an++] = sv->at_entry;
    aux[an++] = 11; aux[an++] = 0;
    aux[an++] = 12; aux[an++] = 0;
    aux[an++] = 13; aux[an++] = 0;
    aux[an++] = 14; aux[an++] = 0;
    aux[an++] = 23; aux[an++] = 0;
    aux[an++] = 17; aux[an++] = 100;
    aux[an++] = 25; aux[an++] = at_random;
    aux[an++] = 0;  aux[an++] = 0;

    int slots = 1 + (argc + 1) + (envc + 1) + an;
    if (slots & 1) sp -= 8;
    sp -= (uint64_t)slots * 8;
    if (sp >= base_user) {
      uint64_t *w = (uint64_t *)U2K(sp);
      int idx = 0;
      w[idx++] = (uint64_t)argc;
      for (int i = 0; i < argc; i++) w[idx++] = sv->argv_user[i];
      w[idx++] = 0;
      for (int i = 0; i < envc; i++) w[idx++] = sv->envp_user[i];
      w[idx++] = 0;
      for (int i = 0; i < an; i++)   w[idx++] = aux[i];
      frame->rsp = sp;
    }
    #undef U2K
  }

  new_task->rsp = (uint64_t)frame;

  // Вставляем в глобальный двусвязный список
  if (!task_list) {
    new_task->next = new_task;
    new_task->prev = new_task;
    task_list = new_task;
  } else {
    new_task->next = task_list->next;
    new_task->prev = task_list;
    task_list->next->prev = new_task;
    task_list->next = new_task;
  }

  // Передаем планировщику для выполнения
  sched_enqueue(new_task);
  return new_task;
}

void task_create(void (*entry)(), uint64_t arg1, uint64_t arg2, uint64_t cr3) {
  task_create_full(entry, arg1, arg2, cr3, 0);
}

uint64_t schedule(uint64_t current_rsp) {
    return sched_switch(current_rsp);
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

    if (!elf_raw && argv[0][0] != '/') {
        char binpath[256];
        sprintf(binpath, "/bin/%s", argv[0]);
        elf_raw = vfs_read_file(binpath, &elf_size);
    }

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

    page_table_t* proc_pml4 = vmm_create_address_space();
    uint64_t phys_pml4 = PHYS(proc_pml4);
    Elf64_Phdr* phdr = (Elf64_Phdr*)(elf_raw + header->e_phoff);
    uint64_t phdr_user = 0;
    uint64_t phsz = (uint64_t)header->e_phnum * header->e_phentsize;
    for (int i = 0; i < header->e_phnum; i++) {
        if (phdr[i].p_type == 1) { 
            uint64_t vaddr = phdr[i].p_vaddr;
            uint64_t memsz = phdr[i].p_memsz;
            uint64_t filesz = phdr[i].p_filesz;
            uint64_t offset = phdr[i].p_offset;
            if (phdr_user == 0 &&
                offset <= header->e_phoff &&
                header->e_phoff + phsz <= offset + phdr[i].p_filesz)
                phdr_user = vaddr + (header->e_phoff - offset);

            if (offset + filesz > elf_size) {
                filesz = (offset < elf_size) ? (elf_size - offset) : 0;
            }

            uint64_t page_offset = vaddr & 0xFFF;
            uint64_t base_vaddr = vaddr & ~0xFFF;
            uint64_t total_memsz = memsz + page_offset;
            uint64_t num_pages = (total_memsz + 4095) / 4096;

            void* phys_mem = pmm_alloc_continuous(num_pages);
            if (!phys_mem) {
                kfree(elf_raw);
                kfree(cmd_copy);
                return false;
            }

            for (uint64_t p = 0; p < num_pages; p++) {
                vmm_map(proc_pml4, 
                        base_vaddr + (p * 4096), 
                        (uint64_t)phys_mem + (p * 4096), 
                        PTE_PRESENT | PTE_USER | PTE_WRITABLE);
            }

            memset((void*)(VIRT(phys_mem)), 0, num_pages * 4096);
            memcpy((void*)(VIRT(phys_mem) + page_offset), elf_raw + offset, filesz);
        }
    }

    uint64_t user_argv_page = 0xB0000000; 
    void* phys_argv = pmm_alloc();
    if (!phys_argv) {
        kfree(elf_raw);
        kfree(cmd_copy);
        return false;
    }
    vmm_map(proc_pml4, user_argv_page, (uint64_t)phys_argv, PTE_PRESENT | PTE_USER | PTE_WRITABLE);
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

    kfree(elf_raw);
    kfree(cmd_copy);
    return true;
}

bool task_load_image(const char *path, char *const argv[], int argc,
                     char *const envp[], int envc,
                     uint64_t *out_entry, uint64_t *out_user_rsp,
                     uint64_t *out_argv_ptr, uint64_t *out_envp_ptr,
                     uint64_t *out_cr3, uint64_t *out_fs_base) {
    if (!path || argc < 1) return false;

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

    Elf64_Phdr *phdr = (Elf64_Phdr *)(elf_raw + header->e_phoff);
    uint64_t phdr_user = 0;
    uint64_t phsz = (uint64_t)header->e_phnum * header->e_phentsize;
    for (int i = 0; i < header->e_phnum; i++) {
        if (phdr[i].p_type != 1) continue; 
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

    uint64_t stack_top   = 0x70000000000ULL;
    uint64_t stack_pages = 2048;
    void *top_stack_phys = 0; 
    for (uint64_t i = 0; i < stack_pages; i++) {
        uint64_t v = stack_top - (stack_pages * 4096) + (i * 4096);
        void *phys = pmm_alloc();
        if (!phys) {
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

    uint64_t user_argv_page = 0xB0000000ULL;
    void *phys_argv = pmm_alloc();
    if (!phys_argv) { vmm_destroy_address_space(phys_pml4); kfree(elf_raw); return false; }
    vmm_map(proc_pml4, user_argv_page, (uint64_t)phys_argv,
            PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    memset((void *)VIRT(phys_argv), 0, 4096);

    if (argc > 16) argc = 16;
    uint64_t *argv_arr = (uint64_t *)VIRT(phys_argv);
    char *strarea = (char *)VIRT(phys_argv) + 256; 
    uint64_t soff = 256;
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i] ? argv[i] : "";
        int len = (int)strlen(s) + 1;
        if (soff + (uint64_t)len > 4096) { 
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

    uint64_t user_env_page = 0xB0001000ULL;
    void *phys_env = pmm_alloc();
    if (!phys_env) { vmm_destroy_address_space(phys_pml4); kfree(elf_raw); return false; }
    vmm_map(proc_pml4, user_env_page, (uint64_t)phys_env,
            PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    memset((void *)VIRT(phys_env), 0, 4096);

    if (envc < 0)  envc = 0;
    if (envc > 31) envc = 31; 
    uint64_t *env_arr = (uint64_t *)VIRT(phys_env);
    char *envstr = (char *)VIRT(phys_env) + 256;
    uint64_t eoff = 256;
    int eput = 0;
    for (int i = 0; i < envc && envp; i++) {
        const char *s = envp[i] ? envp[i] : "";
        int len = (int)strlen(s) + 1;
        if (eoff + (uint64_t)len > 4096) break; 
        env_arr[eput] = user_env_page + eoff;
        memcpy(envstr, s, len);
        envstr[len - 1] = 0;
        envstr += len;
        eoff += len;
        eput++;
    }
    env_arr[eput] = 0;

    uint64_t sysv_rsp = stack_top - 16;
    if (top_stack_phys) {
        uint64_t base_user = stack_top - 4096;
        uint8_t *top = (uint8_t *)VIRT(top_stack_phys);
        #define U2K(u) ((void *)(top + ((u) - base_user)))
        uint64_t sp = stack_top;
        int ok = 1;

        uint64_t argv_u[17];
        for (int i = 0; i < argc; i++) {
            const char *s = argv[i] ? argv[i] : "";
            int len = (int)strlen(s) + 1;
            if (sp - (uint64_t)len < base_user + 512) { argc = i; break; }
            sp -= len;
            memcpy(U2K(sp), s, len);
            argv_u[i] = sp;
        }
        uint64_t envp_u[32];
        for (int i = 0; i < eput; i++) {
            const char *s = (envp && envp[i]) ? envp[i] : "";
            int len = (int)strlen(s) + 1;
            if (sp - (uint64_t)len < base_user + 512) { eput = i; break; }
            sp -= len;
            memcpy(U2K(sp), s, len);
            envp_u[i] = sp;
        }
        sp -= 16;
        rdrand_bytes(U2K(sp), 16);
        uint64_t at_random = sp;

        sp &= ~0xFULL;

        uint64_t aux[32]; int an = 0;
        if (phdr_user) {
            aux[an++] = 3;  aux[an++] = phdr_user; 
            aux[an++] = 4;  aux[an++] = header->e_phentsize; 
            aux[an++] = 5;  aux[an++] = header->e_phnum; 
        }
        aux[an++] = 6;  aux[an++] = 4096; 
        aux[an++] = 9;  aux[an++] = header->e_entry; 
        aux[an++] = 11; aux[an++] = 0; 
        aux[an++] = 12; aux[an++] = 0; 
        aux[an++] = 13; aux[an++] = 0; 
        aux[an++] = 14; aux[an++] = 0; 
        aux[an++] = 23; aux[an++] = 0; 
        aux[an++] = 17; aux[an++] = 100; 
        aux[an++] = 25; aux[an++] = at_random; 
        aux[an++] = 0;  aux[an++] = 0; 

        int slots = 1 + (argc + 1) + (eput + 1) + an;
        if (slots & 1) sp -= 8; 
        sp -= (uint64_t)slots * 8;
        if (sp < base_user) { ok = 0; } 

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

void task_set_fs_base(uint64_t v) {
    if (v) wrmsr(IA32_FS_BASE_MSR, v);
}

uint64_t task_fork(stack_frame_t* parent_frame) {
    task_t* parent = current_task;
    if (!parent || !parent->process) return (uint64_t)-1;

    /* 1. Клонируем пользовательскую половину адресного пространства. */
    page_table_t* child_pml4 = vmm_clone_address_space(parent->process->cr3);
    if (!child_pml4) {
        term_print("FORK: vmm_clone_address_space failed (OOM)\n");
        return (uint64_t)-1;
    }

    /* 2. Структура задачи ребёнка. */
    task_t* child = (task_t*)kmalloc(sizeof(task_t));
    memset(child, 0, sizeof(task_t));
    __asm__ volatile("fxsave64 (%0)" :: "r"(task_fpu_area(child)) : "memory");
    
    child->running   = true;
    child->state     = TASK_STATE_RUNNABLE;
    child->fs_base   = parent->fs_base;
    task_signal_fork(child, parent);

    // Выделяем отдельный процесс для дочернего потока
    process_t* child_proc = (process_t*)kmalloc(sizeof(process_t));
    memset(child_proc, 0, sizeof(process_t));
    
    child_proc->pid        = next_pid++;
    child_proc->parent_pid = parent->process->pid;
    child_proc->pgid       = parent->process->pgid;
    child_proc->cr3        = PHYS(child_pml4);
    child_proc->brk        = parent->process->brk;
    child_proc->zombie     = false;
    child_proc->fdt        = fd_table_clone(parent->process->fdt);
    
    {
        int i = 0;
        for (; i < (int)sizeof(child_proc->cwd) - 1 && parent->process->cwd[i]; i++)
            child_proc->cwd[i] = parent->process->cwd[i];
        child_proc->cwd[i] = '\0';
        if (child_proc->cwd[0] == '\0') { child_proc->cwd[0] = '/'; child_proc->cwd[1] = '\0'; }
    }

    child->process = child_proc;
    child->id      = child_proc->pid; // TID = PID для главного потока

    /* 3. Ядерный стек ребёнка (4 страницы = 16 КБ). */
    void* kstack_phys = pmm_alloc_continuous(4);
    if (!kstack_phys) {
        vmm_destroy_address_space(child_proc->cr3);
        kfree(child_proc);
        kfree(child);
        term_print("FORK: kstack alloc failed (OOM)\n");
        return (uint64_t)-1;
    }
    child->kstack_at_bottom = (uint64_t)kstack_phys + hhdm_offset + 16384;

    /* 4. Кадр прерывания ребёнка */
    stack_frame_t* cf =
        (stack_frame_t*)(child->kstack_at_bottom - sizeof(stack_frame_t));
    *cf = *parent_frame;
    cf->rax = 0;
    child->rsp = (uint64_t)cf;

    /* 5. Вставляем в глобальный двусвязный список */
    if (!task_list) {
        child->next = child;
        child->prev = child;
        task_list = child;
    } else {
        child->next = task_list->next;
        child->prev = task_list;
        task_list->next->prev = child;
        task_list->next = child;
    }

    // Сообщаем планировщику
    sched_enqueue(child);

    return child_proc->pid; 
}

void task_exit_current(int code) {
    task_t* me = current_task;
    if (!me || !me->process) return;

    if (me->process->pid == 1) {
        yield();
        return;
    }

    me->process->exit_code = code;

    if (me->process->fdt) {
        fd_table_destroy(me->process->fdt);
        me->process->fdt = NULL;
    }

    if (me->process->cr3 != 0 && me->process->cr3 != kernel_cr3) {
        vmm_destroy_address_space(me->process->cr3);
    }

    me->running = false;
    me->state   = TASK_STATE_ZOMBIE;
    me->process->zombie  = true;

    // Извлекаем поток из очередей планировщика
    sched_dequeue(me);

    /* Будим родителя, если он спит в waitpid и ждёт нас. */
    if (me->process->parent_pid != 0 && task_list) {
        task_t* p = task_list;
        do {
            if (p->process && p->process->pid == me->process->parent_pid) {
                p->sig_pending |= (1ULL << 17 /* SIGCHLD */);
                if (p->waiting &&
                    (p->wait_for == 0 || p->wait_for == me->process->pid)) {
                    p->running = true;
                    p->state = TASK_STATE_RUNNABLE;
                    p->waiting = false;
                    sched_enqueue(p);
                }
                break;
            }
            p = p->next;
        } while (p && p != task_list);
    }

    yield();
    while (1)
        ;
}

/* Очистка ресурсов зомби-потока за O(1) за счет двусвязного глобального списка! */
static void task_reap(task_t* z) {
    if (task_list) {
        if (z->next == z) { // Единственный процесс в глобальном списке
            task_list = NULL;
        } else {
            z->prev->next = z->next;
            z->next->prev = z->prev;
            if (task_list == z) {
                task_list = z->next;
            }
        }
    }

    if (z->kstack_at_bottom) {
        uint64_t base_phys = z->kstack_at_bottom - 16384 - hhdm_offset;
        for (int p = 0; p < 4; p++)
            pmm_free((void*)(base_phys + (uint64_t)p * 4096));
    }

    // Освобождаем ассоциированный процесс
    if (z->process) {
        kfree(z->process);
    }

    kfree(z);
}

task_t* task_by_id(uint64_t pid) {
    if (!task_list) return NULL;
    task_t* t = task_list;
    do {
        if (t->process && t->process->pid == pid) return t;
        t = t->next;
    } while (t && t != task_list);
    return NULL;
}

int64_t task_waitpid_ex(uint64_t pid, int* status_out, int nohang) {
    while (1) {
        bool have_child = false;
        task_t* found = NULL;

        if (task_list) {
            task_t* t = task_list;
            do {
                if (t->process && t->process->parent_pid == current_task->process->pid &&
                    (pid == 0 || t->process->pid == pid)) {
                    have_child = true;
                    if (t->process->zombie) {
                        found = t;
                        break;
                    }
                }
                t = t->next;
            } while (t && t != task_list);
        }

        if (found) {
            uint64_t cid = found->process->pid;
            if (status_out)
                *status_out = found->process->exit_code;
            task_reap(found);
            return (int64_t)cid;
        }

        if (!have_child)
            return -1;

        if (nohang)
            return 0;

        if (current_task->sig_pending & ~current_task->sig_blocked &
            ~(1ULL << 17 /* SIGCHLD */))
            return -1;

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

void task_kill_self() {
  if (current_task->process && current_task->process->pid == 1)
    return; 

  if (current_task->process && current_task->process->fdt) {
    fd_table_destroy(current_task->process->fdt);
    current_task->process->fdt = NULL;
  }

  if (current_task->process && current_task->process->cr3 != 0 && current_task->process->cr3 != kernel_cr3) {
    vmm_destroy_address_space(current_task->process->cr3);
  }

  current_task->running = false;

  printf("[TASK] Process %u terminated and memory reclaimed.\n",
         (uint32_t)(current_task->process ? current_task->process->pid : current_task->id));

  yield();
  while (1)
    ; 
}

void task_list_all() {
  task_t *start = task_list;
  task_t *curr = start;

  term_print("\e[33m PID   STATE       CR3          MEM_BRK\e[0m\n");
  do {
    char buf[128];
    const char *state = curr->running ? "RUNNING" : "STOPPED";
    sprintf(buf, " %d     %s     %x   %x\n", 
            (uint32_t)(curr->process ? curr->process->pid : curr->id), 
            state,
            (uint32_t)(curr->process ? curr->process->cr3 : curr->cr3), 
            (uint32_t)(curr->process ? curr->process->brk : curr->brk));
    term_print(buf);
    curr = curr->next;
  } while (curr != start);
}

bool task_terminate_by_pid(uint64_t pid) {
  if (pid == 1) {
    term_print("TASK: Cannot kill kernel init process!\n");
    return false;
  }

  if (!task_list) return false;
  task_t *curr = task_list;
  do {
    if (curr->process && curr->process->pid == pid) {
      curr->running = false;
      if (curr->process->cr3 != 0) {
        // vmm_destroy_address_space(curr->process->cr3);
      }
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
    if (!curr) break; 
  } while (curr != task_list);

  term_print("TASK: PID not found.\n");
  return false;
}

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
    if (curr->process && curr->process->pid != 1 && curr != current_task && curr->running) {
      curr->running = false;
      {
        extern volatile uint64_t fg_app_pid;
        if (fg_app_pid == curr->process->pid) fg_app_pid = 0;
      }
      n++;
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
      out->pid = curr->process ? curr->process->pid : curr->id;
      out->cr3 = curr->process ? curr->process->cr3 : curr->cr3;
      out->brk = curr->process ? curr->process->brk : curr->brk;
      out->running = curr->running;
      return true;
    }
    i++;
    curr = curr->next;
  } while (curr && curr != start);
  return false;
}

unsigned current_task_id_for_panic(void) {
    return (current_task && current_task->process) ? (unsigned)current_task->process->pid : 0u;
}