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

// === Структура кадра стека при прерывании / системном вызове ===
typedef struct stack_frame {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t interrupt_number;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) stack_frame_t;

// === Структура снимка состояния задачи (для ps / монитора) ===
typedef struct task_snapshot {
    uint64_t pid;
    uint64_t cr3;
    uint64_t brk;
    bool running;
} task_snapshot_t;

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

// Вспомогательный макрос для FPU-контекста
#define task_fpu_area(t) (void*)(((uint64_t)(t)->fpu_state + 15) & ~(uint64_t)15)

// === Опережающие объявления ===
typedef struct sysv_args {
    int argc;
    uint64_t *argv_user;
    int envc;
    uint64_t *envp_user;
    uint64_t at_phdr;
    uint64_t at_phent;
    uint64_t at_phnum;
    uint64_t at_entry;
} sysv_args_t;

// === Глобальные переменные экспорта ===
extern task_t* current_task;

// === Прототипы функций API управления задачами и процессами ===
void task_init(void);
void task_create(void (*entry)(), uint64_t arg1, uint64_t arg2, uint64_t cr3);
task_t* task_create_full(void (*entry)(), uint64_t arg1, uint64_t arg2, uint64_t cr3, const sysv_args_t *sv);

uint64_t schedule(uint64_t current_rsp);
void yield(void);
bool task_exec(char* full_command);
void task_set_fs_base(uint64_t v);

uint64_t task_fork(struct stack_frame* parent_frame);
void task_exit_current(int code);
task_t* task_by_id(uint64_t pid);

int64_t task_waitpid(uint64_t pid, int* status_out);
int64_t task_waitpid_ex(uint64_t pid, int* status_out, int nohang);

void task_kill_self(void);
void task_list_all(void);
bool task_terminate_by_pid(uint64_t pid);
task_t* task_get_list_head(void);

void task_kill_all_user(void);
int task_kill_all_user_count(void);
bool task_snapshot_at(int idx, task_snapshot_t *out);
unsigned current_task_id_for_panic(void);

// === Системные функции окружения процесса ===
struct fd_table *task_current_fdt(void);
const char *task_current_cwd(void);
int task_chdir(const char *path);
void task_resolve_fs_path(const char *in, char *out, int outsz);

bool task_load_image(const char *path, char *const argv[], int argc,
                     char *const envp[], int envc,
                     uint64_t *out_entry, uint64_t *out_user_rsp,
                     uint64_t *out_argv_ptr, uint64_t *out_envp_ptr,
                     uint64_t *out_cr3, uint64_t *out_fs_base);

#endif /* TASK_H */