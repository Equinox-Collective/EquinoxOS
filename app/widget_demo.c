/*
 * widget_demo - EID widget set + animations demo for EquinoxOS.
 *
 * Window position is hard-coded to match gui_init() in src/gui/gui.c:
 *   app_win = window_create(100, 100, 400, 300, "Application");
 * Do not drag the window — there is no SYS_GET_WIN_POS to refresh offsets.
 */

#include <eid.h>
#include <eid_ext.h>
#include <equos.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define WIN_X 100
#define WIN_Y 100
#define WIN_W 360
#define WIN_H 320

/* Field width 200 px / 8 px per glyph ≈ 25 visible chars.
 * Cap the buffer at that so text cannot run past the right edge. */
#define NAME_FIELD_W 200
#define NAME_MAX     24                 /* 24 chars + NUL = 25 cells */

static uint32_t framebuffer[WIN_W * WIN_H];

int main(void) {
    eid_init();

    eid_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    bool   check1 = false;
    bool   check2 = true;
    float  volume = 0.6f;
    char   name_buf[NAME_MAX + 1] = "Equinox";

    eid_anim_t panel_fade;
    eid_anim_init(&panel_fade, 220.0f, EID_EASE_OUT_CUBIC);
    eid_anim_to(&panel_fade, 1.0f);

    uint32_t last_tick = (uint32_t)_syscall(SYS_GET_TIME, 0, 0, 0, 0, 0);

    while (1) {
        uint32_t now = (uint32_t)_syscall(SYS_GET_TIME, 0, 0, 0, 0, 0);
        float dt = (float)(now - last_tick);
        if (dt < 0.0f) dt = 0.0f;
        last_tick = now;
        eid_anim_step(&panel_fade, dt);

        eid_begin(&ctx, framebuffer, WIN_W, WIN_H);
        ctx.mx -= WIN_X;
        ctx.my -= WIN_Y;

        /* background */
        eid_draw_rect(framebuffer, WIN_W, WIN_H, 0, 0, WIN_W, WIN_H, 0x14161B);

        /* slide-in header */
        float t = eid_anim_eval(&panel_fade);
        int hx = (int)((1.0f - t) * -200.0f);
        eid_draw_gradient_rect(framebuffer, WIN_W, WIN_H,
                               hx, 0, WIN_W, 40, 0x2A4D8C, 0x1A2D58, true);
        eid_draw_text(framebuffer, WIN_W, WIN_H, hx + 12, 12,
                      "Equinox UI demo", 0xFFFFFF);

        /* "Reset name" button — now does something self-explanatory */
        if (eid_button(&ctx, "Reset name", 16, 60, 110, 28) & EID_STATE_CLICKED) {
            strcpy(name_buf, "Equinox");
        }

        /* checkboxes — independent state, not touched by the button */
        eid_checkbox(&ctx, "Enable feature A", 16, 110, &check1);
        eid_checkbox(&ctx, "Show advanced",     16, 140, &check2);

        /* slider */
        eid_draw_text(framebuffer, WIN_W, WIN_H, 16, 178, "Volume", 0xAAAAAA);
        eid_slider(&ctx, "vol", 16, 200, 200, &volume, 0.0f, 1.0f);

        /* text input — buffer capped to NAME_MAX so the visible string
         * cannot exceed the field width. */
        eid_draw_text(framebuffer, WIN_W, WIN_H, 16, 244, "Name:", 0xAAAAAA);
        eid_text_input(&ctx, "name", 70, 240, NAME_FIELD_W, 24,
                       name_buf, (int)sizeof(name_buf));

        /* footer status */
        char status[80] = "[ ";
        strcat(status, check1 ? "A=on" : "A=off");
        strcat(status, ", ");
        strcat(status, check2 ? "B=on" : "B=off");
        strcat(status, " ]");
        eid_draw_text(framebuffer, WIN_W, WIN_H, 16, 286, status, 0x9AC8FF);

        eid_end(&ctx, WIN_X, WIN_Y);
        sys_sleep(16);
    }
    return 0;
}
