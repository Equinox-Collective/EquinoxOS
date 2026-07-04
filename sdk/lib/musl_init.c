#include <stddef.h>
#include <stdint.h>

// Оригинальная структура __libc из исходников Musl
struct __libc {
    char can_do_threads;
    char threaded;
    char secure;
    volatile signed char need_locks;
    int threads_minus_1;
    size_t *auxv;
    void *tls_head;
    size_t tls_size, tls_align, tls_cnt;
    size_t page_size;
};

// Musl экспортирует этот глобальный объект внутри libc.a
extern struct __libc __libc;

void equos_init_musl(void) {
    // Безопасный пустой auxv, чтобы get_random_secret() сразу завершал цикл
    static size_t dummy_auxv[2] = {0, 0};
    __libc.auxv = dummy_auxv;
    __libc.page_size = 4096;
}