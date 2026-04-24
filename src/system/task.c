#include "task.h"
#include "gdt.h"
#include "pmm.h"

extern void term_print(const char* str);
#include "memory.h"
#include "../libc/string.h"
#include "../fs/elf.h"
#include "../fs/fat32.h"
#include "vmm.h"

// MSR addresses for FS/GS base
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

void task_init() {
    __asm__ volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
    current_task = (task_t*)kmalloc(sizeof(task_t));
    current_task->cr3 = kernel_cr3;
    current_task->id = next_pid++;
    current_task->running = true;

    // КРИТИЧНО: У ПЕРВОЙ задачи ядра тоже должен быть стек для TSS
    current_task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384; 
    
    current_task->next = current_task;
    task_list = current_task;
}
// Обновленная функция создания задачи
void task_create(void (*entry)(), uint64_t arg1, uint64_t arg2, uint64_t cr3) {
    task_t* new_task = (task_t*)kmalloc(sizeof(task_t));
    new_task->id = next_pid++;
    new_task->running = true;
    new_task->cr3 = cr3;
    new_task->fs_base = 0; // Initialize FS base

    // Kernel stack
    void* kstack_phys = pmm_alloc_continuous(4); 
    uint64_t kstack_virt = (uint64_t)kstack_phys + hhdm_offset;
    memset((void*)kstack_virt, 0, 16384);
    new_task->kstack_at_bottom = kstack_virt + 16384;

    uint64_t user_stack_virt = 0x70000000000;
    if (cr3 != 0) {
        // Map 8 pages of stack (32KB) for reliability
        void* ustack_phys = pmm_alloc_continuous(8);
        for (int i = 0; i < 8; i++) {
            vmm_map((page_table_t*)VIRT(cr3), 
                    user_stack_virt + (i * 4096), 
                    (uint64_t)ustack_phys + (i * 4096), 
                    PTE_USER | PTE_WRITABLE | PTE_PRESENT);
        }
        
        // Allocate TLS (Thread Local Storage) for mlibc
        // mlibc expects fs:0 to contain a pointer to thread data
        // Layout: [TCB struct][thread data]
        // fs:0 should point to thread_data (after TCB)
        uint64_t tls_virt = 0x60000000000;
        void* tls_phys = pmm_alloc();
        vmm_map((page_table_t*)VIRT(cr3), tls_virt, (uint64_t)tls_phys, 
                PTE_USER | PTE_WRITABLE | PTE_PRESENT);
        memset((void*)VIRT(tls_phys), 0, 4096);
        
        // Set up TCB: first 8 bytes of TLS area contain pointer to thread_data
        // TCB is typically small, thread_data follows
        // fs:0 = &thread_data = tls_virt + sizeof(TCB)
        // For now, assume TCB size is 64 bytes (typical for mlibc)
        uint64_t* tcb = (uint64_t*)VIRT(tls_phys);
        tcb[0] = tls_virt + 64;  // fs:0 points to thread_data area
        new_task->fs_base = tls_virt;
    }

    stack_frame_t* frame = (stack_frame_t*)(new_task->kstack_at_bottom - sizeof(stack_frame_t));
    memset(frame, 0, sizeof(stack_frame_t));
    
    frame->rip = (uint64_t)entry;
    frame->rdi = arg1; 
    frame->rsi = arg2;
    frame->rflags = 0x202; // IF = 1
    
    if (cr3 == 0) {
        frame->cs = 0x08; frame->ss = 0x10;
        frame->rsp = kstack_virt + 16000;
    } else {
        frame->cs = 0x23; // User Code
        frame->ss = 0x1B; // User Data
        // x86_64 ABI: RSP must be 8 mod 16 at function entry
        // (CALL pushes 8-byte return addr, making RSP 0 mod 16 before push rbp)
        // IRETQ doesn't push return addr, so we pre-align to 8 mod 16
        frame->rsp = ((user_stack_virt + (8 * 4096)) & ~0xF) - 8;
    }
    
    new_task->rsp = (uint64_t)frame;
    new_task->next = task_list->next;
    task_list->next = new_task;
}

// task.c
uint64_t schedule(uint64_t current_rsp) {
    tick++;
    if (!current_task) return current_rsp;
    
    // Save state
    current_task->rsp = current_rsp;
    
    // SWITCH TO NEXT TASK
    current_task = current_task->next;

    // First load CR3
    uint64_t new_cr3 = (current_task->cr3 == 0) ? kernel_cr3 : current_task->cr3;
    
    // CRITICAL: We change CR3. From now on we see ONLY what
    // is mapped in the new task's tables.
    __asm__ volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");

    // Now update TSS (stack for interrupts from Ring 3)
    gdt_set_tss_stack(current_task->kstack_at_bottom);
    
    // Set FS base for TLS (Thread Local Storage) - needed for mlibc
    if (current_task->fs_base != 0) {
        wrmsr(IA32_FS_BASE_MSR, current_task->fs_base);
    }
    
    return current_task->rsp;
}
void yield(void) {
    __asm__ volatile ("int $32"); // Вызываем обработчик таймера (IRQ0)
}

bool task_exec(char* full_command) {
    // 1. Разбиваем строку на токены (название файла и аргументы)
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

    // 2. ЧИТАЕМ ФАЙЛ С ДИСКА
    uint32_t elf_size = 0;
    // argv[0] — это имя файла (например "SNAKE.ELF")
    uint8_t* elf_raw = (uint8_t*)fat32_read_file(argv[0], &elf_size);

    if (!elf_raw) {
        term_print("EXEC: File not found: ");
        term_print(argv[0]);
        term_print("\n");
        kfree(cmd_copy);
        return false;
    }

    // 3. Проверяем, что это ELF
    Elf64_Ehdr* header = (Elf64_Ehdr*)elf_raw;
    if (memcmp(header->e_ident, "\x7f\x45\x4c\x46", 4) != 0) {
        term_print("EXEC: Not a valid ELF file\n");
        kfree(elf_raw);
        kfree(cmd_copy);
        return false;
    }

    // 4. Создаем адресное пространство (VMM)
    page_table_t* proc_pml4 = vmm_create_address_space();
    uint64_t phys_pml4 = PHYS(proc_pml4);

    // 5. Загружаем сегменты ELF в виртуальную память процесса
    Elf64_Phdr* phdr = (Elf64_Phdr*)(elf_raw + header->e_phoff);
    for (int i = 0; i < header->e_phnum; i++) {
        if (phdr[i].p_type == 1) { // PT_LOAD
            uint64_t pages = (phdr[i].p_memsz + 4095) / 4096;
            void* phys_mem = pmm_alloc_continuous(pages);
            
            term_print("EXEC: Mapping segment ");
            // Simple number printing
            char num_buf[4] = { '0' + i, 0 };
            term_print(num_buf);
            term_print(" vaddr=");
            // Print vaddr in hex (simplified)
            static char hex_tmp[20];
            uint64_t v = phdr[i].p_vaddr;
            int pos = 18;
            hex_tmp[19] = 0;
            for (int j = 0; j < 16; j++) { hex_tmp[pos--] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; }
            hex_tmp[pos] = 'x'; hex_tmp[--pos] = '0';
            term_print(&hex_tmp[pos]);
            term_print(" pages=");
            num_buf[0] = '0' + (pages / 100); num_buf[1] = '0' + ((pages / 10) % 10); num_buf[2] = '0' + (pages % 10); num_buf[3] = 0;
            term_print(num_buf);
            term_print("\n");
            
            // Map each page of segment as USER with PRESENT flag
            for (uint64_t p = 0; p < pages; p++) {
                vmm_map(proc_pml4, 
                        phdr[i].p_vaddr + (p * 4096), 
                        (uint64_t)phys_mem + (p * 4096), 
                        PTE_PRESENT | PTE_USER | PTE_WRITABLE);
            }
            
            // Copy data from ELF to allocated physical memory via HHDM
            memset((void*)VIRT(phys_mem), 0, phdr[i].p_memsz);
            memcpy((void*)VIRT(phys_mem), elf_raw + phdr[i].p_offset, phdr[i].p_filesz);
        }
    }

    // 6. Создаем задачу
    term_print("EXEC: Starting Ring 3 process...\n");
    // Передаем argc в RDI, argv пока оставим 0
    uint64_t user_argv_page = 0x80000000; 
    void* phys_argv = pmm_alloc();
    vmm_map(proc_pml4, user_argv_page, (uint64_t)phys_argv, PTE_USER | PTE_WRITABLE);
    
    // Формируем структуру в физической памяти (ядро пишет через HHDM)
    uint64_t* user_argv_array = (uint64_t*)VIRT(phys_argv); 
    char* user_string_area = (char*)VIRT(phys_argv) + 128; // Строки положим через 128 байт
    
    uint64_t current_string_offset = 128; // Смещение относительно начала страницы

    // Заполняем массив указателей и копируем сами строки
    for (int i = 0; i < argc; i++) {
        // Записываем УКАЗАТЕЛЬ (виртуальный адрес в памяти юзера!)
        user_argv_array[i] = user_argv_page + current_string_offset;
        
        // Копируем саму строку (через HHDM)
        strcpy(user_string_area, argv[i]);
        
        // Сдвигаем указатели для следующей строки
        int len = strlen(argv[i]) + 1; // +1 для нуль-терминатора
        user_string_area += len;
        current_string_offset += len;
    }
    // Последний элемент массива argv должен быть NULL по стандарту C
    user_argv_array[argc] = 0; 

    term_print("EXEC: Starting Ring 3 process with arguments...\n");
    
    // Pass argc in RDI (arg1), and address of ARRAY in RSI (arg2)
    task_create((void(*)())header->e_entry, (uint64_t)argc, user_argv_page, phys_pml4);
    
    // Set FS base for TLS immediately - needed before user code runs
    // Find the newly created task (it's at task_list->next)
    task_t* new_task = task_list->next;
    if (new_task && new_task->fs_base != 0) {
        // Load FS segment selector (0x1B = User Data segment with RPL=3)
        __asm__ volatile("mov $0x1B, %%ax; mov %%ax, %%fs" ::: "ax");
        wrmsr(IA32_FS_BASE_MSR, new_task->fs_base);
        term_print("EXEC: FS base set for TLS\n");
    }
    
    // 7. Cleanup temporary buffers
    kfree(elf_raw);
    kfree(cmd_copy);
    return true;
}