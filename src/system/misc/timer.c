// timer.c
#include "timer.h"
#include "../core/io.h"
#include "../usr/sched.h"

// volatile крайне важен, чтобы компилятор не оптимизировал проверки в sleep()
volatile uint32_t tick = 0;

/* TCP periodic tick — defined in src/system/drivers/hardware/net/tcp.c.
 * Walks every TCB once per millisecond, retransmits unacked segments whose
 * RTO has elapsed, drains TIME_WAIT timers. Declared here (instead of via
 * a header) to avoid pulling the whole net stack into the timer TU. */
void tcp_tick_with_iface(uint32_t now_ms);

/* Boot-анимация Nyan Cat (см. src/boot/eqstart.c). Пока nyan_boot_active != 0,
 * крутим гифку прямо из PIT-обработчика, чтобы она не «замирала» во время
 * блокирующих этапов загрузки. nyan_boot_anim_frame() перерисовывает кадр
 * только при его смене (раз в ~100 мс), так что нагрузка на IRQ минимальна. */
extern volatile int nyan_boot_active;
extern void nyan_boot_anim_frame(void);

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