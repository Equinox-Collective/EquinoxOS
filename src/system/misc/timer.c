// timer.c
#include "timer.h"
#include "../core/io.h"

// volatile крайне важен, чтобы компилятор не оптимизировал проверки в sleep()
volatile uint32_t tick = 0;

void timer_callback() {
  tick++; // Просто инкремент
}

// В timer.c
void init_timer(uint32_t freq) {
  // Константа PIT: 1193182 Гц.
  // Для 100 Гц делитель будет 11931 (0x2E9B)
  uint32_t divisor = 1193182 / freq;

  outb(0x43, 0x36); // Командный байт: канал 0, lo/hi байт, режим 3
  outb(0x40, (uint8_t)(divisor & 0xFF));
  outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

void sleep(uint32_t ms) {
  /* PIT инициализирован на 1 кГц (см. kernel.c::kmain), поэтому 1 тик = 1 мс.
   * Раньше тут было `ms / 10`, что было сделано под гипотетический 100 Гц
   * PIT, а реальная частота была 50 Гц — итого sleep() врал в 2-20 раз. */
  uint32_t start_tick = tick;
  while (tick < start_tick + ms) {
    __asm__ __volatile__(
        "pause"); // 'pause' лучше для циклов ожидания, чем 'hlt' внутри
  }
}