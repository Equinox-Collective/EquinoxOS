#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

void keyboard_callback(void);
void keyboard_push_string(const char *s);
char get_ascii_char(uint8_t scancode);
void keyboard_push(uint8_t scancode);
uint8_t keyboard_pop(void);

// Доступ к состоянию модификаторов из других подсистем (GUI/eshell).
bool keyboard_super_pressed(void);
bool keyboard_alt_pressed(void);
bool keyboard_ctrl_pressed(void);
bool keyboard_shift_pressed(void);

#endif
