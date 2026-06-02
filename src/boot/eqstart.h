#ifndef EQSTART_H
#define EQSTART_H
#include <stdint.h>

static void log(const char* msg, uint32_t color);

// Boot-анимация Nyan Cat — экспортируется, чтобы другие подсистемы (USB mouse
// test и т.п.) могли продолжать крутить гифку во время своих блокирующих
// циклов и она не «замирала» до старта GUI.
void nyan_init_geometry(void);
void nyan_draw_frame(int frame);
void nyan_boot_anim_frame(void);

#endif