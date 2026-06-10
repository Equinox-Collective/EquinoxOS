#include "../drivers/vesa/vesa.h"
#include "../drivers/hardware/serial/serial.h"
#include <stdint.h>
#include "../drivers/devices/audio/ac97.h"

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t int_no;   
    uint64_t err_code; 
    uint64_t rip, cs, rflags, rsp, ss; 
} __attribute__((packed)) interrupt_frame_t;

// Расшифровка аппаратных исключений x86_64
static const char* exception_messages[32] = {
    "Division By Zero", "Debug", "Non-Maskable Interrupt", "Breakpoint",
    "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 Floating-Point Exception", "Alignment Check", "Machine Check", "SIMD Floating-Point",
    "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security Exception", "Reserved"
};

void panic_handler(interrupt_frame_t* frame) {
    // 1. ЖЕСТКО ВЫКЛЮЧАЕМ ПРЕРЫВАНИЯ
    ac97_stop();
    __asm__ volatile ("cli");

    // Output panic to serial immediately
    serial_puts(COM1, "\n!!! KERNEL PANIC !!!\n");

    // Вытягиваем Control Registers (CR2 содержит адрес, вызвавший Page Fault)
    uint64_t cr2, cr3;
    __asm__ volatile ("mov %%cr2, %0" : "=r" (cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r" (cr3));

    // 3. Рисуем фон (Темно-синий, классический BSOD)
    draw_rect_direct(0, 0, screen_width, screen_height, 0x0000AA);

    // 4. Заголовок
    vesa_draw_string_direct("===============================================================", 30, 30, 0xFFFFFF);
    vesa_draw_string_direct(" A fatal hardware exception has occurred. EquinoxOS is halted. ", 30, 50, 0xFFFFFF);
    vesa_draw_string_direct("===============================================================", 30, 70, 0xFFFFFF);
    
    // 5. Какая именно ошибка произошла?
    if (frame->int_no < 32) {
        vesa_draw_string_direct(exception_messages[frame->int_no], 30, 110, 0x00FFFF);
        serial_puts(COM1, "Exception: ");
        serial_puts(COM1, exception_messages[frame->int_no]);
        serial_puts(COM1, "\n");
    } else {
        vesa_draw_string_direct("Unknown Interrupt", 30, 110, 0x00FFFF);
        serial_puts(COM1, "Exception: Unknown Interrupt\n");
    }
    
    // Serial: dump all critical info
    serial_puts(COM1, "INT_NO: ");
    static char hex_buf[20];
    int pos = 18;
    hex_buf[19] = '\0';
    uint64_t val = frame->int_no;
    for (int i = 0; i < 16; i++) { hex_buf[pos--] = "0123456789ABCDEF"[val & 0xF]; val >>= 4; }
    hex_buf[pos] = 'x'; hex_buf[--pos] = '0';
    serial_puts(COM1, &hex_buf[pos]);
    serial_puts(COM1, "  ERR_CD: ");
    pos = 18; val = frame->err_code;
    for (int i = 0; i < 16; i++) { hex_buf[pos--] = "0123456789ABCDEF"[val & 0xF]; val >>= 4; }
    hex_buf[pos] = 'x'; hex_buf[--pos] = '0';
    serial_puts(COM1, &hex_buf[pos]);
    serial_puts(COM1, "\n");

    serial_puts(COM1, "CR2: ");
    pos = 18; val = cr2;
    for (int i = 0; i < 16; i++) { hex_buf[pos--] = "0123456789ABCDEF"[val & 0xF]; val >>= 4; }
    hex_buf[pos] = 'x'; hex_buf[--pos] = '0';
    serial_puts(COM1, &hex_buf[pos]);
    serial_puts(COM1, "  CR3: ");
    pos = 18; val = cr3;
    for (int i = 0; i < 16; i++) { hex_buf[pos--] = "0123456789ABCDEF"[val & 0xF]; val >>= 4; }
    hex_buf[pos] = 'x'; hex_buf[--pos] = '0';
    serial_puts(COM1, &hex_buf[pos]);
    serial_puts(COM1, "\n");

    serial_puts(COM1, "RIP: ");
    pos = 18; val = frame->rip;
    for (int i = 0; i < 16; i++) { hex_buf[pos--] = "0123456789ABCDEF"[val & 0xF]; val >>= 4; }
    hex_buf[pos] = 'x'; hex_buf[--pos] = '0';
    serial_puts(COM1, &hex_buf[pos]);
    serial_puts(COM1, "  RSP: ");
    pos = 18; val = frame->rsp;
    for (int i = 0; i < 16; i++) { hex_buf[pos--] = "0123456789ABCDEF"[val & 0xF]; val >>= 4; }
    hex_buf[pos] = 'x'; hex_buf[--pos] = '0';
    serial_puts(COM1, &hex_buf[pos]);
    serial_puts(COM1, "\n");

    serial_puts(COM1, "RAX: ");
    pos = 18; val = frame->rax;
    for (int i = 0; i < 16; i++) { hex_buf[pos--] = "0123456789ABCDEF"[val & 0xF]; val >>= 4; }
    hex_buf[pos] = 'x'; hex_buf[--pos] = '0';
    serial_puts(COM1, &hex_buf[pos]);
    serial_puts(COM1, "  RBX: ");
    pos = 18; val = frame->rbx;
    for (int i = 0; i < 16; i++) { hex_buf[pos--] = "0123456789ABCDEF"[val & 0xF]; val >>= 4; }
    hex_buf[pos] = 'x'; hex_buf[--pos] = '0';
    serial_puts(COM1, &hex_buf[pos]);
    serial_puts(COM1, "  RCX: ");
    pos = 18; val = frame->rcx;
    for (int i = 0; i < 16; i++) { hex_buf[pos--] = "0123456789ABCDEF"[val & 0xF]; val >>= 4; }
    hex_buf[pos] = 'x'; hex_buf[--pos] = '0';
    serial_puts(COM1, &hex_buf[pos]);
    serial_puts(COM1, "\n");

    serial_puts(COM1, "RDX: ");
    pos = 18; val = frame->rdx;
    for (int i = 0; i < 16; i++) { hex_buf[pos--] = "0123456789ABCDEF"[val & 0xF]; val >>= 4; }
    hex_buf[pos] = 'x'; hex_buf[--pos] = '0';
    serial_puts(COM1, &hex_buf[pos]);
    serial_puts(COM1, "  RSI: ");
    pos = 18; val = frame->rsi;
    for (int i = 0; i < 16; i++) { hex_buf[pos--] = "0123456789ABCDEF"[val & 0xF]; val >>= 4; }
    hex_buf[pos] = 'x'; hex_buf[--pos] = '0';
    serial_puts(COM1, &hex_buf[pos]);
    serial_puts(COM1, "  RDI: ");
    pos = 18; val = frame->rdi;
    for (int i = 0; i < 16; i++) { hex_buf[pos--] = "0123456789ABCDEF"[val & 0xF]; val >>= 4; }
    hex_buf[pos] = 'x'; hex_buf[--pos] = '0';
    serial_puts(COM1, &hex_buf[pos]);
    serial_puts(COM1, "\n");

    /* Этап 8: pid упавшей задачи + байты кода по RIP + RBP — без этого
     * дампа баг с грязными mmap-страницами было не найти. Оставляем. */
    {
        extern int sprintf(char*, const char*, ...);
        extern unsigned current_task_id_for_panic(void);
        char db[160];
        sprintf(db, "PANIC PID: %u\n", current_task_id_for_panic());
        serial_puts(COM1, db);
        unsigned char *p = (unsigned char *)frame->rip;
        char *w = db; const char *H = "0123456789ABCDEF";
        for (int i = -8; i < 8; i++) {
            *w++ = ' '; *w++ = H[p[i] >> 4]; *w++ = H[p[i] & 0xF];
        }
        *w++ = '\n'; *w = 0;
        serial_puts(COM1, "CODE@RIP:"); serial_puts(COM1, db);
        /* RBP/R8/R11 руками (формат %lx ядро может не знать). */
        uint64_t vals[3] = { frame->rbp, frame->r8, frame->r11 };
        const char *names[3] = { "RBP: 0x", "R8:  0x", "R11: 0x" };
        for (int v = 0; v < 3; v++) {
            w = db;
            for (int i = 15; i >= 0; i--) *w++ = H[(vals[v] >> (i*4)) & 0xF];
            *w++ = '\n'; *w = 0;
            serial_puts(COM1, names[v]); serial_puts(COM1, db);
        }
    }
    serial_puts(COM1, "!!! SYSTEM HALTED !!!\n");

    vesa_draw_string_hex_direct("INT_NO: ", 300, 110, frame->int_no, 0xFFFFFF);
    vesa_draw_string_hex_direct("ERR_CD: ", 500, 110, frame->err_code, 0xFFFFFF);

    // 6. Управляющие регистры (КРИТИЧЕСКИ ВАЖНО ДЛЯ ДЕБАГА ПАМЯТИ)
    vesa_draw_string_direct("--- CONTROL REGISTERS ---", 30, 150, 0xAAAAFF);
    vesa_draw_string_hex_direct("CR2 (Fault Addr): ", 30,  170, cr2, 0xFF7777); // Красным!
    vesa_draw_string_hex_direct("CR3 (Page Dir)  : ", 350, 170, cr3, 0xFFFFFF);

    // 7. Регистры процессора (В две колонки)
    vesa_draw_string_direct("--- CPU REGISTERS ---", 30, 210, 0xAAAAFF);
    
    // Левая колонка
    vesa_draw_string_hex_direct("RAX: ", 30, 230, frame->rax, 0xFFFFFF);
    vesa_draw_string_hex_direct("RBX: ", 30, 250, frame->rbx, 0xFFFFFF);
    vesa_draw_string_hex_direct("RCX: ", 30, 270, frame->rcx, 0xFFFFFF);
    vesa_draw_string_hex_direct("RDX: ", 30, 290, frame->rdx, 0xFFFFFF);
    vesa_draw_string_hex_direct("RSI: ", 30, 310, frame->rsi, 0xFFFFFF);
    vesa_draw_string_hex_direct("RDI: ", 30, 330, frame->rdi, 0xFFFFFF);
    vesa_draw_string_hex_direct("RBP: ", 30, 350, frame->rbp, 0xFFFFFF);
    vesa_draw_string_hex_direct("RSP: ", 30, 370, frame->rsp, 0xFFFFFF);

    // Правая колонка
    vesa_draw_string_hex_direct("R8 : ", 250, 230, frame->r8,  0xFFFFFF);
    vesa_draw_string_hex_direct("R9 : ", 250, 250, frame->r9,  0xFFFFFF);
    vesa_draw_string_hex_direct("R10: ", 250, 270, frame->r10, 0xFFFFFF);
    vesa_draw_string_hex_direct("R11: ", 250, 290, frame->r11, 0xFFFFFF);
    vesa_draw_string_hex_direct("R12: ", 250, 310, frame->r12, 0xFFFFFF);
    vesa_draw_string_hex_direct("R13: ", 250, 330, frame->r13, 0xFFFFFF);
    vesa_draw_string_hex_direct("R14: ", 250, 350, frame->r14, 0xFFFFFF);
    vesa_draw_string_hex_direct("R15: ", 250, 370, frame->r15, 0xFFFFFF);

    // 8. Где именно мы упали (Точка выполнения)
    vesa_draw_string_direct("--- EXECUTION STATE ---", 500, 210, 0xAAAAFF);
    vesa_draw_string_hex_direct("RIP: ", 500, 230, frame->rip, 0xFFFF00); // Желтым, чтобы бросалось в глаза
    vesa_draw_string_hex_direct("CS : ", 500, 250, frame->cs, 0xFFFFFF);
    vesa_draw_string_hex_direct("FLG: ", 500, 270, frame->rflags, 0xFFFFFF);

    // 9. Дамп вершины стека (Первые 5 значений, куда указывает RSP)
    vesa_draw_string_direct("--- RAW STACK DUMP ---", 500, 310, 0xAAAAFF);
    uint64_t* stack = (uint64_t*)frame->rsp;
    
    // ПРОВЕРКА: Если RSP в юзер-спейсе (ниже 0xFFFF...) и мы в панике, 
    // лучше не лезть туда без проверки маппинга, иначе - рекурсивный краш.
    if (frame->rsp < 0x00007FFFFFFFFFFF) {
        vesa_draw_string_direct("User stack access unsafe", 500, 330, 0xFF7777);
    } else {
        for (int i = 0; i < 5; i++) {
            // Рискуем, только если это стек ядра
            vesa_draw_string_hex_direct("-> ", 500, 330 + (i * 20), stack[i], 0xCCCCCC);
        }
    }

    vesa_draw_string_direct("Please restart your computer.", 30, 420, 0x00FF00);

    // 10. МЕРТВАЯ ПЕТЛЯ
    while(1) {
        __asm__ volatile ("hlt");
    }
}