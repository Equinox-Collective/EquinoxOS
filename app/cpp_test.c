#include <equos.h>

// Импортируем Си-функции из SDK
extern "C" {
    int sprintf(char* buf, const char* fmt, ...);
    void term_print(const char* str);
}

// Тестовый класс для проверки глобальных конструкторов
class GlobalTester {
public:
    int value;
    GlobalTester() {
        value = 42; // Это значение должно установиться до входа в main()
    }
};

// Объявляем глобальный статический объект
GlobalTester g_test;

// Проверка полиморфизма без использования RTTI и исключений
class Animal {
public:
    virtual const char* makeSound() = 0;
    virtual ~Animal() {}
};

class Cat : public Animal {
public:
    virtual const char* makeSound() override {
        return "Meow (from C++ Virtual Method!)";
    }
};

int main() {
    term_print("[C++] Running freestanding C++ test application...\n");
    
    char buf[128];
    sprintf(buf, "[C++] Global constructor check: %d (Expected: 42)\n", g_test.value);
    term_print(buf);

    term_print("[C++] Testing dynamic allocation (new/delete)...\n");
    Animal* my_cat = new Cat();
    
    sprintf(buf, "[C++] Poly call: %s\n", my_cat->makeSound());
    term_print(buf);

    delete my_cat;
    term_print("[C++] Testing complete. C++ works flawlessly under Ring 3!\n");
    return 0;
}