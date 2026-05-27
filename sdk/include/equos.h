#ifndef EQUOS_H
#define EQUOS_H

#include <stdint.h>

#define SYS_PRINT 1
#define SYS_READ_FILE 2
#define SYS_WRITE_FILE 3
#define SYS_READ_DIR 4
#define SYS_DRAW_BUFFER 5
#define SYS_GET_TIME 6
#define SYS_GET_MOUSE_FULL 7
#define SYS_GET_SCANCODE 9
#define SYS_EXIT 10
#define SYS_YIELD 11
#define SYS_GET_FONT 12
#define SYS_SLEEP 13
#define SYS_MMAP 14
#define SYS_BRK 15
#define SYS_WRITE 16
#define SYS_AUDIO_PLAY 20
#define SYS_AUDIO_SET_RATE 21
#define SYS_MAP_PHYS 30
#define SYS_SHM_GET 31
#define SYS_GET_VESA_INFO 32
#define SYS_SET_WINDOW_POS 36
#define SYS_NET_DNS_RESOLVE 40
#define SYS_NET_HTTP_GET 41
#define SYS_GET_USED_MEM 34
#define SYS_GET_TOTAL_MEM 35
#define SYS_EXEC 50

/* --- IPC (added in HAL/sync/ipc patch set) --- */
#define SYS_PIPE_CREATE 60
#define SYS_PIPE_READ   61
#define SYS_PIPE_WRITE  62
#define SYS_PIPE_CLOSE  63
#define SYS_MQ_CREATE   64
#define SYS_MQ_SEND     65
#define SYS_MQ_RECV     66
#define SYS_MQ_CLOSE    67

/* --- Task introspection / control (ps / kill / killall from ring 3) --- */
#define SYS_TASK_INFO   70  /* (idx, out_buf) -> 1 if filled, 0 at end */
#define SYS_TASK_KILL   71  /* (pid)         -> 1 ok, 0 fail           */
#define SYS_TASK_KILLALL 72 /* ()            -> number of tasks killed */

/* --- Ring-0 shell bridge --------------------------------------------------
 * Выполнить готовую строку командой ring-0 шелла (см. shellsyntx.h /
 * shell.c). Вывод собирается во временный sink и копируется в user-buf.
 *
 *   rdi = const char *line      (NUL-terminated, < 256 байт)
 *   rsi = char *user_out_buf    (куда писать вывод, NUL-terminated)
 *   rdx = uint64_t out_buf_len  (размер user_out_buf, в байтах)
 *
 * Возвращает количество байт, записанных в out_buf (без NUL).
 */
#define SYS_SHELL_EXEC  73
#define SYS_GET_FG_APP  74   /* () -> PID активного foreground-app, 0 если нет */

/* --- Socket API (phase 1 — see sdk/include/sys/socket.h for wrappers) --- */
#define SYS_SOCKET       80  /* ()                       -> int fd            */
#define SYS_CONNECT      81  /* (fd, ip_be, port)        -> int rc            */
#define SYS_SEND         82  /* (fd, buf, len)           -> int sent          */
#define SYS_RECV         83  /* (fd, buf, len)           -> int recvd         */
#define SYS_CLOSE_SOCK   84  /* (fd)                     -> int rc            */
#define SYS_SETSOCKOPT   85  /* (fd, lvl, opt, val, len) -> int rc            */

/* --- Entropy --------------------------------------------------------------
 * Fill a userspace buffer with cryptographically usable random bytes
 * (RDRAND-backed; soft fallback on ancient CPUs). See sdk/include/sys/random.h.
 *
 *   rdi = void    *buf
 *   rsi = uint32_t len
 *   rdx = uint32_t flags  (reserved, must be 0)
 *
 * Returns 0 on success, -1 on bad args. Never partial.                    */
#define SYS_GETRANDOM   86

typedef struct {
  uint64_t pid;
  uint64_t cr3;
  uint64_t brk;
  uint32_t running;   /* 1 = RUNNING, 0 = STOPPED */
  uint32_t _pad;
} sys_task_info_t;

// Переименовали в _syscall и всегда принимаем 5 аргументов + номер
static inline uint64_t _syscall(uint64_t num, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  uint64_t ret;
  __asm__ volatile("mov %1, %%rax; "
                   "mov %2, %%rdi; "
                   "mov %3, %%rsi; "
                   "mov %4, %%rdx; "
                   "mov %5, %%rcx; "
                   "mov %6, %%r8; "
                   "int $0x80; "
                   "mov %%rax, %0; "
                   : "=r"(ret)
                   : "r"(num), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
                   : "rax", "rdi", "rsi", "rdx", "rcx", "r8", "memory");
  return ret;
}

static inline void *get_system_font() {
  return (void *)_syscall(SYS_GET_FONT, 0, 0, 0, 0, 0);
}

static inline void write_file(const char *name, void *buf, uint32_t size) {
  _syscall(SYS_WRITE_FILE, (uint64_t)name, (uint64_t)buf, size, 0, 0);
}

static inline void sys_sleep(uint32_t ms) {
  _syscall(SYS_SLEEP, ms, 0, 0, 0, 0);
}

static inline void sleep(uint32_t ms) { sys_sleep(ms); }

static inline void sys_audio_submit(void *buffer, uint32_t size) {
  _syscall(SYS_AUDIO_PLAY, (uint64_t)buffer, (uint64_t)size, 0, 0, 0);
}

static inline void sys_exit(int code) { _syscall(SYS_EXIT, code, 0, 0, 0, 0); }
static inline void sys_yield() { _syscall(SYS_YIELD, 0, 0, 0, 0, 0); }
static inline void *sys_get_font() {
  return (void *)_syscall(SYS_GET_FONT, 0, 0, 0, 0, 0);
}

static inline uint32_t net_dns_resolve(const char *hostname) {
  return (uint32_t)_syscall(SYS_NET_DNS_RESOLVE, (uint64_t)hostname, 0, 0, 0,
                            0);
}

static inline void *net_http_get(uint32_t ip, uint32_t *out_size) {
  return (void *)_syscall(SYS_NET_HTTP_GET, (uint64_t)ip, (uint64_t)out_size, 0,
                          0, 0);
}

static inline uint64_t sys_get_used_mem() {
  return _syscall(SYS_GET_USED_MEM, 0, 0, 0, 0, 0);
}

static inline uint64_t sys_get_total_mem() {
  return _syscall(SYS_GET_TOTAL_MEM, 0, 0, 0, 0, 0);
}

static inline int sys_exec(const char *cmd) {
  return (int)_syscall(SYS_EXEC, (uint64_t)cmd, 0, 0, 0, 0);
}

/* ----- IPC userspace wrappers ----- */
static inline int sys_pipe_create(void) {
  return (int)_syscall(SYS_PIPE_CREATE, 0, 0, 0, 0, 0);
}
static inline int sys_pipe_read(int id, void *buf, uint32_t size) {
  return (int)_syscall(SYS_PIPE_READ, (uint64_t)id, (uint64_t)buf,
                       (uint64_t)size, 0, 0);
}
static inline int sys_pipe_write(int id, const void *buf, uint32_t size) {
  return (int)_syscall(SYS_PIPE_WRITE, (uint64_t)id, (uint64_t)buf,
                       (uint64_t)size, 0, 0);
}
static inline void sys_pipe_close(int id) {
  _syscall(SYS_PIPE_CLOSE, (uint64_t)id, 0, 0, 0, 0);
}

static inline int sys_mq_create(uint32_t msg_size) {
  return (int)_syscall(SYS_MQ_CREATE, (uint64_t)msg_size, 0, 0, 0, 0);
}
static inline int sys_mq_send(int id, const void *buf, uint32_t prio) {
  return (int)_syscall(SYS_MQ_SEND, (uint64_t)id, (uint64_t)buf,
                       (uint64_t)prio, 0, 0);
}
static inline int sys_mq_recv(int id, void *buf) {
  return (int)_syscall(SYS_MQ_RECV, (uint64_t)id, (uint64_t)buf, 0, 0, 0);
}
static inline void sys_mq_close(int id) {
  _syscall(SYS_MQ_CLOSE, (uint64_t)id, 0, 0, 0, 0);
}
#endif
