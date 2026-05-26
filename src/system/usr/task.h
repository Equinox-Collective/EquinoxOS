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

// Доступ к глобальному списку задач (циклический односвязный).
// Возвращает голову, либо NULL, если планировщик ещё не поднят.
// Использовать с осторожностью: список — кольцо, идти до тех пор,
// пока next != head.
task_t* task_get_list_head(void);

// Аккуратно убивает все пользовательские задачи кроме idle/init
// (id == 1). Используется в `killall` и SUPER+ALT+F10. Сама задача,
// если она пользовательская, должна позаботиться о yield()/kill_self
// отдельно — здесь мы только метим running=false; планировщик
// доразберётся в ближайшем тике.
// Возвращает кол-во убитых пользовательских задач.
int task_kill_all_user_count(void);

// Снимок задачи по индексу в кольце (начиная с головы, индексация 0..N-1).
// Заполняет out_pid/out_cr3/out_brk/out_running и возвращает true, если
// задача с таким индексом существует. iff false — кольцо короче.
typedef struct {
    uint64_t pid;
    uint64_t cr3;
    uint64_t brk;
    bool running;
} task_snapshot_t;
bool task_snapshot_at(int idx, task_snapshot_t *out);

#endif