#ifndef EID_EXT_H
#define EID_EXT_H

/*
 * EID extension: advanced widgets and lightweight animations.
 *
 *   - eid_button       : clickable button with hover / active visuals.
 *   - eid_checkbox     : two-state checkbox tied to a `bool *value`.
 *   - eid_text_input   : editable single-line text field; consumes keys via
 *                        ctx->last_key (PS/2 scancodes — see scancode_to_ascii).
 *   - eid_slider       : horizontal float slider (drag thumb).
 *
 *   - eid_anim_t       : tiny animation helper (linear / ease-out).
 *
 * All widgets return their final state mask (EID_STATE_*) for the caller.
 *
 * Animations are completely independent of the immediate-mode rendering — you
 * call `eid_anim_step(&a, dt_ms)` once per frame and read `a.value` (0..1).
 */

#include "eid.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Animation helper ---------------------------------------------- */

typedef enum {
    EID_EASE_LINEAR = 0,
    EID_EASE_OUT_QUAD,
    EID_EASE_IN_OUT_QUAD,
    EID_EASE_OUT_CUBIC,
} eid_ease_t;

typedef struct {
    float    value;          /* current 0..1                              */
    float    target;         /* desired 0..1                              */
    float    duration_ms;
    float    elapsed_ms;
    eid_ease_t ease;
    bool     active;
} eid_anim_t;

void  eid_anim_init(eid_anim_t *a, float duration_ms, eid_ease_t ease);
void  eid_anim_to  (eid_anim_t *a, float target);   /* restart toward new */
void  eid_anim_step(eid_anim_t *a, float dt_ms);
float eid_anim_eval(const eid_anim_t *a);           /* eased current      */

/* ---- Widgets -------------------------------------------------------- */

uint32_t eid_button   (eid_ctx_t *ctx, const char *label,
                       int x, int y, int w, int h);

uint32_t eid_checkbox (eid_ctx_t *ctx, const char *label,
                       int x, int y, bool *value);

/* `buf` must be at least `buf_len` bytes, NUL-terminated on entry.
 * Returns mask; the input becomes editable while focused. */
uint32_t eid_text_input(eid_ctx_t *ctx, const char *label,
                        int x, int y, int w, int h,
                        char *buf, int buf_len);

uint32_t eid_slider   (eid_ctx_t *ctx, const char *label,
                       int x, int y, int w,
                       float *value, float vmin, float vmax);

/* Convert a PS/2 scancode (set 1) to a printable ASCII char, 0 if none. */
char eid_scancode_to_ascii(uint8_t sc, bool shift);

#ifdef __cplusplus
}
#endif

#endif /* EID_EXT_H */
