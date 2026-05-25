#include "gui_apps.h"
#include "../syslibc/stdio.h"
#include "../syslibc/string.h"
#include "../system/drivers/devices/mouse/mouse.h"
#include "../system/drivers/vesa/vesa.h"
#include "../system/fs/fat32.h"
#include "../system/fs/vfs.h"
#include "../system/drivers/vesa/bmp.h"
#include "../system/mem/memory.h"
#include "../system/mem/pmm.h"

// --- APP STATES ---
static explorer_file_t real_files[256];
static char notepad_buf[NOTEPAD_MAX_LINES][NOTEPAD_LINE_LEN];
static int explorer_file_count = 0;
static bool explorer_scanned = false;

static uint32_t paint_color = 0x000000;
static int paint_prev_x = -1;
static int paint_prev_y = -1;
static uint8_t prev_mouse_left = 0;
static int notepad_line = 0;
static int notepad_col = 0;
static bool notepad_inited = false;

window_t *focused_window = NULL;
extern volatile uint32_t tick;
extern void term_print(const char *str);

void draw_cursor(int x, int y) {
  static const int cursor_map[8][8] = {
      {2, 0, 0, 0, 0, 0, 0, 0}, {2, 2, 0, 0, 0, 0, 0, 0},
      {2, 1, 2, 0, 0, 0, 0, 0}, {2, 1, 1, 2, 0, 0, 0, 0},
      {2, 1, 1, 1, 2, 0, 0, 0}, {2, 1, 1, 1, 1, 2, 0, 0},
      {2, 2, 2, 2, 2, 2, 2, 0}, {0, 0, 2, 2, 2, 0, 0, 0}};
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 8; j++) {
      if (cursor_map[i][j] == 1)
        put_pixel(x + j, y + i, 0xFFFFFF);
      else if (cursor_map[i][j] == 2)
        put_pixel(x + j, y + i, 0x000000);
    }
  }
}

void notepad_load_content(const char *data, uint32_t size) {
  for (int i = 0; i < NOTEPAD_MAX_LINES; i++) {
    memset(notepad_buf[i], 0, NOTEPAD_LINE_LEN);
  }
  notepad_line = 0;
  notepad_col = 0;

  if (!data) return;

  for (uint32_t i = 0; i < size; i++) {
    char c = data[i];
    if (c == '\0') break;

    if (c == '\n' || c == '\r') {
      if (notepad_line < NOTEPAD_MAX_LINES - 1) {
        notepad_line++;
        notepad_col = 0;
      }
      if (c == '\r' && i + 1 < size && data[i + 1] == '\n') i++;
    } else {
      if (notepad_col < NOTEPAD_LINE_LEN - 1) {
        notepad_buf[notepad_line][notepad_col++] = c;
      }
    }
  }
}

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

void update_gui() {
  uint8_t mouse_just_pressed = (mouse_left_button && !prev_mouse_left);

  if (mouse_just_pressed) {
    window_t *clicked_win = gui_find_window_at(mouse_x, mouse_y);
    if (clicked_win) {
      focused_window = clicked_win;
      window_bring_to_front(clicked_win);
      if (gui_check_close_button(mouse_x, mouse_y)) {
        if (focused_window == clicked_win) focused_window = NULL;
      }
    } else {
      focused_window = NULL;
      int icon = gui_check_icon_click(mouse_x, mouse_y);
      switch (icon) {
      case ICON_TERMINAL:
        term_win->active = true;
        window_bring_to_front(term_win);
        focused_window = term_win;
        break;
      case ICON_SYSMONITOR:
        main_win->active = true;
        window_bring_to_front(main_win);
        focused_window = main_win;
        break;
      case ICON_PAINT:
        paint_win->active = true;
        window_bring_to_front(paint_win);
        focused_window = paint_win;
        break;
      case ICON_EXPLORER:
        explorer_win->active = true;
        explorer_scanned = false;
        window_bring_to_front(explorer_win);
        focused_window = explorer_win;
        break;
      case ICON_NOTEPAD:
        notepad_win->active = true;
        if (!notepad_inited) {
          for (int i = 0; i < NOTEPAD_MAX_LINES; i++)
            memset(notepad_buf[i], 0, NOTEPAD_LINE_LEN);
          notepad_line = 0;
          notepad_col = 0;
          notepad_inited = true;
        }
        window_bring_to_front(notepad_win);
        focused_window = notepad_win;
        break;
      }
    }
  }

  window_t *curr = window_list_head;
  while (curr) {
    if (curr->active && curr->on_draw) {
      curr->on_draw(curr);
    }
    curr = curr->next;
  }

  // System Monitor Drawing
  if (main_win && main_win->active) {
    gui_window_draw_rect(main_win, 0, 0, main_win->w, main_win->h, 0xFFFFFF);
    char info[64];
    uint32_t used_mb = (uint32_t)(pmm_get_used_memory() / 1024 / 1024);
    uint32_t total_mb = (uint32_t)(pmm_get_total_memory() / 1024 / 1024);
    sprintf(info, "System RAM: %u / %u MB", used_mb, total_mb);
    gui_window_draw_string(main_win, info, 15, 20, 0x000000);
    gui_window_draw_rect(main_win, 15, 35, 150, 10, 0xDDDDDD);
    int bar_w = (used_mb * 150) / total_mb;
    gui_window_draw_rect(main_win, 15, 35, bar_w, 10, 0x0078D7);
    /* tick = milliseconds (PIT @ 1 kHz), see kernel.c. */
    uint32_t s = tick / 1000;
    sprintf(info, "Uptime: %02u:%02u:%02u", s / 3600, (s % 3600) / 60, s % 60);
    gui_window_draw_string(main_win, info, 15, 85, 0x555555);
  }

  // Paint Logic
  if (paint_win && paint_win->active) {
    gui_window_draw_rect(paint_win, 0, 0, paint_win->w, 20, 0xCCCCCC);
    gui_window_draw_rect(paint_win, 4, 2, 16, 16, 0x000000);
    gui_window_draw_rect(paint_win, 24, 2, 16, 16, 0xFF0000);
    gui_window_draw_rect(paint_win, 44, 2, 16, 16, 0x00FF00);
    gui_window_draw_rect(paint_win, 64, 2, 16, 16, 0x0000FF);
    gui_window_draw_rect(paint_win, 84, 2, 16, 16, 0xFFFF00);
    gui_window_draw_rect(paint_win, 104, 2, 16, 16, 0xFFFFFF);
    gui_window_draw_rect(paint_win, 124, 2, 16, 16, 0xFF00FF);
    gui_window_draw_rect(paint_win, 144, 2, 16, 16, 0x00FFFF);
    gui_window_draw_rect(paint_win, paint_win->w - 22, 2, 16, 16, paint_color);
    gui_window_draw_string(paint_win, "CLR", paint_win->w - 60, 6, 0x333333);
    gui_window_draw_rect(paint_win, paint_win->w - 110, 2, 45, 15, 0x444444);
    gui_window_draw_string(paint_win, "SAVE", paint_win->w - 105, 6, 0xFFFFFF);

    if (mouse_left_button && !prev_mouse_left) {
      int rel_x = mouse_x - paint_win->x;
      int rel_y = mouse_y - paint_win->y;
      if (rel_y >= 2 && rel_y < 17 && rel_x >= paint_win->w - 110 && rel_x < paint_win->w - 65) {
        uint32_t bmp_size = 0;
        uint8_t *bmp_data = bmp_create_from_window(paint_win, &bmp_size);
        if (bmp_data) {
          fat32_save_file("IMAGE.BMP", (char *)bmp_data, bmp_size);
          kfree(bmp_data);
          explorer_scanned = false;
        }
      }
    }
    if (mouse_left_button) {
      int rel_x = mouse_x - paint_win->x;
      int rel_y = mouse_y - paint_win->y;
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
          gui_window_draw_rect(paint_win, 0, 20, paint_win->w, paint_win->h - 20, 0xFFFFFF);
        }
      } else if (rel_y >= 20 && rel_x >= 0 && rel_x < paint_win->w && rel_y < paint_win->h) {
        if (paint_prev_x >= 0 && paint_prev_y >= 0) {
          gui_window_draw_line(paint_win, paint_prev_x, paint_prev_y, rel_x, rel_y, 1, paint_color);
        } else {
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
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

  // Explorer Logic
  if (explorer_win && explorer_win->active) {
    gui_window_draw_rect(explorer_win, 0, 0, explorer_win->w, explorer_win->h, 0xFFFFFF);
    gui_window_draw_rect(explorer_win, 0, 0, explorer_win->w, 24, 0xF0F0F0);
    gui_window_draw_string(explorer_win, "Path: / (All VFS Volumes)", 8, 7, 0x333333);
    gui_window_draw_rect(explorer_win, explorer_win->w - 60, 3, 52, 18, 0xDDDDDD);
    gui_window_draw_string(explorer_win, "REFR", explorer_win->w - 50, 7, 0x333333);
    gui_window_draw_rect(explorer_win, 0, 24, explorer_win->w, 1, 0xCCCCCC);

    if (!explorer_scanned) {
      explorer_file_count = 0;
      vfs_node_t *dev = vfs_root->next;
      while (dev && explorer_file_count < 256) {
        if (dev->readdir) {
          for (int i = 0; i < 32; i++) {
            vfs_dirent_t *de = dev->readdir(dev, i);
            if (!de) break;
            strcpy(real_files[explorer_file_count].name, de->name);
            real_files[explorer_file_count].size = de->size;
            real_files[explorer_file_count].dev = dev;
            real_files[explorer_file_count].inode = de->inode;
            explorer_file_count++;
            if (explorer_file_count >= 256) break;
          }
        }
        dev = dev->next;
      }
      explorer_scanned = true;
    }

    if (mouse_just_pressed) {
      int rx = mouse_x - explorer_win->x;
      int ry = mouse_y - explorer_win->y;
      if (rx >= explorer_win->w - 60 && rx < explorer_win->w - 8 && ry >= 3 && ry < 21) {
        explorer_scanned = false;
      }
    }

    int y_off = 30;
    if (explorer_file_count == 0) {
      gui_window_draw_string(explorer_win, "No files found on VFS.", 20, 40, 0x999999);
    }
    for (int i = 0; i < explorer_file_count; i++) {
      int row_y = y_off - 2;
      if (i % 2 == 0) gui_window_draw_rect(explorer_win, 0, row_y, explorer_win->w, 18, 0xF5F5F5);
      uint32_t icon_color = (strcmp(real_files[i].dev->name, "EXT2_DISK") == 0) ? 0x40C0F0 : 0xF0C040;
      gui_window_draw_rect(explorer_win, 5, y_off + 2, 8, 8, icon_color);
      gui_window_draw_string(explorer_win, real_files[i].name, 20, y_off, 0x000000);
      gui_window_draw_string(explorer_win, real_files[i].dev->name, explorer_win->w - 100, y_off, 0x888888);

      if (mouse_just_pressed) {
        int rx = mouse_x - explorer_win->x;
        int ry = mouse_y - explorer_win->y;
        if (rx > 0 && rx < explorer_win->w && ry >= row_y && ry < row_y + 18) {
          vfs_node_t *dev = real_files[i].dev;
          vfs_node_t file_node;
          memset(&file_node, 0, sizeof(vfs_node_t));
          file_node.inode = real_files[i].inode;
          strcpy(file_node.name, real_files[i].name);
          uint8_t *file_data = kmalloc(real_files[i].size + 1);
          uint32_t read_bytes = dev->read(&file_node, 0, real_files[i].size, file_data);
          if (read_bytes > 0) {
            file_data[read_bytes] = '\0';
            notepad_load_content((char *)file_data, read_bytes);
            notepad_win->active = true;
            window_bring_to_front(notepad_win);
          }
          kfree(file_data);
        }
      }
      y_off += 18;
    }
  }

  // Notepad Logic
  if (notepad_win && notepad_win->active) {
    gui_window_draw_rect(notepad_win, 0, 0, notepad_win->w, notepad_win->h, 0xFFFFFF);
    gui_window_draw_rect(notepad_win, 0, 0, notepad_win->w, 18, 0xF0F0F0);
    gui_window_draw_string(notepad_win, "Notepad - EquinoxOS", 8, 5, 0x333333);
    gui_window_draw_rect(notepad_win, 0, 18, notepad_win->w, 1, 0xCCCCCC);
    gui_window_draw_rect(notepad_win, notepad_win->w - 80, 2, 50, 15, 0x228B22);
    gui_window_draw_string(notepad_win, "SAVE", notepad_win->w - 75, 6, 0xFFFFFF);

    if (mouse_left_button && !prev_mouse_left) {
      int rx = mouse_x - notepad_win->x;
      int ry = mouse_y - notepad_win->y;
      if (rx >= notepad_win->w - 80 && rx < notepad_win->w - 30 && ry >= 2 && ry < 17) {
        char save_buffer[2048] = {0};
        for (int i = 0; i <= notepad_line; i++) {
          strcat(save_buffer, notepad_buf[i]);
          strcat(save_buffer, "\n");
        }
        vfs_node_t *dev = vfs_root->next;
        while (dev) {
          if (dev->write) {
            vfs_node_t file_node;
            memset(&file_node, 0, sizeof(vfs_node_t));
            strcpy(file_node.name, "NOTES.TXT");
            dev->write(&file_node, 0, strlen(save_buffer), (uint8_t *)save_buffer);
            break;
          }
          dev = dev->next;
        }
        explorer_scanned = false;
      }
    }
    for (int i = 0; i < NOTEPAD_MAX_LINES; i++) {
      gui_window_draw_string(notepad_win, notepad_buf[i], 8, 22 + i * 14, 0x000000);
    }
    int cx = 8 + notepad_col * 8;
    int cy = 22 + notepad_line * 14;
    /* 500 ms on / 500 ms off caret blink (tick is now in ms). */
    if ((tick / 500) % 2 == 0) {
      gui_window_draw_rect(notepad_win, cx, cy, 2, 10, 0x000000);
    }
  }

  prev_mouse_left = mouse_left_button;
  gui_compositor_render();
  vesa_update();
}
