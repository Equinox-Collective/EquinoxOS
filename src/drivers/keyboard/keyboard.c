#include "keyboard.h"
#include "../../io/io.h"
#include "../../shell/shell.h"
#include "../../gui/gui.h"
#include <stdint.h>
#include <stdbool.h>

extern volatile uint8_t last_scancode;
extern bool is_app_running;
extern void notepad_handle_char(char c);

static bool shift_pressed = false;

static const char ascii_table[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

static const char ascii_table_shift[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

char get_ascii_char(uint8_t scancode) {
    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = true; return 0; }
    if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = false; return 0; }
    if (scancode & 0x80) return 0;
    if (scancode >= sizeof(ascii_table)) return 0;
    return shift_pressed ? ascii_table_shift[scancode] : ascii_table[scancode];
}

void keyboard_callback() {
    uint8_t scancode = inb(0x60);
    last_scancode = scancode; // Сохраняем для игр (Змейка/Дум)
    
    char c = get_ascii_char(scancode);

    if (c > 0) {
        if (!is_app_running) {
            // Route to notepad if it's active, otherwise to shell
            if (notepad_win && notepad_win->active) {
                notepad_handle_char(c);
            } else {
                shell_handle_char(c);
            }
        }
    }
}
