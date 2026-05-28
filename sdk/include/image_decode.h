#ifndef _EQUOS_IMAGE_DECODE_H
#define _EQUOS_IMAGE_DECODE_H

/*
 * Tiny PNG/JPEG decode wrapper used by browser.elf to turn fetched image
 * bytes into a flat RGBA8888 buffer that the framebuffer blitter can
 * consume directly.
 *
 * Implementation lives in sdk/lib_image/image_decode.c and is a one-line
 * wrapper around vendored stb_image.h (PNG + JPEG only, no HDR / GIF /
 * TGA / PSD / PIC / PNM — see third_party/stb_image/README.equos.md).
 */

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t *rgba;   /* w*h*4 bytes, freshly malloc'd, RGBA byte order  */
  int      w, h;   /* pixel dimensions                                */
} eq_image_t;

/* Decode the given image buffer into a freshly allocated RGBA8888
 * surface. Returns 0 on success, -1 on decode failure. On failure
 * `out` is zeroed. */
int  eq_image_decode(const uint8_t *data, size_t len, eq_image_t *out);

/* Release the RGBA buffer returned by eq_image_decode(). Safe on a
 * zero-initialised struct. */
void eq_image_free(eq_image_t *img);

#endif /* _EQUOS_IMAGE_DECODE_H */
