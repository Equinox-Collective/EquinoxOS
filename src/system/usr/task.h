#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>

// Состояние всех регистров x86_64 при прерывании
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t interrupt_number, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) stack_frame_t;

typedef struct task {
    uint64_t rsp;
    uint64_t kstack_at_bottom; // <--- ДОБАВЬ ЭТО (Верхушка стека ядра)
    uint64_t cr3;
    uint64_t fs_base;          // FS base for TLS (Thread Local Storage)
    struct task* next;
    uint64_t id;
    bool running;
    uint64_t sleep_until;
    uint64_t brk;
} task_t;

extern task_t* current_task; 
void task_init();
uint64_t schedule(uint64_t current_rsp); // Вызывается из ассемблера
void yield(void);
void task_create(void (*entry)(), uint64_t arg1, uint64_t arg2, uint64_t cr3);
bool task_exec(char* full_command);
void task_kill_self();
bool task_terminate_by_pid(uint64_t pid);
void task_list_all();

#endif