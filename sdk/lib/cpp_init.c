#include <stddef.h>

// Эти символы автоматически экспортируются компоновщиком (ld)
extern void (*__init_array_start[])(void) __attribute__((weak));
extern void (*__init_array_end[])(void) __attribute__((weak));

void __libc_init_array(void) {
    if (!__init_array_start || !__init_array_end) {
        return;
    }
    size_t count = __init_array_end - __init_array_start;
    for (size_t i = 0; i < count; i++) {
        if (__init_array_start[i]) {
            __init_array_start[i]();
        }
    }
}