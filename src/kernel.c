#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// --- ЗАГОЛОВКИ СИСТЕМЫ И БИБЛИОТЕК ---
#include "boot/limine/limine.h"
#include "libc/string.h"
#include "libc/stdio.h"
#include "api.h"
#include "fs/elf.h"
#include "gui/gui.h"

// --- СИСТЕМНЫЕ ПОДСИСТЕМЫ ---
#include "system/pic.h"
#include "system/idt.h"
#include "system/pmm.h"
#include "system/memory.h"
#include "system/timer.h"
#include "system/task.h"

// --- ДРАЙВЕРЫ ---
#include "drivers/vga/vesa.h"
#include "drivers/vga/bmp.h"
#include "drivers/mouse/mouse.h"
#include "drivers/pci/pci.h"
#include "drivers/net/rtl8139.h"

// --- ФАЙЛОВАЯ СИСТЕМА И ОБОЛОЧКА ---
#include "fs/vfs.h"
#include "fs/fs.h"
#include "shell/shell.h"
#include "fs/fat32.h"

// --- ВНЕШНИЕ ПЕРЕМЕННЫЕ И ФУНКЦИИ ---
extern size_t used_memory; 
extern volatile uint32_t tick;
extern char shell_buffer[64];

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ---
bool is_app_running = false;
bool should_run_app = false; 
char term_history[8][64] = {0};
volatile uint8_t last_scancode = 0;

// --- LIMINE REQUESTS ---
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0
};
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0
};
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0
};

// =========================================================================
//                              GUI & WINDOWS
// (TODO: В будущем вынести в отдельный gui.c / gui.h)
// =========================================================================

void draw_cursor(int x, int y) {
    static const int cursor_map[8][8] = {
        {2,0,0,0,0,0,0,0}, {2,2,0,0,0,0,0,0}, {2,1,2,0,0,0,0,0}, {2,1,1,2,0,0,0,0},
        {2,1,1,1,2,0,0,0}, {2,1,1,1,1,2,0,0}, {2,2,2,2,2,2,2,0}, {0,0,2,2,2,0,0,0}
    };
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            if (cursor_map[i][j] == 1) put_pixel(x + j, y + i, 0xFFFFFF);
            else if (cursor_map[i][j] == 2) put_pixel(x + j, y + i, 0x000000);
        }
    }
}

static uint8_t prev_mouse_left = 0;
static uint32_t paint_color = 0x000000;
static int paint_prev_x = -1;
static int paint_prev_y = -1;

// Notepad state
#define NOTEPAD_MAX_LINES 16
#define NOTEPAD_LINE_LEN  48
static char notepad_buf[NOTEPAD_MAX_LINES][NOTEPAD_LINE_LEN];
static int notepad_line = 0;
static int notepad_col = 0;
static bool notepad_inited = false;

void notepad_handle_char(char c) {
    if (!notepad_inited) return;
    if (c == '\b') {
        if (notepad_col > 0) {
            notepad_col--;
            notepad_buf[notepad_line][notepad_col] = '\0';
        } else if (notepad_line > 0) {
            notepad_line--;
            notepad_col = strlen(notepad_buf[notepad_line]);
        }
    } else if (c == '\n') {
        if (notepad_line < NOTEPAD_MAX_LINES - 1) {
            notepad_line++;
            notepad_col = 0;
        }
    } else {
        if (notepad_col < NOTEPAD_LINE_LEN - 1) {
            notepad_buf[notepad_line][notepad_col] = c;
            notepad_col++;
            notepad_buf[notepad_line][notepad_col] = '\0';
        }
    }
}

// Explorer state
static char explorer_files[16][13]; // FAT32 8.3 names
static int explorer_file_count = 0;
static bool explorer_scanned = false;
static int explorer_scroll = 0;

void update_gui() {
    // Handle mouse clicks (rising edge detection)
    uint8_t mouse_just_pressed = (mouse_left_button && !prev_mouse_left);

    if (mouse_just_pressed) {
        // Close button has priority
        if (!gui_check_close_button(mouse_x, mouse_y)) {
            // Then check desktop icons
            int icon = gui_check_icon_click(mouse_x, mouse_y);
            switch(icon) {
                case ICON_TERMINAL:
                    term_win->active = true;
                    window_bring_to_front(term_win);
                    break;
                case ICON_SYSMONITOR:
                    main_win->active = true;
                    window_bring_to_front(main_win);
                    break;
                case ICON_PAINT:
                    paint_win->active = true;
                    window_bring_to_front(paint_win);
                    break;
                case ICON_EXPLORER:
                    explorer_win->active = true;
                    explorer_scanned = false;
                    window_bring_to_front(explorer_win);
                    break;
                case ICON_NOTEPAD:
                    notepad_win->active = true;
                    if (!notepad_inited) {
                        for(int i=0; i<NOTEPAD_MAX_LINES; i++)
                            memset(notepad_buf[i], 0, NOTEPAD_LINE_LEN);
                        notepad_line = 0; notepad_col = 0;
                        notepad_inited = true;
                    }
                    window_bring_to_front(notepad_win);
                    break;
            }
        }
    }

    // 1. Terminal
    if (term_win && term_win->active) {
        gui_window_draw_rect(term_win, 0, 0, term_win->w, term_win->h, 0x000000);
        for(int i = 0; i < 8; i++) {
            gui_window_draw_string(term_win, term_history[i], 10, 10 + (i * 15), 0xAAAAAA);
        }
        gui_window_draw_string(term_win, "> ", 10, 10 + (8 * 15), 0xFFFFFF);
        gui_window_draw_string(term_win, shell_buffer, 26, 10 + (8 * 15), 0x00FF00);
    }

    // 2. System Monitor
    if (main_win && main_win->active) {
        gui_window_draw_rect(main_win, 0, 0, main_win->w, main_win->h, 0xFFFFFF);
        char mem_info[64];
        sprintf(mem_info, "RAM: %d MB", used_memory / 1024 / 1024);
        gui_window_draw_string(main_win, mem_info, 15, 15, 0x000000);
    }

    // 3. Paint — smooth line drawing with Bresenham interpolation
    if (paint_win && paint_win->active) {
        // Color palette bar (top 20px)
        gui_window_draw_rect(paint_win, 0, 0, paint_win->w, 20, 0xCCCCCC);
        gui_window_draw_rect(paint_win, 4, 2, 16, 16, 0x000000);
        gui_window_draw_rect(paint_win, 24, 2, 16, 16, 0xFF0000);
        gui_window_draw_rect(paint_win, 44, 2, 16, 16, 0x00FF00);
        gui_window_draw_rect(paint_win, 64, 2, 16, 16, 0x0000FF);
        gui_window_draw_rect(paint_win, 84, 2, 16, 16, 0xFFFF00);
        gui_window_draw_rect(paint_win, 104, 2, 16, 16, 0xFFFFFF);
        gui_window_draw_rect(paint_win, 124, 2, 16, 16, 0xFF00FF);
        gui_window_draw_rect(paint_win, 144, 2, 16, 16, 0x00FFFF);
        // Current color indicator
        gui_window_draw_rect(paint_win, paint_win->w - 22, 2, 16, 16, paint_color);
        // "Clear" button
        gui_window_draw_string(paint_win, "CLR", paint_win->w - 60, 6, 0x333333);

        if (mouse_left_button) {
            int rel_x = mouse_x - paint_win->x;
            int rel_y = mouse_y - paint_win->y;
            // Color selection on click
            if (rel_y >= 0 && rel_y < 20 && mouse_just_pressed) {
                if (rel_x >= 4 && rel_x < 20) paint_color = 0x000000;
                else if (rel_x >= 24 && rel_x < 40) paint_color = 0xFF0000;
                else if (rel_x >= 44 && rel_x < 60) paint_color = 0x00FF00;
                else if (rel_x >= 64 && rel_x < 80) paint_color = 0x0000FF;
                else if (rel_x >= 84 && rel_x < 100) paint_color = 0xFFFF00;
                else if (rel_x >= 104 && rel_x < 120) paint_color = 0xFFFFFF;
                else if (rel_x >= 124 && rel_x < 140) paint_color = 0xFF00FF;
                else if (rel_x >= 144 && rel_x < 160) paint_color = 0x00FFFF;
                else if (rel_x >= paint_win->w - 60 && rel_x < paint_win->w - 24) {
                    // Clear canvas
                    gui_window_draw_rect(paint_win, 0, 20, paint_win->w, paint_win->h - 20, 0xFFFFFF);
                }
            }
            // Canvas drawing with line interpolation
            else if (rel_y >= 20 && rel_x >= 0 && rel_x < paint_win->w && rel_y < paint_win->h) {
                if (paint_prev_x >= 0 && paint_prev_y >= 0) {
                    gui_window_draw_line(paint_win, paint_prev_x, paint_prev_y, rel_x, rel_y, 1, paint_color);
                } else {
                    for(int dy = -1; dy <= 1; dy++)
                        for(int dx = -1; dx <= 1; dx++)
                            gui_window_put_pixel(paint_win, rel_x + dx, rel_y + dy, paint_color);
                }
                paint_prev_x = rel_x;
                paint_prev_y = rel_y;
            }
        } else {
            paint_prev_x = -1;
            paint_prev_y = -1;
        }
    }

    // 4. Explorer — graphical file browser
    if (explorer_win && explorer_win->active) {
        gui_window_draw_rect(explorer_win, 0, 0, explorer_win->w, explorer_win->h, 0xFFFFFF);

        // Toolbar
        gui_window_draw_rect(explorer_win, 0, 0, explorer_win->w, 24, 0xF0F0F0);
        gui_window_draw_string(explorer_win, "Path: /", 8, 7, 0x333333);
        // Refresh button
        gui_window_draw_rect(explorer_win, explorer_win->w - 60, 3, 52, 18, 0xDDDDDD);
        gui_window_draw_string(explorer_win, "Refresh", explorer_win->w - 56, 7, 0x333333);
        // Separator
        gui_window_draw_rect(explorer_win, 0, 24, explorer_win->w, 1, 0xCCCCCC);

        // Scan files on first open or refresh click
        if (!explorer_scanned) {
            explorer_file_count = 0;
            // Add device entries
            strcpy(explorer_files[explorer_file_count++], "[dev] fb0");
            strcpy(explorer_files[explorer_file_count++], "[dev] net");
            // TODO: real fat32 file listing could go here
            explorer_scanned = true;
        }

        // Refresh button click
        if (mouse_just_pressed) {
            int rx = mouse_x - explorer_win->x;
            int ry = mouse_y - explorer_win->y;
            if (rx >= explorer_win->w - 60 && rx < explorer_win->w - 8 && ry >= 3 && ry < 21) {
                explorer_scanned = false;
            }
        }

        // Draw file list with icons
        int y_off = 30;
        for(int i = 0; i < explorer_file_count && y_off < explorer_win->h - 10; i++) {
            // Alternate row background
            if (i % 2 == 0)
                gui_window_draw_rect(explorer_win, 0, y_off - 2, explorer_win->w, 18, 0xF8F8F8);
            // Folder/device icon (small colored square)
            uint32_t ic = (explorer_files[i][0] == '[') ? 0x0078D7 : 0xF0C040;
            gui_window_draw_rect(explorer_win, 8, y_off, 12, 12, ic);
            gui_window_draw_string(explorer_win, explorer_files[i], 26, y_off + 2, 0x222222);
            y_off += 18;
        }

        if (explorer_file_count == 0) {
            gui_window_draw_string(explorer_win, "No files found", 26, 40, 0x999999);
        }
    }

    // 5. Notepad
    if (notepad_win && notepad_win->active) {
        gui_window_draw_rect(notepad_win, 0, 0, notepad_win->w, notepad_win->h, 0xFFFFFF);
        // Menu bar
        gui_window_draw_rect(notepad_win, 0, 0, notepad_win->w, 18, 0xF0F0F0);
        gui_window_draw_string(notepad_win, "Notepad - EquinoxOS", 8, 5, 0x333333);
        // Separator
        gui_window_draw_rect(notepad_win, 0, 18, notepad_win->w, 1, 0xCCCCCC);
        // Text content
        for(int i = 0; i < NOTEPAD_MAX_LINES; i++) {
            gui_window_draw_string(notepad_win, notepad_buf[i], 8, 22 + i * 14, 0x000000);
        }
        // Cursor blink (simple block)
        int cx = 8 + notepad_col * 8;
        int cy = 22 + notepad_line * 14;
        if ((tick / 50) % 2 == 0) {
            gui_window_draw_rect(notepad_win, cx, cy, 2, 10, 0x000000);
        }
    }

    prev_mouse_left = mouse_left_button;

    gui_compositor_render();
    vesa_update();
}

// =========================================================================
//                              SYSTEM API
// =========================================================================

void term_print(const char* str) {
    while (*str) {
        // Если встретили символ переноса строки
        if (*str == '\n') {
            // Сдвигаем все строки вверх (освобождаем место снизу)
            for (int i = 0; i < 7; i++) {
                memcpy(term_history[i], term_history[i+1], 64);
            }
            // Очищаем самую нижнюю строку
            memset(term_history[7], 0, 64);
        } 
        else {
            // Ищем конец текущей (последней) строки
            int len = 0;
            while (term_history[7][len] != '\0' && len < 63) {
                len++;
            }

            // Если в строке еще есть место
            if (len < 63) {
                term_history[7][len] = *str;
                term_history[7][len + 1] = '\0';
            } else {
                // Если строка переполнена — принудительный перенос (автоперенос)
                for (int i = 0; i < 7; i++) {
                    memcpy(term_history[i], term_history[i+1], 64);
                }
                memset(term_history[7], 0, 64);
                term_history[7][0] = *str;
                term_history[7][1] = '\0';
            }
        }
        str++;
    }
}
void* sys_get_file(const char* name, uint64_t* size) {
    if (module_request.response == NULL) return NULL;
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file* module = module_request.response->modules[i];
        if (strstr(module->path, name) != NULL) {
            *size = module->size;
            return module->address;
        }
    }
    return NULL;
}

char sys_get_key() { return 0; } // Заглушка
uint32_t sys_get_time_ms() { return tick * 10; }

uint8_t sys_get_scancode() {
    uint8_t code = last_scancode;
    last_scancode = 0;
    return code;
}

// Вызывается приложением для отрисовки
void sys_draw_app_buffer(int x, int y, int w, int h, uint32_t* buffer) {
    if (app_win && app_win->active) {
        // Копируем пиксели программы в личный буфер окна!
        for(int i = 0; i < h; i++) {
            memcpy(&app_win->buffer[i * w], &buffer[i * w], w * 4);
        }
    }
    update_gui(); // Перерисовываем
}

// =========================================================================
//                              MAIN LOOPS & INIT
// =========================================================================

void network_thread() {
    while(1) {
        rtl8139_receive();
        // Мы больше не тормозим GUI!
        // Если пакетов нет, можно чуть-чуть подождать
        __asm__("pause"); 
    }
}

void init_sse() {
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2); // Сбросить EM (Emulation)
    cr0 |= (1 << 1);  // Установить MP (Monitor Coprocessor)
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (3 << 9);  // Установить OSFXSR (9) и OSXMMEXCPT (10)
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
}

void run_elf(uint8_t* elf_data) {
    Elf64_Ehdr* hdr = (Elf64_Ehdr*)elf_data;
    
    if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' || 
        hdr->e_ident[2] != 'L' || hdr->e_ident[3] != 'F') {
        term_print("Not a valid ELF file!\n");
        return;
    }

    term_print("Loading ELF segments...\n");

    Elf64_Phdr* phdr = (Elf64_Phdr*)(elf_data + hdr->e_phoff);
    for (int i = 0; i < hdr->e_phnum; i++) {
        if (phdr[i].p_type == 1) { // PT_LOAD = 1
            uint8_t* dest = (uint8_t*)phdr[i].p_vaddr;
            uint8_t* src = elf_data + phdr[i].p_offset;
            memcpy(dest, src, phdr[i].p_filesz);
            
            if (phdr[i].p_memsz > phdr[i].p_filesz) {
                memset(dest + phdr[i].p_filesz, 0, phdr[i].p_memsz - phdr[i].p_filesz);
            }
        }
    }

    term_print("Starting App Window...\n");
    
    app_win->active = true;
    strcpy(app_win->title, "Snake Game");
    
    EquinoxAPI api;
    api.draw_buffer = sys_draw_app_buffer;
    api.get_scancode = sys_get_scancode;
    api.get_time_ms = sys_get_time_ms;
    
    typedef void (*app_entry_t)(EquinoxAPI*);
    app_entry_t entry = (app_entry_t)hdr->e_entry;
    
    is_app_running = true;
    
    // Включаем запись в защищенную память (Ring 0 bypass)
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~0x10000ULL; 
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    // Выполнение приложения
    entry(&api);
    
    is_app_running = false;
    app_win->active = false;
    term_print("App closed.\n");
    
    cr0 |= 0x10000ULL;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

void exec_module() {
    if (module_request.response == NULL) return;
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file* mod = module_request.response->modules[i];
        if (strstr(mod->path, "app.elf\n")) {
            run_elf(mod->address);
            return;
        }
    }
    term_print("app.elf not found!\n");
}

void kmain(void) {
    // 1. Память
    pmm_init(); 
    uint64_t hhdm_offset = hhdm_request.response->offset;
    init_heap(pmm_alloc_continuous(16384) + hhdm_offset, 64 * 1024 * 1024);

    // 2. Графика и VFS
    vfs_init();
    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    init_vesa((uintptr_t)fb->address, fb->width, fb->height, fb->pitch);
    fb_install_vfs();

    // 3. Прерывания и база
    __asm__("cli");
    init_idt();
    init_sse();
    pic_remap();
    init_mouse();
    init_timer(100);
    task_init();
    task_create(network_thread);
    __asm__("sti");

    fat32_init();
    // 4. Периферия
    pci_init();
    rtl8139_install_vfs(); 

    gui_init(); 

    printf("EquinoxOS Booted. Memory: %d MB free\n", free_memory / 1024 / 1024);
    printf("Devices registered: /dev/fb0, /dev/net\n");

    shell_init();

    while(1) {
        update_gui();
        
        if (should_run_app) {
            should_run_app = false;
            exec_module();
        }

        __asm__("hlt");
    }
}
