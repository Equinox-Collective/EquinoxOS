#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <stdbool.h>

// init_mouse теперь возвращает bool, чтобы ядро знало, 
// успешно ли прошла диагностика PS/2-контроллера.
bool init_mouse(void);
void mouse_callback(void);

// Универсальный обработчик координат для USB мышей
void usb_mouse_update(int8_t dx, int8_t dy, uint8_t buttons);

#endif