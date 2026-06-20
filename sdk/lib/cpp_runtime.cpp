#include <stddef.h>

// Импортируем стандартные функции выделения памяти из libc
extern "C" void* malloc(size_t size);
extern "C" void free(void* ptr);

// Перегрузка операторов new / delete для одиночных объектов
void* operator new(size_t size) {
    return malloc(size);
}

void operator delete(void* ptr) noexcept {
    free(ptr);
}

// Перегрузка операторов new[] / delete[] для массивов
void* operator new[](size_t size) {
    return malloc(size);
}

void operator delete[](void* ptr) noexcept {
    free(ptr);
}

// Перегрузка операторов delete с указанием размера (C++14)
void operator delete(void* ptr, size_t size) noexcept {
    (void)size;
    free(ptr);
}

void operator delete[](void* ptr, size_t size) noexcept {
    (void)size;
    free(ptr);
}

// Заглушка для обработки вызовов чистых виртуальных функций (абстрактных классов)
extern "C" void __cxa_pure_virtual() {
    // В случае критической ошибки останавливаем процессор в бесконечном цикле
    while (true) {
        __asm__ volatile("hlt");
    }
}

// Заглушки для деструкторов глобальных объектов при выходе (нам не требуется их вызывать при завершении задачи)
extern "C" {
    void* __dso_handle = nullptr;

    int __cxa_atexit(void (*destructor)(void*), void* arg, void* dso) {
        (void)destructor;
        (void)arg;
        (void)dso;
        return 0; // Возвращаем успех
    }
}