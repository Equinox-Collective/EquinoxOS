#include <eid_ext.h>
#include <eid.h>
#include <equos.h>
#include <string.h>

/* ----- Animation helper --------------------------------------------- */

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void eid_anim_init(eid_anim_t *a, float duration_ms, eid_ease_t ease) {
    a->value       = 0.0f;
    a->target      = 0.0f;
    a->duration_ms = duration_ms <= 0.0f ? 1.0f : duration_ms;
    a->elapsed_ms  = duration_ms;   /* "settled" by default */
    a->ease        = ease;
    a->active      = false;
}

void eid_anim_to(eid_anim_t *a, float target) {
    a->target     = clampf(target, 0.0f, 1.0f);
    a->elapsed_ms = 0.0f;
    a->active     = (a->value != a->target);
}

void eid_anim_step(eid_anim_t *a, float dt_ms) {
    if (!a->active) return;
    a->elapsed_ms += dt_ms;
    if (a->elapsed_ms >= a->duration_ms) {
        a->elapsed_ms = a->duration_ms;
        a->value      = a->target;
        a->active     = false;
        return;
    }
    float t = a->elapsed_ms / a->duration_ms;
    /* current = start + (target - start) * eased(t).
     * We don't store "start", so we instead linearly track toward target with
     * a stepped fraction — close enough for fade/slide UX. */
    float eased = t;
    switch (a->ease) {
        case EID_EASE_OUT_QUAD:    eased = 1.0f - (1.0f - t) * (1.0f - t); break;
        case EID_EASE_IN_OUT_QUAD:
            eased = t < 0.5f ? 2.0f * t * t
                             : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * 0.5f;
            break;
        case EID_EASE_OUT_CUBIC: {
            float k = 1.0f - t;
            eased = 1.0f - k * k * k;
            break;
        }
        case EID_EASE_LINEAR:
        default: eased = t; break;
    }
    a->value = (a->target == 1.0f) ? eased : 1.0f - eased;
}

float eid_anim_eval(const eid_anim_t *a) { return a->value; }

/* ----- Button -------------------------------------------------------- */

uint32_t eid_button(eid_ctx_t *ctx, const char *label, int x, int y, int w, int h) {
    uint32_t id = eid_get_id(label, x, y);
    uint32_t st = eid_process_interaction(ctx, id, x, y, w, h);

    uint32_t bg     = 0x2A2D34;
    if (st & EID_STATE_HOVER)  bg = 0x3A3F49;
    if (st & EID_STATE_ACTIVE) bg = 0x1E2127;

    /* shadow */
    eid_draw_rect(ctx->fb, ctx->win_w, ctx->win_h, x + 1, y + h, w, 2, 0x101116);
    /* body */
    eid_draw_rect(ctx->fb, ctx->win_w, ctx->win_h, x, y, w, h, bg);
    /* border */
    eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h, x,         y,         x + w - 1, y,         0x4A505C);
    eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h, x,         y + h - 1, x + w - 1, y + h - 1, 0x4A505C);
    eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h, x,         y,         x,         y + h - 1, 0x4A505C);
    eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h, x + w - 1, y,         x + w - 1, y + h - 1, 0x4A505C);

    int label_w = 8 * (int)strlen(label);
    int tx = x + (w - label_w) / 2;
    int ty = y + (h - 16) / 2;
    eid_draw_text(ctx->fb, ctx->win_w, ctx->win_h, tx, ty, label, 0xE6E6E6);
    return st;
}

/* ----- Checkbox ------------------------------------------------------ */

uint32_t eid_checkbox(eid_ctx_t *ctx, const char *label, int x, int y, bool *value) {
    int box = 18;
    uint32_t id = eid_get_id(label, x, y);
    uint32_t st = eid_process_interaction(ctx, id, x, y, box, box);

    if (st & EID_STATE_CLICKED) *value = !*value;

    uint32_t border = (st & EID_STATE_HOVER) ? 0x6FA8DC : 0x4A505C;
    eid_draw_rect(ctx->fb, ctx->win_w, ctx->win_h, x, y, box, box, 0x1E2127);
    eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h, x,           y,           x + box - 1, y,           border);
    eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h, x,           y + box - 1, x + box - 1, y + box - 1, border);
    eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h, x,           y,           x,           y + box - 1, border);
    eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h, x + box - 1, y,           x + box - 1, y + box - 1, border);

    if (*value) {
        /* Draw a chunky check mark. */
        for (int i = 0; i < 3; ++i) {
            eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h,
                          x + 4, y + 9 + i, x + 8, y + 13 + i, 0x6FA8DC);
            eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h,
                          x + 8, y + 13 + i, x + 15, y + 4 + i, 0x6FA8DC);
        }
    }

    /* label */
    eid_draw_text(ctx->fb, ctx->win_w, ctx->win_h,
                  x + box + 8, y + 2, label, 0xE6E6E6);
    return st;
}

/* ----- Slider -------------------------------------------------------- */

uint32_t eid_slider(eid_ctx_t *ctx, const char *label, int x, int y, int w,
                    float *value, float vmin, float vmax) {
    int track_h = 6;
    int track_y = y + 9;
    int thumb_w = 14;
    int thumb_h = 22;
    int reach = w - thumb_w;
    float norm = (*value - vmin) / (vmax - vmin);
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    int thumb_x = x + (int)(norm * (float)reach);

    uint32_t id = eid_get_id(label, x, y);
    uint32_t st = eid_process_interaction(ctx, id, x, y - 4, w, thumb_h + 4);

    if (st & EID_STATE_ACTIVE) {
        int px = ctx->mx - x - thumb_w / 2;
        if (px < 0)      px = 0;
        if (px > reach)  px = reach;
        float new_norm = (float)px / (float)reach;
        *value = vmin + new_norm * (vmax - vmin);
    }

    /* track */
    eid_draw_rect(ctx->fb, ctx->win_w, ctx->win_h, x, track_y, w, track_h, 0x1E2127);
    eid_draw_rect(ctx->fb, ctx->win_w, ctx->win_h, x, track_y, thumb_x - x + thumb_w / 2,
                  track_h, 0x6FA8DC);

    /* thumb */
    uint32_t tc = (st & EID_STATE_ACTIVE) ? 0xC8E0F4 : 0xE6E6E6;
    eid_draw_rect(ctx->fb, ctx->win_w, ctx->win_h, thumb_x, y, thumb_w, thumb_h, tc);
    return st;
}

/* ----- Text input ---------------------------------------------------- */
/*
 * Single line. Backspace removes last char, printable ASCII appends. The
 * caller is responsible for `buf` lifetime and initial NUL termination.
 */

char eid_scancode_to_ascii(uint8_t sc, bool shift) {
    /* Set 1 (PC/AT) make-codes for the main row. We only handle make-codes;
     * break codes (>= 0x80) are ignored. */
    if (sc & 0x80) return 0;
    static const char map_lo[128] = {
        0,   27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',  0, 'a','s',
        'd','f','g','h','j','k','l',';','\'','`',  0,'\\','z','x','c','v',
        'b','n','m',',','.','/',  0, '*',   0, ' ',   0,   0,   0,   0,   0,
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
          0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    };
    static const char map_hi[128] = {
        0,   27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b','\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',  0, 'A','S',
        'D','F','G','H','J','K','L',':','"','~',  0, '|','Z','X','C','V',
        'B','N','M','<','>','?',  0, '*',   0, ' ',   0,   0,   0,   0,   0,
    };
    return shift ? map_hi[sc] : map_lo[sc];
}

uint32_t eid_text_input(eid_ctx_t *ctx, const char *label, int x, int y,
                        int w, int h, char *buf, int buf_len) {
    uint32_t id = eid_get_id(label, x, y);
    uint32_t st = eid_process_interaction(ctx, id, x, y, w, h);

    bool focused = (st & EID_STATE_FOCUSED) != 0;

    /* When focused, consume the latest scancode. */
    if (focused && ctx->last_key) {
        char c = eid_scancode_to_ascii(ctx->last_key, false);
        int  len = (int)strlen(buf);
        if (c == '\b') {
            if (len > 0) buf[len - 1] = '\0';
        } else if (c >= 0x20 && c <= 0x7E) {
            if (len < buf_len - 1) {
                buf[len]     = c;
                buf[len + 1] = '\0';
            }
        }
        ctx->last_key = 0; /* consume */
    }

    /* draw */
    uint32_t border = focused ? 0x6FA8DC : 0x4A505C;
    eid_draw_rect(ctx->fb, ctx->win_w, ctx->win_h, x, y, w, h, 0x14161B);
    eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h, x,         y,         x + w - 1, y,         border);
    eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h, x,         y + h - 1, x + w - 1, y + h - 1, border);
    eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h, x,         y,         x,         y + h - 1, border);
    eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h, x + w - 1, y,         x + w - 1, y + h - 1, border);

    int ty = y + (h - 16) / 2;
    eid_draw_text(ctx->fb, ctx->win_w, ctx->win_h, x + 6, ty, buf, 0xE6E6E6);

    if (focused) {
        /* Caret: simple, no blink (since we don't track absolute time here). */
        int cw = 6 + 8 * (int)strlen(buf);
        if (cw < w - 4) {
            eid_draw_line(ctx->fb, ctx->win_w, ctx->win_h,
                          x + cw, ty, x + cw, ty + 14, 0xE6E6E6);
        }
    }
    return st;
}
