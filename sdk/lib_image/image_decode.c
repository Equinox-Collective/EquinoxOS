/*
 * sdk/lib_image/image_decode.c — RGBA decoder used by browser.elf.
 *
 * Thin wrapper around vendored stb_image (PNG + JPEG only). All other
 * decoders are stripped to keep the link size down. See
 * third_party/stb_image/README.equos.md.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <image_decode.h>

/* ---- stb_image configuration --------------------------------------- */

/* We don't have stdio file ops in user space the way stb expects, and
 * we never load from disk via stb anyway — only from in-memory blobs
 * produced by eq_http_get(). */
#define STBI_NO_STDIO

/* Drop everything we don't ship to keep .text small. PNG covers the
 * vast majority of web assets; JPEG covers the rest. HDR / GIF / TGA /
 * PSD / PIC / PNM all add code that we can't justify in a hobby
 * browser. */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG

/* Strip the human-readable failure strings — we don't surface them and
 * they pull in a bunch of literal storage. */
#define STBI_NO_FAILURE_STRINGS

/* No threading primitives available in user space. */
#define STBI_NO_THREAD_LOCALS

/* Route allocations through our malloc(), so memory shows up on the
 * normal user-heap accounting and stb's per-call buffers get returned
 * to the heap on eq_image_free(). */
#define STBI_MALLOC(sz)        malloc(sz)
#define STBI_REALLOC(p, sz)    realloc((p), (sz))
#define STBI_FREE(p)           free(p)

/* stb's default uses assert() from <assert.h>; we don't have a real
 * one in user space, so silence it. Bad inputs end up as decode
 * failures via the normal stbi__err() path. */
#define STBI_ASSERT(x)         ((void)0)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ---- Public API ---------------------------------------------------- */

int eq_image_decode(const uint8_t *data, size_t len, eq_image_t *out) {
  if (!out) return -1;
  out->rgba = NULL; out->w = 0; out->h = 0;
  if (!data || len == 0) return -1;

  int w = 0, h = 0, n = 0;
  /* Force 4 channels — easier to blit, no per-pixel branch. */
  stbi_uc *pixels =
      stbi_load_from_memory((const stbi_uc *)data, (int)len, &w, &h, &n, 4);
  if (!pixels) return -1;

  out->rgba = (uint8_t *)pixels;
  out->w = w;
  out->h = h;
  return 0;
}

void eq_image_free(eq_image_t *img) {
  if (!img || !img->rgba) return;
  stbi_image_free(img->rgba);
  img->rgba = NULL;
  img->w = 0;
  img->h = 0;
}
