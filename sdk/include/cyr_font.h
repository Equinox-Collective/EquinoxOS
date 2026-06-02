#ifndef EQ_CYR_FONT_H
#define EQ_CYR_FONT_H
#include <stdint.h>
/* GNU Unifont 15.1.05 8x16 bitmaps for U+0400..U+04FF (Cyrillic Basic).
 * Indexed by (codepoint - 0x0400). Each glyph is 16 bytes (one byte
 * per row, MSB-first 8 pixels). Missing codepoints are all-zero. */
extern const uint8_t cyr_font_8x16[256][16];
#endif
