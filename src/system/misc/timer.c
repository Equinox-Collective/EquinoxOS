// Вставьте в начало timer.c:
#include "../usr/sched.h"
#include "timer.h"
#include <stdint.h>

void timer_callback() {
  tick++;

  // Быстрое пробуждение готовых потоков
  sched_timer_tick(tick);

  if (nyan_boot_active) {
    nyan_boot_anim_frame();
  }
  
  if ((tick & 0x0F) == 0) {
    tcp_tick_with_iface(tick);
  }
}

void sleep(uint32_t ms) {
  // Вместо пустого цикла ожидания, усыпляем ПОТОК, освобождая CPU для других задач!
  if (current_task) {
      sched_make_sleep(current_task, tick + ms);
      sched_yield(); // Передаём управление
  } else {
      // Фолбэк для ранних этапов инициализации ядра
      uint32_t start_tick = tick;
      while (tick < start_tick + ms) {
          __asm__ __volatile__("pause");
      }
  }
}