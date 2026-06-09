#ifndef UREGS_H
#define UREGS_H

#include <stdint.h>

/* Кадр, который стаб `syscall_interrupt_asm` (int 0x80) строит на ядерном
 * стеке и передаёт в syscall_handler(syscall_regs_t*). Содержит ВЕСЬ набор
 * GP-регистров пользователя ПЛЮС iretq-кадр (rip/cs/rflags/rsp/ss) в конце —
 * поэтому, переписав rip/rsp/rdi/cs/ss/rflags, обработчик может перенаправить
 * возврат в пользователя (используется execve и доставкой сигналов Этап 4).
 *
 * ВНИМАНИЕ: порядок и смещения полей ДОЛЖНЫ совпадать с порядком push в
 * syscall_interrupt_asm (interrupt.asm). Не менять без правки ассемблера. */
typedef struct
{
  uint64_t rax; // syscall_number
  uint64_t r9;
  uint64_t r8;
  uint64_t rbx;
  uint64_t rcx;
  uint64_t rdx;
  uint64_t rsi;
  uint64_t rdi;
  uint64_t rbp;
  uint64_t r10, r11, r12, r13, r14, r15;
  uint64_t rip, cs, rflags, rsp, ss;
} syscall_regs_t;

#endif
