# stb_image (vendored)

Source: https://github.com/nothings/stb — single-header public-domain image
loader. We vendor stb_image.h v2.30 verbatim.

## EquinoxOS integration

The implementation is compiled in exactly one file:
`sdk/lib_image/image_decode.c`. That file sets the usual `STB_*` overrides
and selects PNG/JPEG only (no HDR, no TGA, no GIF, no PIC, no PSD, no PNM)
to keep the link size down and avoid pulling in math functions we don't
have (no `ldexp` for HDR, no `pow` outside of stb_truetype).

The wrapper exposes a tiny API in `sdk/include/image_decode.h`:
```
typedef struct { uint8_t *rgba; int w, h; } eq_image_t;
int  eq_image_decode(const uint8_t *data, size_t len, eq_image_t *out);
void eq_image_free(eq_image_t *img);
```

`eq_image_decode()` always returns 4-channel RGBA8 regardless of the source
pixel format. Caller owns the buffer and must call `eq_image_free()`.
