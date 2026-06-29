#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>

// Состояния потока
typedef enum {
    TASK_STATE_RUNNABLE,  // Готов к выполнению (находится в Run Queue)
    TASK_STATE_SLEEPING,  // Спит по таймеру (находится в Sleep Queue)
    TASK_STATE_BLOCKED,   // Заблокирован на мьютексе/семафоре/ожидании ввода (вне планировщика)
    TASK_STATE_ZOMBIE     // Мертв, ожидает waitpid (вне планировщика)
} task_state_t;

// === Контейнер ресурсов Процесса (Process Control Block) ===
typedef struct process {
    uint64_t pid;               // Идентификатор процесса (PID)
    uint64_t parent_pid;        // PID родительского процесса
    uint64_t pgid;              // Группа процессов (PGID)
    uint64_t cr3;               // Страничная таблица процесса (общее адресное пространство)
    uint64_t brk;               // Граница кучи процесса (общее выделение памяти)
    char cwd[256];              // Общий рабочий каталог процессов
    struct fd_table *fdt;       // Общая таблица файловых дескрипторов
    int exit_code;              // Код завершения процесса
    bool zombie;                // Флаг зомби-состояния
} process_t;

// === Поток исполнения (Thread Control Block) ===
typedef struct task {
    uint64_t rsp;               // Сохраненный указатель стека потока
    uint64_t id;                // Идентификатор потока (TID)
    task_state_t state;         // Текущее состояние потока
    bool running;               // Обратная совместимость

    bool waiting;               // Ожидание в waitpid
    uint64_t wait_for;          // Кого именно ждем в waitpid

    uint64_t kstack_at_bottom;  // Ядерный стек этого потока (для прерываний)
    uint64_t fs_base;           // Базовый адрес TLS этого потока

    // Ссылка на родительский процесс (где лежат общие ресурсы)
    process_t *process;

    uint64_t sleep_until;       // Таймер сна потока
    uint8_t fpu_state[512] __attribute__((aligned(16)));

    // Сигналы (доставляются конкретному потоку)
    uint64_t sig_pending;
    uint64_t sig_blocked;
    uint64_t sig_handlers[32];
    uint64_t sig_restorer;

    // Очереди планировщика (ИСПОЛЬЗУЮТСЯ ИСКЛЮЧИТЕЛЬНО В sched.c)
    struct task *sched_next;
    struct task *sched_prev;

    // Глобальный список всех потоков в системе (ИСПОЛЬЗУЮТСЯ В task.c / signal.c / waitpid)
    struct task *next;
    struct task *prev;
} task_t;

// Вспомогательный макрос для FPU-контекста, который у вас используется в ядре
#define task_fpu_area(t) (void*)(((uint64_t)(t)->fpu_state + 15) & ~(uint64_t)15)

#endif /* TASK_H */