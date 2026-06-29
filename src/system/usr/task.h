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

#define MAX_FD 32

typedef enum {
    TASK_STATE_RUNNABLE,  // Готов к выполнению (находится в Run Queue)
    TASK_STATE_SLEEPING,  // Спит по таймеру (находится в Sleep Queue)
    TASK_STATE_BLOCKED,   // Заблокирован на мьютексе/семафоре/ожидании ввода (вне планировщика)
    TASK_STATE_ZOMBIE     // Мертв, ожидает waitpid (вне планировщика)
} task_state_t;

typedef struct process {
    uint64_t pid;               // Идентификатор процесса
    uint64_t parent_pid;        // PID родительского процесса
    uint64_t pgid;
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
    bool running;               // Совместимость
    uint64_t kstack_at_bottom;  // Ядерный стек этого потока (для прерываний)
    uint64_t fs_base;           // Базовый адрес TLS этого потока

    // Ссылка на родительский процесс (где лежат общие ресурсы)
    process_t *process;

    uint64_t sleep_until;       // Таймер сна потока
    uint8_t fpu_state[512] __attribute__((aligned(16)));
    bool waiting;        // Флаг ожидания завершения дочернего процесса
    uint64_t wait_for;

    // Сигналы (доставляются конкретному потоку)
    uint64_t sig_pending;
    uint64_t sig_blocked;
    uint64_t sig_handlers[32];
    uint64_t sig_restorer;

    // Ссылки очереди планировщика
    struct task *next;
    struct task *prev;
} task_t;

static inline void* task_fpu_area(task_t* t) {
    return (void*)(((uint64_t)t->fpu_state + 15) & ~(uint64_t)15);
}

/* Кладёт в fpu-область задачи эталонное «чистое» состояние (fninit +
 * MXCSR=0x1F80). Вызывать при создании каждой задачи. */
void task_fpu_init_area(task_t* t);

extern task_t* current_task; 
void task_init();
uint64_t schedule(uint64_t current_rsp); // Вызывается из ассемблера
void yield(void);
void task_create(void (*entry)(), uint64_t arg1, uint64_t arg2, uint64_t cr3);

/* --- Этап 6b: System V AMD64 initial stack для musl / Linux-ABI бинарей ----
 * Описывает данные для построения SysV-кадра на вершине пользовательского
 * стека (argc / argv[] / NULL / envp[] / NULL / auxv / AT_NULL). Указатели
 * argv_user/envp_user — это УЖЕ пользовательские адреса строк (напр. в argv-
 * странице 0xB0000000), строки на стек НЕ копируются. Родной crt0 этот кадр
 * игнорирует (читает argc/argv из регистров), поэтому детект ABI не нужен. */
typedef struct {
    int argc;
    const uint64_t *argv_user;   /* argc пользовательских указателей на строки */
    int envc;
    const uint64_t *envp_user;   /* envc указателей (может быть NULL при envc=0) */
    uint64_t at_phdr;            /* адрес Program Headers в памяти процесса      */
    uint64_t at_phent;           /* размер одной записи PH                        */
    uint64_t at_phnum;           /* число записей PH                              */
    uint64_t at_entry;           /* точка входа ELF (e_entry)                     */
} sysv_args_t;

/* Полный конструктор задачи. sv != NULL и cr3 != 0 -> дополнительно строит
 * SysV-кадр и ставит на него user-rsp. Возвращает созданную задачу. */
task_t* task_create_full(void (*entry)(), uint64_t arg1, uint64_t arg2,
                         uint64_t cr3, const sysv_args_t *sv);
bool task_exec(char* full_command);

/* Этап 1b — execve. task_load_image() загружает ELF в НОВОЕ адресное
 * пространство и готовит стек/TLS/argv (argv[] уже скопирован в память ядра).
 * Переключение cr3 и освобождение старого пространства делает вызывающий. */
bool task_load_image(const char *path, char *const argv[], int argc,
                     char *const envp[], int envc,
                     uint64_t *out_entry, uint64_t *out_user_rsp,
                     uint64_t *out_argv_ptr, uint64_t *out_envp_ptr,
                     uint64_t *out_cr3, uint64_t *out_fs_base);
void task_set_fs_base(uint64_t v);

/* Этап 3: cwd текущего процесса (логический абсолютный путь). */
const char *task_current_cwd(void);
/* Сменить cwd текущего процесса на нормализованный target (логический
 * абсолютный путь). Возвращает 0 при успехе, -1 при ошибке. */
int task_chdir(const char *path);
/* Разрешить пользовательский путь в имя для VFS (ext2): нормализует относ.
 * cwd текущего процесса и убирает ведущий '/'. "/" -> "". */
void task_resolve_fs_path(const char *in, char *out, int outsz);

/* --- Этап 1: процессная модель ------------------------------------------- *
 * task_fork(): клонирует текущий процесс. parent_frame — сохранённый кадр
 * прерывания (stack_frame_t*) из обработчика int 0x80, т.е. регистры
 * родителя на момент syscall. Возвращает pid ребёнка родителю; ребёнок
 * "вернётся" из того же int 0x80 со значением 0 в rax. */
uint64_t task_fork(stack_frame_t* parent_frame);

/* Помечает текущий процесс зомби с кодом code, освобождает его адресное
 * пространство и будит родителя, если тот ждёт в waitpid. Не возвращается
 * (уходит в планировщик). Вызывается из обработчика SYS_EXIT. */
void task_exit_current(int code);

/* Ждёт завершения ребёнка. pid > 0 — конкретный ребёнок, 0 — любой.
 * Пишет код выхода в *status_out (kernel-указатель). Возвращает pid
 * завершившегося ребёнка, либо -1 если у процесса нет таких детей (ECHILD).
 * Блокирует (через yield) пока подходящий ребёнок не станет зомби. */
int64_t task_waitpid(uint64_t pid, int* status_out);
int64_t task_waitpid_ex(uint64_t pid, int* status_out, int nohang);
task_t* task_by_id(uint64_t pid);

/* Этап 2: вернуть таблицу дескрипторов текущего процесса (для fs/fd.c). */
struct fd_table *task_current_fdt(void);

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

// Удобная обёртка: то же самое, но без возврата кол-ва.
void task_kill_all_user(void);

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