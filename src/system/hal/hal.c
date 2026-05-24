#include "hal.h"
#include "../drivers/vesa/vesa.h"
#include "../drivers/hardware/disk/ata.h"
#include "../../syslibc/string.h"

extern void term_print(const char *str);

/* ---------- VESA → hal_display adapter ---------------------------- */

extern uint32_t screen_width, screen_height, screen_pitch;
extern uintptr_t fb_base_addr;

static uint32_t vesa_w(void)     { return screen_width;  }
static uint32_t vesa_h(void)     { return screen_height; }
static uint32_t vesa_pitch_f(void){ return screen_pitch; }
static uint32_t vesa_bpp(void)   { return 32;            }
static void    *vesa_fb(void)    { return (void *)fb_base_addr; }

static void vesa_put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 ||
        (uint32_t)x >= screen_width || (uint32_t)y >= screen_height) return;
    uint32_t *fb = (uint32_t *)fb_base_addr;
    fb[(uint32_t)y * (screen_pitch / 4) + (uint32_t)x] = color;
}

static void vesa_fill_rect_f(int x, int y, int w, int h, uint32_t color) {
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx)
            vesa_put_pixel(xx, yy, color);
}

static const hal_display_ops_t vesa_display_ops = {
    .name        = "vesa-lfb",
    .width       = vesa_w,
    .height      = vesa_h,
    .pitch       = vesa_pitch_f,
    .bpp         = vesa_bpp,
    .framebuffer = vesa_fb,
    .put_pixel   = vesa_put_pixel,
    .fill_rect   = vesa_fill_rect_f,
    .present     = NULL,
};

/* ---------- PS/2 → hal_input adapter ------------------------------- */

extern uint8_t keyboard_pop(void);
extern int  mouse_x, mouse_y;
extern bool mouse_left_button, mouse_right_button;

static uint8_t kbd_poll(void) { return keyboard_pop(); }

static void mouse_state(int *x, int *y, uint8_t *buttons) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (buttons)
        *buttons = (mouse_left_button  ? 1 : 0) |
                   (mouse_right_button ? 2 : 0);
}

static const hal_input_ops_t ps2_input_ops = {
    .name              = "ps2",
    .kbd_poll_scancode = kbd_poll,
    .mouse_state       = mouse_state,
};

/* ---------- ATA PIO → hal_block adapter --------------------------- */

static int ata_block_read(hal_block_ops_t *self,
                          uint64_t lba, uint32_t count, void *buf) {
    read_sectors_ata_pio((uintptr_t)buf, lba + self->lba_offset, count);
    return (int)count;
}

static int ata_block_write(hal_block_ops_t *self,
                           uint64_t lba, uint32_t count, const void *buf) {
    write_sectors_ata_pio((uintptr_t)buf, lba + self->lba_offset, count);
    return (int)count;
}

static hal_block_ops_t ata0_block = {
    .name         = "ata0",
    .sector_size  = 512,
    .sector_count = 0,             /* not introspected yet — 0 == unknown */
    .lba_offset   = 0,
    .read         = ata_block_read,
    .write        = ata_block_write,
};

/* ---------- Registry storage --------------------------------------- */

static const hal_display_ops_t *g_display = NULL;
static const hal_input_ops_t   *g_input   = NULL;

static hal_block_ops_t *g_blocks[HAL_MAX_BLOCK_DEVS] = {0};
static int              g_block_n = 0;

void hal_register_display(const hal_display_ops_t *ops) { g_display = ops; }
void hal_register_input  (const hal_input_ops_t   *ops) { g_input   = ops; }

const hal_display_ops_t *hal_display(void) { return g_display; }
const hal_input_ops_t   *hal_input  (void) { return g_input;   }

int hal_register_block(hal_block_ops_t *ops) {
    if (g_block_n >= HAL_MAX_BLOCK_DEVS) return -1;
    g_blocks[g_block_n] = ops;
    return g_block_n++;
}

hal_block_ops_t *hal_block(int id) {
    if (id < 0 || id >= g_block_n) return (hal_block_ops_t *)0;
    return g_blocks[id];
}

int hal_block_count(void) { return g_block_n; }

void hal_init(void) {
    hal_register_display(&vesa_display_ops);
    hal_register_input  (&ps2_input_ops);
    hal_register_block  (&ata0_block);
    term_print("[HAL] display=vesa-lfb input=ps2 block[0]=ata0\n");
}
