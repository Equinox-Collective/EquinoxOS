#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include <stdbool.h>

typedef struct window {
    int x, y, w, h;
    char title[64];
    uint32_t* buffer;  // Личный холст окна (размером w * h)
    bool active;
    bool dragging;
    int drag_off_x, drag_off_y;
    int z_index;       // Для порядка отрисовки
    struct window* next;
} window_t;

// Desktop icon
typedef struct {
    int x, y;
    char label[32];
    int icon_id;
} desktop_icon_t;

#define ICON_TERMINAL   0
#define ICON_SYSMONITOR 1
#define ICON_PAINT      2
#define ICON_EXPLORER   3
#define ICON_NOTEPAD    4
#define MAX_DESKTOP_ICONS 8
#define TASKBAR_HEIGHT  32
#define ICON_SIZE       48

void gui_init(void);
window_t* window_create(int x, int y, int w, int h, const char* title);
void window_bring_to_front(window_t* win);
void gui_compositor_render(void);

// Window drawing
void gui_window_put_pixel(window_t* win, int x, int y, uint32_t color);
void gui_window_draw_rect(window_t* win, int x, int y, int w, int h, uint32_t color);
void gui_window_draw_string(window_t* win, const char* s, int x, int y, uint32_t color);

// Desktop system
void gui_desktop_init(void);
void gui_render_desktop_icons(void);
void gui_render_taskbar(void);
int gui_check_icon_click(int mx, int my);
bool gui_check_close_button(int mx, int my);

// Bresenham line in window buffer
void gui_window_draw_line(window_t* win, int x0, int y0, int x1, int y1, int thickness, uint32_t color);

// System windows
extern window_t* term_win;
extern window_t* main_win;
extern window_t* app_win;
extern window_t* paint_win;
extern window_t* explorer_win;
extern window_t* notepad_win;

#endif
