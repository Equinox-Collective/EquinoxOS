#ifndef HAL_H
#define HAL_H

/*
 * EquinoxOS Hardware Abstraction Layer (HAL).
 *
 * The point: subsystems above the HAL (GUI, VFS, input) should not know which
 * physical driver answers their requests. Today VESA + PS/2 + ATA PIO are the
 * only backends, but later (VirtIO-GPU, USB-HID, AHCI/NVMe) more can be plugged
 * in without rewriting callers.
 *
 * This file ONLY defines the interface and a small registry. The existing
 * drivers (vesa.c, mouse.c, keyboard.c, ata.c) keep working unchanged. New
 * code is encouraged to go through `hal_display()`, `hal_input()`, `hal_block()`
 * rather than calling drivers directly.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* --------------------------------------------------------------------- */
/* Display backend                                                       */
/* --------------------------------------------------------------------- */

typedef struct hal_display_ops {
    const char *name;

    /* Resolution & layout */
    uint32_t (*width)(void);
    uint32_t (*height)(void);
    uint32_t (*pitch)(void);   /* bytes per scanline */
    uint32_t (*bpp)(void);     /* bits per pixel    */

    /* Direct framebuffer access (linear, top-left origin, ARGB8888). */
    void *(*framebuffer)(void);

    /* Pixel ops (slow path; usually code blits via framebuffer ptr). */
    void (*put_pixel)(int x, int y, uint32_t color);
    void (*fill_rect)(int x, int y, int w, int h, uint32_t color);

    /* Optional double-buffer presenter. May be NULL if backend draws direct. */
    void (*present)(void);
} hal_display_ops_t;

/* --------------------------------------------------------------------- */
/* Input backend                                                         */
/* --------------------------------------------------------------------- */

typedef struct hal_input_ops {
    const char *name;

    /* Keyboard */
    uint8_t  (*kbd_poll_scancode)(void); /* 0 if no key */

    /* Pointer */
    void (*mouse_state)(int *x, int *y, uint8_t *buttons);
} hal_input_ops_t;

/* --------------------------------------------------------------------- */
/* Block device backend (one volume / partition)                         */
/* --------------------------------------------------------------------- */

typedef struct hal_block_ops {
    const char *name;          /* "ata0", "ahci0p1", "virtio0" ...        */
    uint32_t sector_size;      /* almost always 512                       */
    uint64_t sector_count;     /* total sectors of this volume            */
    uint64_t lba_offset;       /* offset added before issuing I/O         */

    int (*read)(struct hal_block_ops *self, uint64_t lba,
                uint32_t count, void *buf);
    int (*write)(struct hal_block_ops *self, uint64_t lba,
                 uint32_t count, const void *buf);
} hal_block_ops_t;

/* --------------------------------------------------------------------- */
/* Registry                                                              */
/* --------------------------------------------------------------------- */

#define HAL_MAX_BLOCK_DEVS 8

void hal_init(void);

/* Display / input have a single "primary" slot each. */
void hal_register_display(const hal_display_ops_t *ops);
void hal_register_input  (const hal_input_ops_t  *ops);

const hal_display_ops_t *hal_display(void);
const hal_input_ops_t   *hal_input  (void);

/* Block devices are a small array; ID is index into `hal_block(i)`. */
int  hal_register_block(hal_block_ops_t *ops); /* returns id, or -1 */
hal_block_ops_t *hal_block(int id);
int  hal_block_count(void);

#endif /* HAL_H */
