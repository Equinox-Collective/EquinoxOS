#include <eid.h>
#include <eid_ext.h>
#include <equos.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef BROWSER_BUILD
#include "qjs_window.h"   /* qjs_window_set_history_length proto */
#include <image_decode.h> /* eq_image_decode for <img> rendering    */
#endif
/* dom.h has no BROWSER_BUILD gates and the DOM walker (w_emit_node)
 * is shared by both builds — include it unconditionally so
 * apply_css_for_node can reference dom_node_t in either config. */
#include "dom.h"

/* Defined in sdk/lib/string.c but not declared in sdk/include/string.h yet.
 * Pulled forward here to silence -Wimplicit-function-declaration in the
 * <input> type= matcher below. */
extern int strcasecmp(const char *s1, const char *s2);

#define WIN_W 640
#define WIN_H 420
#define CONTENT_X 18
#define CONTENT_Y 56
#define CONTENT_W (WIN_W - 36)
#define LINE_H 18
#define MAX_LINES 512
#define LINE_CHARS 74

#define CLR_BG 0xFDFCFC
#define CLR_CHROME 0x1A1C1E
#define CLR_CHROME_2 0x2C2F33
#define CLR_TEXT 0x202124
#define CLR_MUTED 0x5F6368
#define CLR_H1 0x174EA6
#define CLR_H2 0x188038
#define CLR_LINK 0x1A73E8
#define CLR_CODE_BG 0xF1F3F4
#define CLR_CODE 0xB04214
#define CLR_BORDER 0xDADCE0
#define CLR_ACCENT 0x4285F4

/* ── Forward Declarations ────────────────────────────────────── */
static void load_page(const char *url);
static void render(const char *filename);
static void blank_line(void);
static void parse_css_declarations_to_state(const char *decl);

/* ── R6/B2: UTF-8 → ASCII fallback ─────────────────────────────
 *
 * Our PSF1 console font has only 256 glyphs (ASCII + a CP437-ish
 * upper half), so any UTF-8 multi-byte sequence reaching it shows up
 * as garbled high-bit pairs ("ÔÇö" for "—", etc.). Modern hand-written
 * pages contain *lots* of punctuation in U+2000-2FFF (— … → ★ · ™),
 * and our target site is bilingual EN/RU, so we also transliterate
 * Cyrillic into Latin so Russian text remains roughly readable.
 *
 * The pipeline runs once per push_line(): decode UTF-8 → codepoint,
 * substitute via the table below, drop anything else. ASCII is
 * passed through unchanged. */

/* Decode the first UTF-8 codepoint at *p, returning bytes consumed
 * (1..4). Sets *cp to the codepoint, or U+FFFD on a malformed
 * sequence (and consumes a single byte). */
static int utf8_decode_one(const unsigned char *p, int rem, uint32_t *cp) {
  if (rem <= 0) { *cp = 0; return 0; }
  unsigned char b0 = p[0];
  if (b0 < 0x80) { *cp = b0; return 1; }
  if ((b0 & 0xE0) == 0xC0 && rem >= 2 && (p[1] & 0xC0) == 0x80) {
    *cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
    return 2;
  }
  if ((b0 & 0xF0) == 0xE0 && rem >= 3 &&
      (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
    *cp = ((uint32_t)(b0 & 0x0F) << 12) |
          ((uint32_t)(p[1] & 0x3F) << 6) |
          (uint32_t)(p[2] & 0x3F);
    return 3;
  }
  if ((b0 & 0xF8) == 0xF0 && rem >= 4 &&
      (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
      (p[3] & 0xC0) == 0x80) {
    *cp = ((uint32_t)(b0 & 0x07) << 18) |
          ((uint32_t)(p[1] & 0x3F) << 12) |
          ((uint32_t)(p[2] & 0x3F) << 6) |
          (uint32_t)(p[3] & 0x3F);
    return 4;
  }
  *cp = 0xFFFD;
  return 1;
}

/* codepoint → ASCII string (possibly multi-char). NULL means
 * "no substitution defined". */
static const char *cp_to_ascii_fallback(uint32_t cp) {
  switch (cp) {
    /* Latin-1 punctuation */
    case 0x00A0: return " ";   /* nbsp                     */
    case 0x00A9: return "(c)"; /* ©                        */
    case 0x00AE: return "(r)"; /* ®                        */
    case 0x00B0: return "*";   /* °                        */
    case 0x00B7: return "*";   /* ·                        */
    case 0x00AB: return "<<";  /* «                        */
    case 0x00BB: return ">>";  /* »                        */
    case 0x00D7: return "x";   /* ×                        */
    case 0x00F7: return "/";   /* ÷                        */
    /* General punctuation */
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: return "-";
    case 0x2014: return "--";  /* em dash                  */
    case 0x2018: case 0x2019: return "'";
    case 0x201C: case 0x201D: return "\"";
    case 0x2022: return "*";   /* bullet                   */
    case 0x2026: return "..."; /* ellipsis                 */
    case 0x2032: return "'";
    case 0x2033: return "\"";
    /* Arrows */
    case 0x2190: return "<-";
    case 0x2191: return "^";
    case 0x2192: return "->";
    case 0x2193: return "v";
    case 0x2194: return "<>";
    case 0x2196: return "<-";
    case 0x2197: return "->";
    case 0x2198: return "->";
    case 0x2199: return "<-";
    /* Misc symbols */
    case 0x2605: return "*";   /* ★                        */
    case 0x2606: return "*";   /* ☆                        */
    case 0x2713: return "+";   /* ✓                        */
    case 0x2717: return "x";   /* ✗                        */
    case 0x2122: return "(tm)";
    default: return NULL;
  }
}

/* Cyrillic transliteration table (U+0410..U+044F).
 * Capital/lowercase pair-by-pair so we can index by (cp - 0x0410). */
static const char *cyr_translit[] = {
  /* 0410 А */ "A",  "B",  "V",  "G",  "D",  "E",  "Zh", "Z",
  /* 0418 И */ "I",  "J",  "K",  "L",  "M",  "N",  "O",  "P",
  /* 0420 Р */ "R",  "S",  "T",  "U",  "F",  "H",  "C",  "Ch",
  /* 0428 Ш */ "Sh", "Sch","\"", "Y",  "'",  "E",  "Yu", "Ya",
  /* 0430 а */ "a",  "b",  "v",  "g",  "d",  "e",  "zh", "z",
  /* 0438 и */ "i",  "j",  "k",  "l",  "m",  "n",  "o",  "p",
  /* 0440 р */ "r",  "s",  "t",  "u",  "f",  "h",  "c",  "ch",
  /* 0448 ш */ "sh", "sch","\"", "y",  "'",  "e",  "yu", "ya",
};

static const char *cp_to_translit(uint32_t cp) {
  if (cp >= 0x0410 && cp <= 0x044F) return cyr_translit[cp - 0x0410];
  if (cp == 0x0401) return "Yo";   /* Ё */
  if (cp == 0x0451) return "yo";   /* ё */
  return NULL;
}

/* R6/B2c: convert UTF-8 input to *renderer-ready* UTF-8 output.
 *
 * Cyrillic Basic (U+0400..U+04FF) now passes straight through —
 * eid_draw_text knows how to decode UTF-8 and look up the bundled
 * GNU Unifont Cyrillic glyphs. Everything else stays on the old
 * path: typographic ASCII subs (en-dash → "--", curly quotes →
 * "'") and the Cyrillic *transliteration* table is kept ONLY as a
 * fallback for codepoints outside Basic Cyrillic that previously
 * had hand-coded substitutions (the Ё/ё specials in
 * cp_to_translit). Unknown non-ASCII codepoints are dropped
 * silently (the alternative — '?' — would litter every page that
 * has any symbol we forgot).
 *
 * Returns bytes written (NOT including NUL). out_cap counts bytes,
 * not codepoints. */
static int utf8_to_ascii(const char *in, int in_len, char *out, int out_cap) {
  if (out_cap <= 0) return 0;
  int w = 0;
  const unsigned char *p = (const unsigned char *)in;
  int rem = in_len;
  while (rem > 0 && w < out_cap - 1) {
    uint32_t cp = 0;
    int n = utf8_decode_one(p, rem, &cp);
    if (n <= 0) break;
    p += n; rem -= n;

    if (cp < 0x80) {
      out[w++] = (char)cp;
      continue;
    }
    /* Cyrillic Basic — pass through as raw UTF-8 (2 bytes/codepoint). */
    if (cp >= 0x0400 && cp <= 0x04FF) {
      /* re-encode cp as 2-byte UTF-8 (every value in the range fits) */
      if (w + 2 >= out_cap) break;
      out[w++] = (char)(0xC0 | (cp >> 6));
      out[w++] = (char)(0x80 | (cp & 0x3F));
      continue;
    }
    const char *sub = cp_to_ascii_fallback(cp);
    if (!sub) sub = cp_to_translit(cp);
    if (!sub) continue;
    while (*sub && w < out_cap - 1) out[w++] = *sub++;
  }
  out[w] = 0;
  return w;
}

/* Count visible cells (codepoints) in the first `nbytes` bytes of a
 * UTF-8 string buffer. Stops at NUL even if nbytes is larger. */
static int utf8_cells_n(const char *s, int nbytes) {
  int cells = 0;
  for (int i = 0; i < nbytes && s[i]; ) {
    unsigned char b = (unsigned char)s[i];
    int adv = (b < 0x80) ? 1
            : ((b & 0xE0) == 0xC0) ? 2
            : ((b & 0xF0) == 0xE0) ? 3
            : ((b & 0xF8) == 0xF0) ? 4 : 1;
    if (i + adv > nbytes) break;
    cells++; i += adv;
  }
  return cells;
}

/* Count visible columns in a UTF-8 string: each ASCII byte or each
 * UTF-8 codepoint is exactly one 8-px cell in our renderer. */
static int utf8_visible_cols(const char *s) {
  if (!s) return 0;
  int n = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; ) {
    if (*p < 0x80) { n++; p++; }
    else if ((*p & 0xE0) == 0xC0) { n++; p += 2; }
    else if ((*p & 0xF0) == 0xE0) { n++; p += 3; }
    else if ((*p & 0xF8) == 0xF0) { n++; p += 4; }
    else { n++; p++; }
  }
  return n;
}

/* ── CSS Engine ────────────────────────────────────────────────── */

/* Real-world pages (e.g. the EquinoxOS landing page) push past
 * the old 128-rule limit. With ~200 rules in their stylesheet,
 * everything past .feat-grid / .hero-grid / .stats was silently
 * truncated — which made the L5 grid layout look like a no-op
 * because the matching display:grid rules were never stored.
 * 512 leaves comfortable headroom for the bigger sites we
 * actually try to render. */
#define MAX_CSS_RULES 512
#define MAX_SELECTOR 64
#define MAX_CSS_CLASS 128

typedef enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } text_align_t;

typedef struct {
  char selector[MAX_SELECTOR]; /* e.g. "h1", ".intro", "#main" */
  uint32_t color;              /* text colour        (0 = unset) */
  uint32_t bg_color;           /* background colour  (0 = unset) */
  bool underline;              /* text-decoration: underline      */
  int margin_top;              /* blank lines before (0-2)        */
  int margin_bottom;           /* blank lines after  (0-2)        */
  bool bold;                   /* font-weight: bold               */
  text_align_t align;          /* text-align property             */
  bool has_color;
  bool has_bg;
  bool has_align;
  bool display_none;
  bool block_bg; /* Background should fill full width */
  int padding;   /* padding in line-units (0-6)       */
  bool has_padding;
  int max_width;  /* max-width in pixels (0 = unset)   */
  int font_size;  /* font-size hint: 0=normal, 1=large, 2=xlarge */
  bool uppercase; /* text-transform: uppercase         */
  /* R6/L4: flex (and L5: grid) container properties.
   *   display: 0 = unset (inherit/block), 1 = flex, 2 = grid
   *            (display:none is encoded in `display_none` above).
   *   flex_dir: 0 = row, 1 = column.
   *   gap: pixels between flex/grid items (both axes).
   *   justify: 0 = start, 1 = end, 2 = center, 3 = space-between.
   *   align_items: 0 = stretch, 1 = start, 2 = end, 3 = center.
   *   flex_grow / flex_basis: item-level. Read by the *parent*
   *     flex container when distributing remaining width.
   *     flex_basis=0 means "auto" (intrinsic). */
  int display;
  int flex_dir;
  int gap_px;
  int justify;
  int align_items;
  int flex_grow;
  int flex_basis;
  /* L5 (grid): raw `grid-template-columns` string, parsed by the
   * container when laying out children. */
  char grid_cols[64];
} css_rule_t;

static css_rule_t css_rules[MAX_CSS_RULES];
static int css_rule_count = 0;
static uint32_t body_bg = CLR_BG;

/* ── Line model (extended) ─────────────────────────────────────── */

typedef enum {
  STYLE_NORMAL,
  STYLE_H1,
  STYLE_H2,
  STYLE_LINK,
  STYLE_CODE,
  STYLE_MUTED,
  STYLE_BULLET,
  STYLE_HR,
  /* Phase R4/F0: interactive widgets. The line text is the label
   * (button) or the current value (input); widget_node points at
   * the originating DOM node so dispatched events know their
   * target. */
  STYLE_BUTTON,
  /* Phase R4/F1: editable single-line input. widget_node points at
   * the `<input>` DOM element; its current text is stored on the
   * `value` attribute and read/written per frame by the renderer
   * (it round-trips through eid_text_input's caller-owned buffer). */
  STYLE_INPUT,
  /* Phase R5/N2: toggle widget for <input type=checkbox> and
   * <input type=radio>. widget_node points at the input. The
   * "checked" DOM attribute carries the on/off state. */
  STYLE_CHECKBOX,
  /* Phase R5/N2: <select>. widget_node points at the select node;
   * its "value" attribute holds the selected <option>'s value. */
  STYLE_SELECT,
  /* Phase R6/B4: image line. The line itself takes up a vertical
   * slot wider than LINE_H; the renderer reads the image dimensions
   * from g_images[image_idx] and advances cur_y by image_h + pad. */
  STYLE_IMAGE,
} line_style_t;

/* R6/B2c: text buffer now holds UTF-8 so it must be sized in bytes,
 * not characters. Cyrillic is 2 bytes per codepoint, other BMP
 * symbols up to 3, so 4× LINE_CHARS leaves headroom for the rare
 * mixed line. */
#define LINE_BYTES (LINE_CHARS * 4 + 1)
typedef struct {
  char text[LINE_BYTES];
  line_style_t style;
  bool indent;
  /* CSS overrides – per line */
  uint32_t css_color; /* 0 = use default from style */
  uint32_t css_bg;    /* 0 = none                   */
  bool css_underline;
  bool css_bold;
  text_align_t css_align;
  bool full_width_bg;
  int padding;    /* padding hint (line-units) */
  int font_size;  /* 0=normal, 1=large, 2=xlarge */
  bool uppercase; /* text-transform: uppercase */
  char link_url[128];
  /* Phase R4/F0: widget hookup. NULL for plain text lines.
   * Stored as void* to avoid a forward decl above dom.h's include. */
  void *widget_node;
  /* Phase R6/B4: index into g_images[] for STYLE_IMAGE lines, -1
   * for everything else. */
  int image_idx;
  /* Phase R6/L1: pixel-positioned line box.
   * box_x is relative to CONTENT_X (so a normal full-width line has
   * box_x=0). box_y is the cumulative document Y of this line's top,
   * relative to CONTENT_Y+14 (i.e. before the scroll offset is
   * subtracted). box_w is the content area available for this line
   * (CONTENT_W - box_x by default). box_h is the line's full
   * vertical advance — usually LINE_H, but widgets (button/input/
   * checkbox/select) take LINE_H+6, and STYLE_IMAGE takes
   * image_h + 2*PAD + 6.
   *
   * Default behaviour: push_line() fills these from the current
   * layout frame (see layout_stack), so the existing
   * one-line-per-row pipeline is preserved bit-for-bit when the
   * root frame is the only one on the stack. Flex/grid containers (L4+L5) will
   * overwrite box_x/box_y/box_w on direct children to splice them
   * into columns. */
  int box_x;
  int box_y;
  int box_w;
  int box_h;
} line_t;

static uint32_t fb[WIN_W * WIN_H];
static eid_ctx_t ui;

#ifdef BROWSER_BUILD
/* ── Phase R6/B4: in-document image cache ────────────────────────────
 * <img> tags get fetched and decoded into a fixed-size table. The
 * line stream stores indices into this table via line_t::image_idx;
 * render() reads dimensions from here to advance cur_y by the right
 * amount and to blit the pixels.
 *
 * The table is owned by the htmlview instance and cleared at the
 * start of every parse_html() so we don't leak across navigations.
 * Cap is intentionally small — a hobby browser doesn't need to
 * juggle hundreds of images. */
#define MAX_DOC_IMAGES 8
static eq_image_t g_images[MAX_DOC_IMAGES];
static int        g_image_count;

static void images_reset(void) {
  for (int i = 0; i < g_image_count; i++) eq_image_free(&g_images[i]);
  g_image_count = 0;
}

/* Forward declared so w_emit_node's <img> handler (defined well above
 * the BROWSER_BUILD section) can call it. Real implementation lives
 * down with load_page() because it needs eq_http_get / resolve_url. */
static int load_image_for_src(const char *src);
#else
static int load_image_for_src(const char *src) { (void)src; return -1; }
#endif
static line_t lines[MAX_LINES];
static int line_count = 0;
static int scroll_line = 0;

/* Phase R6/L2: layout context stack.
 *
 * Each frame describes a rectangular sub-area of the document into
 * which subsequent push_line() calls write. The bottom-most frame
 * covers the full content area; nested frames (created by flex/grid
 * containers in L4+L5) carve out narrower sub-rectangles whose
 * children's box_x/box_y are anchored against the frame, not the
 * page root.
 *
 * Fields:
 *   x       — left offset (px) where the next line's box_x starts,
 *             relative to the content area (CONTENT_X anchors at
 *             render time).
 *   y       — current Y cursor (px) within the frame; advanced by
 *             push_line and layout_extend_last/layout_set_last_height.
 *             Each frame's y starts at the parent's y at push time
 *             and advances independently — when we pop, the parent
 *             absorbs (y - start_y) so its own cursor catches up.
 *   w       — content width available for children (px).
 *   start_y — y at which this frame was pushed (in document space).
 *             Used so pop can report frame height to the parent.
 *   flow    — reserved for L4/L5: 0=block stream (current), 1=flex-row,
 *             2=flex-col, 3=grid. L2 only emits flow=0.
 *
 * L2 keeps everything visually a no-op: a single root frame with
 * x=0, y=0, w=CONTENT_W is pushed at the start of every parse
 * pass, and push_line still produces a vertical text stream. The
 * push/pop machinery is in place but unused until L3+. */
typedef struct {
  int x;
  int y;
  int w;
  int start_y;
  int flow;
} layout_frame_t;

#define LAYOUT_MAX_DEPTH 16
static layout_frame_t layout_stack[LAYOUT_MAX_DEPTH];
static int layout_depth = 0;

static inline layout_frame_t *layout_top(void) {
  return &layout_stack[layout_depth];
}

/* Reset to a single root frame covering the content area.
 * Called at the top of every parse pass. */
static void layout_reset(void) {
  layout_depth = 0;
  layout_stack[0].x       = 0;
  layout_stack[0].y       = 0;
  layout_stack[0].w       = CONTENT_W;
  layout_stack[0].start_y = 0;
  layout_stack[0].flow    = 0;
}

/* Push a sub-frame. Reserved for L3+ (render_subtree, flex/grid).
 * Returns NULL on overflow without disturbing the stack. */
static layout_frame_t *layout_push(int x, int y, int w, int flow) {
  if (layout_depth + 1 >= LAYOUT_MAX_DEPTH) return NULL;
  layout_depth++;
  layout_frame_t *f = &layout_stack[layout_depth];
  f->x       = x;
  f->y       = y;
  f->w       = w;
  f->start_y = y;
  f->flow    = flow;
  return f;
}

/* Pop the current frame, returning its consumed height (y - start_y).
 * Does *not* propagate height up; callers (L4/L5 containers) decide
 * how to splice the popped height into the parent cursor. */
static int layout_pop(void) {
  if (layout_depth == 0) return 0;
  int h = layout_stack[layout_depth].y - layout_stack[layout_depth].start_y;
  layout_depth--;
  return h;
}

/* Default horizontal box for a normal stream line. Indented bullets
 * shrink the available width by 18 px (matching the legacy indent
 * offset in render()). */
#define LAYOUT_DEFAULT_INDENT 18
static char page_title[64] = "index.html";
static char current_url[128] = "res/index.html";
static char history[16][128];
static int history_ptr = -1;
static bool is_navigating_history = false;
static bool is_typing_url = false;
static int url_cursor = 0;

static void push_history(const char *url) {
  if (history_ptr < 15) {
    history_ptr++;
    strcpy(history[history_ptr], url);
  }
#ifdef BROWSER_BUILD
  qjs_window_set_history_length(history_ptr + 1);
#endif
}



/* ── Style Stack ────────────────────────────────────────────────── */

typedef struct {
  uint32_t color;
  uint32_t bg;
  bool underline;
  bool bold;
  text_align_t align;
  bool display_none;
  bool full_width_bg;
  int padding;
  int max_width;
  int font_size;
  bool uppercase;
  /* R6/L4+L5: container CSS that controls how the *children* of
   * the current element are laid out. NOT inherited; these are
   * read from the element's own CSS rule and used to dispatch
   * to emit_flex_container / emit_grid_container instead of the
   * normal recursive emit. */
  int display;
  int flex_dir;
  int gap_px;
  int justify;
  int align_items;
  int flex_grow;
  int flex_basis;
  char grid_cols[64];
} style_state_t;

static eid_font_t *h_font = NULL;
static eid_font_t *h_font_large = NULL;

#define MAX_STYLE_STACK 64
static style_state_t style_stack[MAX_STYLE_STACK];
static int style_depth = 0;

static void reset_style_stack(void) {
  style_depth = 0;
  memset(&style_stack[0], 0, sizeof(style_state_t));
  style_stack[0].align = ALIGN_LEFT;
}

static void push_style_state(void) {
  if (style_depth < MAX_STYLE_STACK - 1) {
    style_stack[style_depth + 1] = style_stack[style_depth];
    style_depth++;
  }
}

static void apply_css_to_current_state(const css_rule_t *r) {
  if (r->has_color)
    style_stack[style_depth].color = r->color;
  if (r->has_bg) {
    style_stack[style_depth].bg = r->bg_color;
    style_stack[style_depth].full_width_bg = true;
  }
  if (r->underline)
    style_stack[style_depth].underline = true;
  if (r->bold)
    style_stack[style_depth].bold = true;
  if (r->has_align)
    style_stack[style_depth].align = r->align;
  if (r->display_none)
    style_stack[style_depth].display_none = true;
  if (r->has_padding && r->padding > 0)
    style_stack[style_depth].padding = r->padding;
  if (r->max_width > 0)
    style_stack[style_depth].max_width = r->max_width;
  if (r->font_size > 0)
    style_stack[style_depth].font_size = r->font_size;
  if (r->uppercase)
    style_stack[style_depth].uppercase = true;
  /* R6/L4+L5: container layout. These OVERWRITE rather than OR
   * because they describe how this element's children are laid
   * out; a flex/grid declaration replaces any inherited value. */
  if (r->display != 0)
    style_stack[style_depth].display = r->display;
  if (r->flex_dir != 0)
    style_stack[style_depth].flex_dir = r->flex_dir;
  if (r->gap_px > 0)
    style_stack[style_depth].gap_px = r->gap_px;
  if (r->justify != 0)
    style_stack[style_depth].justify = r->justify;
  if (r->align_items != 0)
    style_stack[style_depth].align_items = r->align_items;
  /* flex_grow / flex_basis describe how this element behaves as
   * an item *within* a parent flex container; the parent reads
   * them via lookup_flex_item_props(). They still land here so a
   * child container that's also a flex item can be re-queried. */
  if (r->flex_grow > 0)
    style_stack[style_depth].flex_grow = r->flex_grow;
  if (r->flex_basis > 0)
    style_stack[style_depth].flex_basis = r->flex_basis;
  if (r->grid_cols[0])
    strncpy(style_stack[style_depth].grid_cols, r->grid_cols,
            sizeof(style_stack[style_depth].grid_cols) - 1);
}

static void pop_style_state(void) {
  if (style_depth > 0) {
    style_depth--;
  }
}

static void print(const char *s) {
  _syscall(SYS_PRINT, (uint64_t)s, 0, 0, 0, 0);
}

static bool ascii_isspace(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

static char ascii_lower(char c) {
  if (c >= 'A' && c <= 'Z')
    return c + ('a' - 'A');
  return c;
}

static char scancode_to_ascii(uint8_t scancode) {
  const char table[] = {0,    27,  '1', '2',  '3', '4', '5', '6', '7', '8',
                        '9',  '0', '-', '=',  0,   0,   'q', 'w', 'e', 'r',
                        't',  'y', 'u', 'i',  'o', 'p', '[', ']', 0,   0,
                        'a',  's', 'd', 'f',  'g', 'h', 'j', 'k', 'l', ';',
                        '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n',
                        'm',  ',', '.', '/',  0,   '*', 0,   ' '};
  if (scancode >= sizeof(table))
    return 0;
  return table[scancode];
}

/* ── CSS: colour parser (hex, rgb, rgba, named) ──────────────── */

static int hex_digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + c - 'a';
  if (c >= 'A' && c <= 'F')
    return 10 + c - 'A';
  return -1;
}

static int parse_int_from(const char *s, const char **next) {
  int val = 0;
  while (*s == ' ')
    s++;
  while (*s >= '0' && *s <= '9') {
    val = val * 10 + (*s - '0');
    s++;
  }
  if (next)
    *next = s;
  return val;
}

static uint32_t parse_css_color(const char *s) {
  /* Skip leading whitespace */
  while (*s == ' ')
    s++;

  /* rgb(r, g, b) or rgba(r, g, b, a) */
  if (strncmp(s, "rgb", 3) == 0) {
    const char *p = s + 3;
    if (*p == 'a')
      p++;
    if (*p == '(')
      p++;
    const char *next;
    int r = parse_int_from(p, &next);
    p = next;
    while (*p == ' ' || *p == ',')
      p++;
    int g = parse_int_from(p, &next);
    p = next;
    while (*p == ' ' || *p == ',')
      p++;
    int b = parse_int_from(p, &next);
    if (r > 255)
      r = 255;
    if (g > 255)
      g = 255;
    if (b > 255)
      b = 255;
    uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return c ? c : 0x000001; /* avoid 0 = unset */
  }

  /* Hex color: find '#' anywhere in the string */
  const char *hash = s;
  while (*hash && *hash != '#')
    hash++;
  if (*hash == '#') {
    hash++;
    int len = 0;
    const char *p = hash;
    while (*p && hex_digit(*p) >= 0) {
      len++;
      p++;
    }

    if (len == 6) {
      uint32_t r = (hex_digit(hash[0]) << 4) | hex_digit(hash[1]);
      uint32_t g = (hex_digit(hash[2]) << 4) | hex_digit(hash[3]);
      uint32_t b = (hex_digit(hash[4]) << 4) | hex_digit(hash[5]);
      uint32_t c = (r << 16) | (g << 8) | b;
      return c ? c : 0x000001;
    }
    if (len == 3) {
      uint32_t r = hex_digit(hash[0]);
      r = (r << 4) | r;
      uint32_t g = hex_digit(hash[1]);
      g = (g << 4) | g;
      uint32_t b = hex_digit(hash[2]);
      b = (b << 4) | b;
      uint32_t c = (r << 16) | (g << 8) | b;
      return c ? c : 0x000001;
    }
  }

  /* Named CSS colours */
  struct {
    const char *name;
    uint32_t val;
  } named[] = {
      {"red", 0xE53935},           {"green", 0x43A047},
      {"blue", 0x1E88E5},          {"white", 0xFFFFFF},
      {"black", 0x000001},         {"gray", 0x808080},
      {"grey", 0x808080},          {"yellow", 0xFDD835},
      {"orange", 0xFB8C00},        {"purple", 0x8E24AA},
      {"pink", 0xD81B60},          {"cyan", 0x00BCD4},
      {"brown", 0x6D4C41},         {"navy", 0x1A237E},
      {"teal", 0x00897B},          {"coral", 0xFF7043},
      {"gold", 0xFFD600},          {"silver", 0xBDBDBD},
      {"maroon", 0x880E4F},        {"olive", 0x827717},
      {"lime", 0xC6FF00},          {"darkgray", 0x555555},
      {"darkgrey", 0x555555},      {"lightgray", 0xD3D3D3},
      {"lightgrey", 0xD3D3D3},     {"darkgreen", 0x006400},
      {"darkblue", 0x00008B},      {"darkred", 0x8B0000},
      {"darkslategray", 0x2F4F4F}, {"slategray", 0x708090},
      {"dimgray", 0x696969},       {"whitesmoke", 0xF5F5F5},
      {"ghostwhite", 0xF8F8FF},    {"ivory", 0xFFFFF0},
      {"beige", 0xF5F5DC},         {"limegreen", 0x32CD32},
      {"forestgreen", 0x228B22},   {"steelblue", 0x4682B4},
      {"royalblue", 0x4169E1},     {"tomato", 0xFF6347},
      {"salmon", 0xFA8072},        {"crimson", 0xDC143C},
      {"indigo", 0x4B0082},        {"violet", 0xEE82EE},
      {"plum", 0xDDA0DD},          {"chocolate", 0xD2691E},
      {"sienna", 0xA0522D},        {"tan", 0xD2B48C},
      {"khaki", 0xF0E68C},         {"aqua", 0x00FFFF},
      {"fuchsia", 0xFF00FF},       {"transparent", 0},
  };
  for (int i = 0; i < (int)(sizeof(named) / sizeof(named[0])); i++) {
    int nlen = strlen(named[i].name);
    if (strncmp(s, named[i].name, nlen) == 0 &&
        (s[nlen] == '\0' || s[nlen] == ' ' || s[nlen] == ';' ||
         s[nlen] == '!')) {
      return named[i].val;
    }
  }

  return 0; /* unknown */
}

/* ── CSS: skip whitespace ────────────────────────────────────── */
static const char *skip_ws(const char *p) {
  while (*p && ascii_isspace(*p))
    p++;
  return p;
}

/* ── CSS: parse a single property: value pair ────────────────── */
static void apply_css_property(css_rule_t *rule, const char *prop,
                               const char *val) {
  if (strncmp(prop, "color", 5) == 0 && prop[5] == '\0') {
    uint32_t c = parse_css_color(val);
    if (c) {
      rule->color = c;
      rule->has_color = true;
    }
  } else if (strncmp(prop, "background-color", 16) == 0 ||
             (strncmp(prop, "background", 10) == 0 && prop[10] == '\0')) {
    uint32_t c = parse_css_color(val);
    if (c) {
      rule->bg_color = c;
      rule->has_bg = true;
    }
  } else if (strncmp(prop, "text-decoration", 15) == 0) {
    if (strstr(val, "underline"))
      rule->underline = true;
  } else if (strncmp(prop, "font-weight", 11) == 0) {
    if (strstr(val, "bold") || strstr(val, "700") || strstr(val, "800") ||
        strstr(val, "900"))
      rule->bold = true;
  } else if (strncmp(prop, "text-align", 10) == 0) {
    if (strstr(val, "center")) {
      rule->align = ALIGN_CENTER;
      rule->has_align = true;
    } else if (strstr(val, "right")) {
      rule->align = ALIGN_RIGHT;
      rule->has_align = true;
    } else if (strstr(val, "left")) {
      rule->align = ALIGN_LEFT;
      rule->has_align = true;
    }
  } else if (strncmp(prop, "display", 7) == 0) {
    if (strstr(val, "none")) {
      rule->display_none = true;
    } else if (strstr(val, "grid")) {
      /* check grid before flex because "inline-grid" / "grid"
       * substrings both contain "grid"; flex check below also
       * matches "inline-flex" — both are accepted. */
      rule->display = 2;
    } else if (strstr(val, "flex")) {
      rule->display = 1;
    }
  } else if (strncmp(prop, "flex-direction", 14) == 0) {
    if (strstr(val, "column")) rule->flex_dir = 1;
    else                        rule->flex_dir = 0;
  } else if (strncmp(prop, "gap", 3) == 0 &&
             (prop[3] == '\0' || prop[3] == ':' )) {
    int px = 0; const char *v = val;
    while (*v >= '0' && *v <= '9') { px = px * 10 + (*v - '0'); v++; }
    if (px > 0) rule->gap_px = px;
  } else if (strncmp(prop, "justify-content", 15) == 0) {
    if      (strstr(val, "space-between")) rule->justify = 3;
    else if (strstr(val, "center"))        rule->justify = 2;
    else if (strstr(val, "flex-end")  ||
             strstr(val, "end"))           rule->justify = 1;
    else                                   rule->justify = 0;
  } else if (strncmp(prop, "align-items", 11) == 0) {
    if      (strstr(val, "center"))        rule->align_items = 3;
    else if (strstr(val, "flex-end")  ||
             strstr(val, "end"))           rule->align_items = 2;
    else if (strstr(val, "flex-start")||
             strstr(val, "start"))         rule->align_items = 1;
    else                                   rule->align_items = 0;
  } else if (strncmp(prop, "flex-basis", 10) == 0) {
    int px = 0; const char *v = val;
    while (*v >= '0' && *v <= '9') { px = px * 10 + (*v - '0'); v++; }
    if (px > 0) rule->flex_basis = px;
  } else if (strncmp(prop, "flex-grow", 9) == 0) {
    int g = 0; const char *v = val;
    while (*v >= '0' && *v <= '9') { g = g * 10 + (*v - '0'); v++; }
    if (g > 0) rule->flex_grow = g;
  } else if (strncmp(prop, "flex", 4) == 0 &&
             (prop[4] == '\0' || prop[4] == ':')) {
    /* CSS `flex: <grow> <shrink>? <basis>?` shorthand. We honour
     * the first number as grow, and if a second number / Xpx
     * follows it lands in basis. The common cases are `flex: 1`
     * (grow=1, basis=0) and `flex: 0 0 200px` (basis=200). */
    int g = 0; const char *v = val;
    while (*v == ' ') v++;
    while (*v >= '0' && *v <= '9') { g = g * 10 + (*v - '0'); v++; }
    if (g > 0) rule->flex_grow = g;
    while (*v == ' ' || (*v >= '0' && *v <= '9')) v++; /* skip shrink */
    int basis = 0;
    while (*v == ' ') v++;
    while (*v >= '0' && *v <= '9') { basis = basis * 10 + (*v - '0'); v++; }
    if (basis > 0) rule->flex_basis = basis;
  } else if (strncmp(prop, "grid-template-columns", 21) == 0) {
    /* Store the raw value; the grid container parses it at
     * layout time so we don't lose precision (1fr vs 200px etc). */
    int n = 0;
    while (val[n] && n < (int)sizeof(rule->grid_cols) - 1) {
      rule->grid_cols[n] = val[n]; n++;
    }
    rule->grid_cols[n] = 0;
  } else if (strncmp(prop, "padding", 7) == 0 && prop[7] == '\0') {
    /* Convert px to line-units: rough heuristic */
    int px = 0;
    const char *v = val;
    while (*v >= '0' && *v <= '9') {
      px = px * 10 + (*v - '0');
      v++;
    }
    if (px > 0) {
      rule->padding = (px + 8) / LINE_H;
      if (rule->padding > 6)
        rule->padding = 6;
      rule->has_padding = true;
    }
  } else if (strncmp(prop, "max-width", 9) == 0) {
    int px = 0;
    const char *v = val;
    while (*v >= '0' && *v <= '9') {
      px = px * 10 + (*v - '0');
      v++;
    }
    if (px > 0)
      rule->max_width = px;
  } else if (strncmp(prop, "font-size", 9) == 0) {
    int px = 0;
    const char *v = val;
    while (*v >= '0' && *v <= '9') {
      px = px * 10 + (*v - '0');
      v++;
    }
    if (px >= 24)
      rule->font_size = 2;
    else if (px >= 18)
      rule->font_size = 1;
  } else if (strncmp(prop, "text-transform", 14) == 0) {
    if (strstr(val, "uppercase"))
      rule->uppercase = true;
  } else if (strncmp(prop, "margin", 6) == 0 && prop[6] == '\0') {
    if (strstr(val, "auto")) {
      rule->align = ALIGN_CENTER;
      rule->has_align = true;
    }
    rule->margin_top = 1;
    rule->margin_bottom = 1;
  } else if (strncmp(prop, "margin-top", 10) == 0) {
    rule->margin_top = 1;
  } else if (strncmp(prop, "margin-bottom", 13) == 0) {
    rule->margin_bottom = 1;
  }
}

/* ── CSS: parse the declaration block between { and } ────────── */
static void parse_css_declarations(css_rule_t *rule, const char *decl,
                                   int dlen) {
  char prop[48], val[48];
  int pi = 0, vi = 0;
  bool in_val = false;

  for (int i = 0; i < dlen; i++) {
    char c = decl[i];
    if (c == ':') {
      in_val = true;
      vi = 0;
      continue;
    }
    if (c == ';' || i == dlen - 1) {
      if (i == dlen - 1 && c != ';') {
        if (in_val && vi < 47)
          val[vi++] = c;
        else if (!in_val && pi < 47)
          prop[pi++] = c;
      }
      prop[pi] = '\0';
      val[vi] = '\0';

      /* Trim leading spaces */
      const char *pp = prop;
      while (*pp == ' ')
        pp++;
      const char *vv = val;
      while (*vv == ' ')
        vv++;

      if (pp[0] && vv[0])
        apply_css_property(rule, pp, vv);

      pi = 0;
      vi = 0;
      in_val = false;
      continue;
    }
    if (in_val) {
      if (vi < 47)
        val[vi++] = ascii_lower(c);
    } else {
      if (pi < 47)
        prop[pi++] = ascii_lower(c);
    }
  }
}

/* ── CSS: parse a full <style> block ─────────────────────────── */
static void parse_css_block(const char *css, int css_len) {
  const char *p = css;
  const char *end = css + css_len;

  while (p < end && css_rule_count < MAX_CSS_RULES) {
    p = skip_ws(p);
    if (p >= end)
      break;

    /* Skip CSS comments */
    if (p + 1 < end && p[0] == '/' && p[1] == '*') {
      p += 2;
      while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
        p++;
      if (p + 1 < end)
        p += 2;
      continue;
    }

    /* Skip @-rules (@media, @keyframes, @font-face, @import, etc.) */
    if (*p == '@') {
      while (p < end && *p != '{' && *p != ';')
        p++;
      if (p >= end)
        break;
      if (*p == ';') {
        p++;
        continue;
      }
      /* Skip nested braces */
      int depth = 1;
      p++;
      while (p < end && depth > 0) {
        if (*p == '{')
          depth++;
        else if (*p == '}')
          depth--;
        p++;
      }
      continue;
    }

    /* Read selector (preserve spaces for compound selector splitting) */
    char sel[MAX_SELECTOR];
    int si = 0;
    bool prev_space = false;
    while (p < end && *p != '{' && si < MAX_SELECTOR - 1) {
      if (ascii_isspace(*p)) {
        if (si > 0)
          prev_space = true;
      } else {
        if (prev_space && si > 0 && si < MAX_SELECTOR - 1)
          sel[si++] = ' ';
        prev_space = false;
        sel[si++] = ascii_lower(*p);
      }
      p++;
    }
    sel[si] = '\0';
    if (p >= end || *p != '{')
      break;
    p++; /* skip { */

    /* Find closing } (handle nested braces) */
    const char *brace = p;
    int depth = 1;
    while (brace < end && depth > 0) {
      if (*brace == '{')
        depth++;
      else if (*brace == '}')
        depth--;
      if (depth > 0)
        brace++;
    }
    if (brace >= end)
      break;

    int decl_len = (int)(brace - p);

    /* Support comma-separated selectors: "h1, h2 { ... }" */
    char *tok = sel;
    while (*tok && css_rule_count < MAX_CSS_RULES) {
      while (*tok == ',' || ascii_isspace(*tok))
        tok++;
      if (!*tok)
        break;

      char single[MAX_SELECTOR];
      int qi = 0;
      while (*tok && *tok != ',' && qi < MAX_SELECTOR - 1)
        single[qi++] = *tok++;
      /* Trim trailing spaces from selector string */
      while (qi > 0 && ascii_isspace(single[qi - 1]))
        qi--;
      single[qi] = '\0';
      if (qi == 0)
        continue;

      /* For compound selectors like '.header h1', use last segment */
      char *last_space = NULL;
      for (int s = 0; single[s]; s++) {
        if (single[s] == ' ')
          last_space = &single[s];
      }
      char *final_sel = last_space ? (last_space + 1) : single;

      css_rule_t *r = &css_rules[css_rule_count];
      memset(r, 0, sizeof(css_rule_t));
      strncpy(r->selector, final_sel, MAX_SELECTOR - 1);

      parse_css_declarations(r, p, decl_len);
      css_rule_count++;
    }

    p = brace + 1; /* skip } */
  }
}

/* ── CSS: extract all <style> blocks from HTML ───────────────── */
static void extract_css(const char *html, uint32_t size) {
  css_rule_count = 0;

  for (uint32_t i = 0; i + 6 < size; i++) {
    if (html[i] != '<')
      continue;
    /* Check for <style */
    bool match = true;
    const char *kw = "style";
    for (int k = 0; kw[k]; k++) {
      if (ascii_lower(html[i + 1 + k]) != kw[k]) {
        match = false;
        break;
      }
    }
    if (!match)
      continue;
    char after = html[i + 6];
    if (after != '>' && after != ' ' && after != '\t' && after != '\n')
      continue;

    /* Skip to end of <style ...> */
    uint32_t start = i + 1;
    while (start < size && html[start] != '>')
      start++;
    if (start >= size)
      return;
    start++;

    /* Find </style> */
    uint32_t end = start;
    while (end + 8 < size) {
      if (html[end] == '<' && html[end + 1] == '/' &&
          ascii_lower(html[end + 2]) == 's' &&
          ascii_lower(html[end + 3]) == 't' &&
          ascii_lower(html[end + 4]) == 'y' &&
          ascii_lower(html[end + 5]) == 'l' &&
          ascii_lower(html[end + 6]) == 'e')
        break;
      end++;
    }

    parse_css_block(html + start, (int)(end - start));
    i = end;
  }
}

/* ── CSS: Helper to match multiple classes in class="..." ──────── */
/* Moved from below */
static void extract_attr(const char *tag, const char *attr, char *out, int max);
static bool tag_eq(const char *tag, const char *name);

static bool has_class(const char *classes, const char *cls) {
  int clen = strlen(cls);
  const char *p = classes;
  while (*p) {
    while (*p == ' ')
      p++;
    if (strncmp(p, cls, clen) == 0 && (p[clen] == ' ' || p[clen] == '\0'))
      return true;
    while (*p && *p != ' ')
      p++;
  }
  return false;
}

/* ── HTML helpers ─────────────────────────────────────────────── */

static bool tag_eq(const char *tag, const char *name) {
  int i = 0;
  while (name[i] != '\0') {
    if (ascii_lower(tag[i]) != name[i])
      return false;
    i++;
  }
  return tag[i] == '\0' || tag[i] == ' ' || tag[i] == '/' || tag[i] == '>';
}

static void extract_element_name(const char *tag, char *out, int max) {
  int i = 0;
  const char *p = tag;
  if (*p == '/')
    p++;
  while (*p && *p != ' ' && *p != '>' && *p != '/' && i < max - 1) {
    out[i++] = ascii_lower(*p++);
  }
  out[i] = '\0';
}

static void extract_attr(const char *tag, const char *attr, char *out,
                         int max) {
  out[0] = '\0';
  int alen = strlen(attr);
  const char *p = tag;
  while (*p) {
    if (strncmp(p, attr, alen) == 0 &&
        (p[alen] == '=' || p[alen] == ' ' || p[alen] == '\t')) {
      const char *start = p;
      p += alen;
      while (*p == ' ' || *p == '\t')
        p++;
      if (*p == '=') {
        p++;
        while (*p == ' ' || *p == '\t')
          p++;
        char delim = '\0';
        if (*p == '"' || *p == '\'') {
          delim = *p;
          p++;
        }
        int i = 0;
        while (*p && i < max - 1) {
          if (delim) {
            if (*p == delim)
              break;
          } else {
            if (*p == ' ' || *p == '\t' || *p == '>')
              break;
          }
          out[i++] = *p++;
        }
        out[i] = '\0';
        return;
      }
    }
    p++;
  }
}

/* ── Apply CSS overrides for the current element ─────────────── */
static void apply_css_with_attrs(const char *elem, const char *cls,
                                 const char *id, const char *inline_style) {
  /* 1. Stylesheet rules (element, .class, #id, element.class selectors) */
  for (int i = 0; i < css_rule_count; i++) {
    const css_rule_t *r = &css_rules[i];
    bool match = false;

    if (r->selector[0] == '.') {
      if (cls[0] && has_class(cls, r->selector + 1))
        match = true;
    } else if (r->selector[0] == '#') {
      if (id[0] && strcmp(r->selector + 1, id) == 0)
        match = true;
    } else {
      /* Element or Element.class or Element#id */
      char sel_elem[MAX_SELECTOR];
      int dot_idx = -1;
      int hash_idx = -1;
      for (int k = 0; r->selector[k]; k++) {
        if (r->selector[k] == '.') {
          dot_idx = k;
          break;
        }
        if (r->selector[k] == '#') {
          hash_idx = k;
          break;
        }
      }

      if (dot_idx != -1) {
        strncpy(sel_elem, r->selector, dot_idx);
        sel_elem[dot_idx] = '\0';
        if (strcmp(sel_elem, elem) == 0 &&
            has_class(cls, r->selector + dot_idx + 1))
          match = true;
      } else if (hash_idx != -1) {
        strncpy(sel_elem, r->selector, hash_idx);
        sel_elem[hash_idx] = '\0';
        if (strcmp(sel_elem, elem) == 0 &&
            strcmp(id, r->selector + hash_idx + 1) == 0)
          match = true;
      } else {
        if (elem[0] && strcmp(r->selector, elem) == 0)
          match = true;
      }
    }

    if (match) {
      apply_css_to_current_state(r);
      if (r->margin_top > 0)
        blank_line();
    }
  }

  /* 2. Inline style attribute */
  if (inline_style && inline_style[0]) {
    parse_css_declarations_to_state(inline_style);
  }
}

/* Legacy entry: takes a raw tag string (e.g. `a class="x" href="y"`)
 * and parses its attributes out of the string. Used by the legacy
 * parse_html() pipeline that doesn't have a DOM at hand. */
static void apply_css_for_element(const char *tag) {
  char elem[MAX_SELECTOR];
  char cls[MAX_CSS_CLASS];
  char id[MAX_CSS_CLASS];
  char inline_style[256];

  extract_element_name(tag, elem, MAX_SELECTOR);
  extract_attr(tag, "class", cls, MAX_CSS_CLASS);
  extract_attr(tag, "id", id, MAX_CSS_CLASS);
  extract_attr(tag, "style", inline_style, sizeof(inline_style));

  apply_css_with_attrs(elem, cls, id, inline_style);
}

/* R6/L4: DOM-walk entry. Reads class/id/style directly off the
 * DOM node (the previous code path was passing only the tag name
 * to apply_css_for_element, so class/id selectors silently
 * missed for every element walked through the DOM tree — that
 * was effectively breaking *all* class-based CSS during the
 * w_emit_node pass). */
static void apply_css_for_node(dom_node_t *n) {
  if (!n || !n->tag_name) return;
  const char *cls = dom_get_attr(n, "class");
  const char *id  = dom_get_attr(n, "id");
  const char *st  = dom_get_attr(n, "style");
  apply_css_with_attrs(n->tag_name, cls ? cls : "", id ? id : "",
                       st ? st : "");
}

static void parse_css_declarations_to_state(const char *decl) {
  css_rule_t tmp;
  memset(&tmp, 0, sizeof(tmp));
  parse_css_declarations(&tmp, decl, (int)strlen(decl));
  apply_css_to_current_state(&tmp);
}

static void resolve_url(const char *base, const char *rel, char *out) {
  if (strstr(rel, "://")) {
    strcpy(out, rel);
    return;
  }
  if (rel[0] == '/') {
    /* Absolute path on same host */
    const char *p = strstr(base, "://");
    if (p) {
      p += 3;
      while (*p && *p != '/')
        p++;
      int len = (int)(p - base);
      strncpy(out, base, len);
      strcpy(out + len, rel);
    } else {
      strcpy(out, rel);
    }
    return;
  }
  /* Relative path */
  strcpy(out, base);
  char *last_slash = strrchr(out, '/');
  if (last_slash) {
    if (last_slash < strstr(out, "://") + 3) {
      strcpy(last_slash + 1, rel);
    } else {
      strcpy(last_slash + 1, rel);
    }
  } else {
    strcpy(out, rel);
  }
}

static char tag_context[256]; /* Current tag for link extraction */

/* ── HTML Entity Decoder ──────────────────────────────────────── */
static int decode_entity(const char *src, char *out) {
  /* Returns number of chars consumed from src (including &..;) */
  /* Writes decoded char to *out, returns 0 on failure */
  if (src[0] != '&')
    return 0;
  const char *semi = src + 1;
  while (*semi && *semi != ';' && (semi - src) < 12)
    semi++;
  if (*semi != ';')
    return 0;
  int elen = (int)(semi - src - 1);
  const char *name = src + 1;

  /* Numeric entities */
  if (name[0] == '#') {
    int val = 0;
    if (name[1] == 'x' || name[1] == 'X') {
      for (int i = 2; i < elen; i++) {
        int d = hex_digit(name[i]);
        if (d < 0)
          break;
        val = val * 16 + d;
      }
    } else {
      for (int i = 1; i < elen; i++) {
        if (name[i] >= '0' && name[i] <= '9')
          val = val * 10 + (name[i] - '0');
      }
    }
    if (val > 0 && val < 128) {
      *out = (char)val;
      return (int)(semi - src + 1);
    }
    if (val >= 128) {
      *out = '?';
      return (int)(semi - src + 1);
    } /* non-ASCII placeholder */
    return 0;
  }

  /* Named entities */
  struct {
    const char *n;
    char c;
  } ents[] = {
      {"amp", '&'},    {"lt", '<'},     {"gt", '>'},    {"quot", '"'},
      {"apos", '\''},  {"nbsp", ' '},   {"ndash", '-'}, {"mdash", '-'},
      {"lsquo", '\''}, {"rsquo", '\''}, {"ldquo", '"'}, {"rdquo", '"'},
      {"bull", '*'},   {"hellip", '.'}, {"copy", 'c'},  {"reg", 'r'},
      {"trade", ' '},  {"laquo", '<'},  {"raquo", '>'}, {"times", 'x'},
      {"divide", '/'}, {"cent", 'c'},   {"pound", '#'}, {"euro", 'E'},
      {"yen", 'Y'},    {"deg", 'o'},
  };
  for (int i = 0; i < (int)(sizeof(ents) / sizeof(ents[0])); i++) {
    if ((int)strlen(ents[i].n) == elen && strncmp(name, ents[i].n, elen) == 0) {
      *out = ents[i].c;
      return (int)(semi - src + 1);
    }
  }
  return 0; /* unknown entity, leave as-is */
}

static void push_line(const char *text, int len, line_style_t style,
                      bool indent) {
  if (line_count >= MAX_LINES)
    return;

  if (len < 0)
    len = 0;

  /* R6/B2c: every text line passes through the UTF-8 normalise pass
   * which keeps Cyrillic as raw UTF-8 (so the eid renderer can hit
   * the bundled Unifont glyph table) and substitutes other non-ASCII
   * codepoints to plain ASCII (— → "--", … → "...", →/↑ → ASCII). We
   * then truncate at LINE_CHARS *visible columns*, never mid-byte
   * inside a UTF-8 sequence. */
  char norm_buf[LINE_BYTES];
  int  norm_len = utf8_to_ascii(text, len, norm_buf, sizeof(norm_buf));
  int  out_w = 0, cols = 0;
  for (int i = 0; i < norm_len && cols < LINE_CHARS; ) {
    unsigned char b = (unsigned char)norm_buf[i];
    int adv = (b < 0x80) ? 1
            : ((b & 0xE0) == 0xC0) ? 2
            : ((b & 0xF0) == 0xE0) ? 3
            : ((b & 0xF8) == 0xF0) ? 4 : 1;
    if (i + adv > norm_len) break;
    if (out_w + adv >= (int)sizeof(lines[line_count].text)) break;
    for (int k = 0; k < adv; k++) lines[line_count].text[out_w++] = norm_buf[i + k];
    i += adv; cols++;
  }
  lines[line_count].text[out_w] = '\0';
  len = out_w;

  /* Apply text-transform: uppercase */
  if (style_stack[style_depth].uppercase) {
    for (int i = 0; lines[line_count].text[i]; i++) {
      char c = lines[line_count].text[i];
      if (c >= 'a' && c <= 'z')
        lines[line_count].text[i] = c - ('a' - 'A');
    }
  }

  lines[line_count].style = style;
  lines[line_count].indent = indent;
  lines[line_count].css_color = style_stack[style_depth].color;
  lines[line_count].css_bg = style_stack[style_depth].bg;
  lines[line_count].css_underline = style_stack[style_depth].underline;
  lines[line_count].css_bold = style_stack[style_depth].bold;
  lines[line_count].css_align = style_stack[style_depth].align;
  lines[line_count].full_width_bg = style_stack[style_depth].full_width_bg;
  lines[line_count].padding = style_stack[style_depth].padding;
  lines[line_count].font_size = style_stack[style_depth].font_size;
  lines[line_count].uppercase = style_stack[style_depth].uppercase;

  if (style == STYLE_LINK) {
    extract_attr(tag_context, "href", lines[line_count].link_url, 127);
  } else {
    lines[line_count].link_url[0] = '\0';
  }
  /* Default for everything that isn't an <img> placeholder; the
   * <img> handler overwrites this immediately after we return. */
  lines[line_count].image_idx = -1;

  /* R6/L1+L2: pixel-positioned box, anchored against the current
   * layout frame. The root frame covers (0, 0, CONTENT_W) so this
   * reproduces the legacy vertical-stream geometry bit-for-bit;
   * a nested frame (L3+) would shift these into a sub-rectangle. */
  layout_frame_t *f = layout_top();
  int bx = indent ? LAYOUT_DEFAULT_INDENT : 0;
  if (bx > f->w) bx = f->w; /* defensive: never go negative-width */
  lines[line_count].box_x = f->x + bx;
  lines[line_count].box_y = f->y;
  lines[line_count].box_w = f->w - bx;
  lines[line_count].box_h = LINE_H;
  f->y += LINE_H;

  line_count++;
}

/* R6/L1+L2: post-adjust the last-pushed line's vertical advance.
 * `extra` is added on top of the LINE_H already consumed by
 * push_line(). Used by widget/image emitters to model their
 * heavier visual height (button + padding, image + card, …).
 * Advances the *current* frame's cursor, not the global one. */
static void layout_extend_last(int extra) {
  if (line_count == 0 || extra <= 0) return;
  lines[line_count - 1].box_h += extra;
  layout_top()->y += extra;
}

/* R6/L1+L2: replace the last-pushed line's vertical advance with
 * an absolute pixel height. Used by the <img> emitter once the
 * image dimensions are known. */
static void layout_set_last_height(int h) {
  if (line_count == 0 || h <= 0) return;
  int delta = h - lines[line_count - 1].box_h;
  lines[line_count - 1].box_h = h;
  layout_top()->y += delta;
}

static void blank_line(void) {
  if (line_count == 0)
    return;
  if (line_count > 0 && lines[line_count - 1].text[0] == '\0')
    return;
  push_line("", 0, STYLE_NORMAL, false);
}

/* R6/B2c: width math is in *cells*, not bytes. `*len` and `word_len`
 * are byte indices; we use utf8_cells_n to convert. Buffer overflow
 * is guarded against LINE_BYTES. */
static void append_word(char *line, int *len, const char *word, int word_len,
                        line_style_t style, bool indent) {
  int max_cells = indent ? (LINE_CHARS - 4) : LINE_CHARS;
  if (word_len <= 0) return;

  int cur_cells  = utf8_cells_n(line, *len);
  int word_cells = utf8_cells_n(word, word_len);

  if (cur_cells > 0 && cur_cells + 1 + word_cells > max_cells) {
    push_line(line, *len, style, indent);
    *len = 0;
    cur_cells = 0;
  }

  if (cur_cells > 0 && *len + 1 < LINE_BYTES) {
    line[(*len)++] = ' ';
    cur_cells++;
  }

  /* Word longer than the whole line: split at cell boundaries. */
  while (word_cells > max_cells) {
    /* fill what's left of the line in cells */
    int room_cells = max_cells - cur_cells;
    int i = 0, took = 0;
    while (i < word_len && took < room_cells && *len + 4 < LINE_BYTES) {
      unsigned char b = (unsigned char)word[i];
      int adv = (b < 0x80) ? 1 : ((b & 0xE0) == 0xC0) ? 2
              : ((b & 0xF0) == 0xE0) ? 3 : ((b & 0xF8) == 0xF0) ? 4 : 1;
      if (i + adv > word_len) break;
      for (int k = 0; k < adv; k++) line[(*len)++] = word[i + k];
      i += adv; took++;
    }
    word += i; word_len -= i; word_cells -= took;
    push_line(line, *len, style, indent);
    *len = 0; cur_cells = 0;
  }

  /* Tail copy: append the rest of the word, byte by byte but
   * respecting cell budget and buffer headroom. */
  int i = 0;
  while (i < word_len && cur_cells < max_cells && *len + 4 < LINE_BYTES) {
    unsigned char b = (unsigned char)word[i];
    int adv = (b < 0x80) ? 1 : ((b & 0xE0) == 0xC0) ? 2
            : ((b & 0xF0) == 0xE0) ? 3 : ((b & 0xF8) == 0xF0) ? 4 : 1;
    if (i + adv > word_len) break;
    for (int k = 0; k < adv; k++) line[(*len)++] = word[i + k];
    i += adv; cur_cells++;
  }
}

static void flush_current(char *line, int *len, line_style_t style,
                          bool indent) {
  if (*len > 0) {
    push_line(line, *len, style, indent);
    *len = 0;
  }
}

static void read_tag(const char *html, uint32_t size, uint32_t *pos, char *tag,
                     int tag_size) {
  int len = 0;
  (*pos)++;

  while (*pos < size && html[*pos] != '>' && len < tag_size - 1) {
    tag[len++] = ascii_lower(html[*pos]);
    (*pos)++;
  }
  while (*pos < size && html[*pos] != '>')
    (*pos)++;
  if (*pos < size && html[*pos] == '>')
    (*pos)++;

  tag[len] = '\0';
}

/* Status bar is 18 px; content starts at CONTENT_Y + 14 (page title bar
 * offset matches what render() uses). Leave LINE_H slack so the
 * bottom-most line isn't clipped against the status bar border. */
static int visible_lines(void) {
  return (WIN_H - 18 - (CONTENT_Y + 14) - LINE_H) / LINE_H;
}

/* ── Apply CSS overrides for the current element ─────────────── */

static void copy_title_from_html(const char *html, uint32_t size) {
  for (uint32_t i = 0; i + 7 < size; i++) {
    if (html[i] == '<' && tag_eq(html + i + 1, "title")) {
      uint32_t start = i + 1;
      while (start < size && html[start] != '>')
        start++;
      if (start >= size)
        return;
      start++;

      uint32_t end = start;
      while (end + 8 < size) {
        if (html[end] == '<' && html[end + 1] == '/' &&
            tag_eq(html + end + 2, "title"))
          break;
        end++;
      }

      /* R6/B2: titles often contain U+2014 em-dashes etc. Collapse
       * whitespace inline as before, but route the byte stream through
       * the same UTF-8 → ASCII pass everything else uses. */
      char raw[256];
      int rlen = 0;
      for (uint32_t j = start; j < end && rlen < (int)sizeof(raw) - 1; j++) {
        char c = html[j];
        if (ascii_isspace(c)) c = ' ';
        if (c == ' ' && rlen > 0 && raw[rlen - 1] == ' ') continue;
        raw[rlen++] = c;
      }
      raw[rlen] = '\0';
      utf8_to_ascii(raw, rlen, page_title, sizeof(page_title));
      return;
    }
  }
}

/* =====================================================================
 * DOM-tree-driven parser (phase J3 step 2).
 *
 * Replaces the byte-stream state machine below with: build a dom_node_t
 * tree, then walk it emitting lines through the existing push_line /
 * append_word / flush_current / blank_line / style-stack helpers. The
 * line-emission layer is therefore unchanged — only the input layer
 * (HTML bytes → events) is swapped.
 *
 * Why: J4 needs DOM bindings in QuickJS. The tree becomes the single
 * source of truth that both the renderer and the JS DOM bindings
 * share.
 *
 * Safety net: the legacy parser is kept under parse_html_legacy() and
 * is reachable via the --legacy CLI flag in case of regression.
 * ===================================================================== */

#include "dom.h"

#ifdef BROWSER_BUILD
/* Phase J6a: pull QuickJS + ca-bundle in early so parse_html (defined
 * well above the load_page() switch) can hand the DOM off to scripts.
 * The original includes lower in the file (inside the load_page
 * branch) are kept for clarity. */
#include "qjs_page.h"
#include "qjs_window.h"
#include "../third_party/ca_bundle/ca_bundle.h"
#endif

typedef struct {
  /* R6/B2c: buffers hold UTF-8 bytes, sized to fit a full LINE_CHARS
   * row of Cyrillic-or-wider codepoints. The width caps below
   * (LINE_CHARS) still mean *visible cells*, not bytes. */
  char         current[LINE_BYTES];
  int          current_len;       /* bytes used in current */
  char         word[LINE_BYTES];
  int          word_len;          /* bytes used in word */
  line_style_t style;
  bool         in_list;
  bool         in_pre;
  bool         at_li_start;
} walk_ctx_t;

/* Save/restore the renderer-visible style for tags that override it.
 * The CSS style stack already handles colour/bold/etc; this is only
 * about the per-line `line_style_t` enum (H1/H2/CODE/LINK/BULLET/...). */
typedef struct {
  line_style_t style;
  bool         in_pre;
} style_save_t;

static void w_flush(walk_ctx_t *w) {
  if (w->word_len > 0) {
    append_word(w->current, &w->current_len, w->word, w->word_len,
                w->style, w->in_list);
    w->word_len = 0;
  }
  flush_current(w->current, &w->current_len, w->style, w->in_list);
}

static void w_emit_text(walk_ctx_t *w, const char *text) {
  if (!text) return;

  /* R6/B2: walk UTF-8 by codepoint, not byte. The word/line buffers
   * count ASCII chars (matching the renderer's 1 char = 8 px font),
   * so non-ASCII codepoints have to be substituted *before* we hit
   * the width math in append_word() — otherwise a 2-byte cyrillic
   * char would be split mid-sequence the moment the byte-counted
   * word fills up. */
  const unsigned char *p = (const unsigned char *)text;
  int rem = 0; while (text[rem]) rem++;
  while (rem > 0) {
    uint32_t cp = 0;
    int n = utf8_decode_one(p, rem, &cp);
    if (n <= 0) break;
    p += n; rem -= n;

    if (w->at_li_start) {
      append_word(w->current, &w->current_len, "*", 1, STYLE_BULLET, true);
      w->at_li_start = false;
    }

    if (cp < 0x80) {
      char c = (char)cp;
      if (w->in_pre && (c == '\n' || c == '\r')) {
        if (w->word_len > 0) {
          append_word(w->current, &w->current_len, w->word, w->word_len,
                      w->style, w->in_list);
          w->word_len = 0;
        }
        flush_current(w->current, &w->current_len, w->style, w->in_list);
        continue;
      }
      if (c == '&') {
        char decoded;
        int consumed = decode_entity((const char *)(p - 1), &decoded);
        if (consumed > 0) {
          if (w->word_len < LINE_CHARS) w->word[w->word_len++] = decoded;
          /* skip the entity body. We already consumed '&'; advance by
           * (consumed-1) more bytes. */
          int extra = consumed - 1;
          if (extra > rem) extra = rem;
          p += extra; rem -= extra;
          continue;
        }
      }
      if (ascii_isspace(c)) {
        if (w->word_len > 0) {
          append_word(w->current, &w->current_len, w->word, w->word_len,
                      w->style, w->in_list);
          w->word_len = 0;
        }
      } else if (w->word_len < LINE_CHARS) {
        w->word[w->word_len++] = c;
      }
      continue;
    }

    /* R6/B2c: Cyrillic Basic (U+0400..U+04FF) — keep raw UTF-8 in
     * the word buffer so the renderer can render it directly through
     * eid_draw_text's Unifont lookup. Word/line buffers still count
     * visible cells, but cell-width math (LINE_CHARS) below uses
     * codepoint count not byte count; we just need to make sure we
     * don't split a UTF-8 sequence across the buffer boundary. */
    if (cp >= 0x0400 && cp <= 0x04FF) {
      if (w->word_len + 2 < LINE_CHARS) {
        w->word[w->word_len++] = (char)(0xC0 | (cp >> 6));
        w->word[w->word_len++] = (char)(0x80 | (cp & 0x3F));
      }
      continue;
    }

    /* Non-ASCII outside Cyrillic Basic: substitute (may be NULL →
     * drop). All fallback strings are non-whitespace so we don't
     * need to flush a word boundary. */
    const char *sub = cp_to_ascii_fallback(cp);
    if (!sub) sub = cp_to_translit(cp);
    if (!sub) continue;
    while (*sub && w->word_len < LINE_CHARS) w->word[w->word_len++] = *sub++;
  }
}

/* Synthesize an `<a href="...">`-style string into tag_context so
 * push_line can pick it up via extract_attr() when emitting STYLE_LINK
 * lines. Matches the original parser's contract. */
static void w_set_link_context(const dom_node_t *n) {
  const char *href = dom_get_attr(n, "href");
  if (!href) { tag_context[0] = 0; return; }
  /* Format: a href="VALUE" — exactly what extract_attr expects. */
  int o = 0;
  const char *prefix = "a href=\"";
  while (prefix[o]) { tag_context[o] = prefix[o]; o++; }
  for (const char *s = href; *s && o < (int)sizeof(tag_context) - 2; s++)
    tag_context[o++] = *s;
  tag_context[o++] = '"';
  tag_context[o] = 0;
}

/* Forward decl: emit_flex_container / emit_grid_container call
 * w_emit_node via render_subtree, but they themselves are called
 * from w_emit_node, so we need a declaration up front. */
static void w_emit_node(walk_ctx_t *w, dom_node_t *n);
/* L3 helpers live further down the file (just above
 * rebuild_lines_from_dom); flex / grid containers call into
 * them. */
static void render_subtree(walk_ctx_t *w, dom_node_t *node,
                           int frame_x, int frame_y, int frame_w,
                           int *out_first, int *out_count, int *out_h);
static void layout_translate_range(int first, int count, int dx, int dy);

/* ─────────────────────────────────────────────────────────────────
 * R6/L4: flex container layout.
 *
 * Called by w_emit_node when the current element's CSS resolves
 * display:flex. Iterates the element's children, decides widths
 * for flex-direction:row (each child gets a column-shaped sub-
 * frame), and drops back to vertical stacking for
 * flex-direction:column. In all cases `gap` and `align-items`
 * are honoured.
 *
 * Width distribution (row):
 *   - sum_basis = Σ child.flex_basis (children whose basis > 0
 *                                     consume their basis verbatim)
 *   - remaining = container_w − sum_basis − (n−1)*gap
 *   - sum_grow  = Σ child.flex_grow  (default 0; "flex:1" sets to 1)
 *   - any child with flex_grow > 0 gets:
 *         child_w = basis + remaining * grow / sum_grow
 *   - children with neither basis nor grow get an equal share of
 *     whatever's left (i.e. they behave like flex:1).
 *
 * Height computation (row):
 *   - render_subtree each child into its column.
 *   - row height = max(child heights).
 *   - apply align-items: stretch (default) leaves children as is;
 *     start/end/center re-translate each child vertically within
 *     the row.
 *
 * Column direction:
 *   - children are stacked normally with `gap` separation; no
 *     width splitting (each child gets the full container width).
 *
 * Justify-content (row):
 *   - flex-start (default): no horizontal redistribution.
 *   - center: shift each child by leftover/2.
 *   - flex-end: shift each child by leftover.
 *   - space-between: split leftover between gaps.
 *
 * The function returns after handling the children; the caller
 * skips the normal child-recursion loop. The container's own
 * outer style stack frame (push_style_state above) is left
 * untouched and will be popped after the postlude.
 * ────────────────────────────────────────────────────────────── */

/* Helper: read an element's flex-item props (basis, grow) without
 * touching the global style stack. Used by emit_flex_container
 * to decide each child's width. */
typedef struct {
  int basis;
  int grow;
  bool has_grow;
  bool has_basis;
} flex_item_props_t;

static void lookup_flex_item_props(dom_node_t *n, flex_item_props_t *out) {
  out->basis = 0; out->grow = 0;
  out->has_basis = false; out->has_grow = false;
  if (!n || !n->tag_name) return;
  const char *cls = dom_get_attr(n, "class");
  const char *id  = dom_get_attr(n, "id");
  const char *st  = dom_get_attr(n, "style");

  /* Walk the global stylesheet rules and accumulate any flex-grow
   * / flex-basis that matches this element. Mirrors apply_css's
   * matching but doesn't touch style_stack. */
  for (int i = 0; i < css_rule_count; i++) {
    const css_rule_t *r = &css_rules[i];
    bool match = false;
    if (r->selector[0] == '.') {
      if (cls && has_class(cls, r->selector + 1)) match = true;
    } else if (r->selector[0] == '#') {
      if (id && strcmp(r->selector + 1, id) == 0) match = true;
    } else {
      char sel_elem[MAX_SELECTOR];
      int dot_idx = -1, hash_idx = -1;
      for (int k = 0; r->selector[k]; k++) {
        if (r->selector[k] == '.') { dot_idx = k; break; }
        if (r->selector[k] == '#') { hash_idx = k; break; }
      }
      if (dot_idx != -1) {
        strncpy(sel_elem, r->selector, dot_idx);
        sel_elem[dot_idx] = 0;
        if (strcmp(sel_elem, n->tag_name) == 0 &&
            cls && has_class(cls, r->selector + dot_idx + 1)) match = true;
      } else if (hash_idx != -1) {
        strncpy(sel_elem, r->selector, hash_idx);
        sel_elem[hash_idx] = 0;
        if (strcmp(sel_elem, n->tag_name) == 0 &&
            id && strcmp(r->selector + hash_idx + 1, id) == 0) match = true;
      } else {
        if (strcmp(r->selector, n->tag_name) == 0) match = true;
      }
    }
    if (match) {
      if (r->flex_grow  > 0) { out->grow  = r->flex_grow;  out->has_grow  = true; }
      if (r->flex_basis > 0) { out->basis = r->flex_basis; out->has_basis = true; }
    }
  }
  /* Inline style overrides stylesheet matches. */
  if (st && st[0]) {
    css_rule_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    parse_css_declarations(&tmp, st, (int)strlen(st));
    if (tmp.flex_grow  > 0) { out->grow  = tmp.flex_grow;  out->has_grow  = true; }
    if (tmp.flex_basis > 0) { out->basis = tmp.flex_basis; out->has_basis = true; }
  }
}

/* Count renderable element children (skip text-only / comment
 * nodes; they get absorbed into whichever flex item contains
 * them by virtue of being inside that item's subtree). */
static int count_flex_children(dom_node_t *parent) {
  int n = 0;
  for (dom_node_t *c = parent->first_child; c; c = c->next_sibling) {
    if (c->type == DOM_NODE_ELEMENT) n++;
  }
  return n;
}

static void emit_flex_container(walk_ctx_t *w, dom_node_t *n) {
  layout_frame_t *parent = layout_top();
  int container_x = parent->x;
  int container_y = parent->y;
  int container_w = parent->w;
  int flex_dir    = style_stack[style_depth].flex_dir;
  int gap         = style_stack[style_depth].gap_px;
  int justify     = style_stack[style_depth].justify;
  int align       = style_stack[style_depth].align_items;

  /* Flush any pending inline text from the parent first; we
   * don't want the container's children to inherit a half-built
   * word run from before. */
  w_flush(w);

  /* Column direction → no horizontal splitting. Just walk the
   * children normally but insert `gap` between them. Cheap and
   * sufficient because the existing block stream already stacks
   * vertically. The only thing we add is the gap. */
  if (flex_dir == 1) {
    int first = 1;
    for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
      if (c->type != DOM_NODE_ELEMENT) {
        /* keep text/comment in-line behaviour for column flex */
        w_emit_node(w, c);
        continue;
      }
      if (!first && gap > 0) {
        layout_top()->y += gap;
      }
      w_emit_node(w, c);
      first = 0;
    }
    w_flush(w);
    return;
  }

  /* ---- flex-direction: row ---- */
  int nchildren = count_flex_children(n);
  if (nchildren <= 0) {
    /* No element children → fall back to normal recursion so
     * orphan text still gets rendered. */
    for (dom_node_t *c = n->first_child; c; c = c->next_sibling)
      w_emit_node(w, c);
    w_flush(w);
    return;
  }

  /* Single child row is the same as normal recursion. */
  if (nchildren == 1) {
    for (dom_node_t *c = n->first_child; c; c = c->next_sibling)
      w_emit_node(w, c);
    w_flush(w);
    return;
  }

  /* If the container is too narrow to give every child at least
   * MIN_COL_W pixels, degrade to vertical stacking. This is
   * critical at our 604-px content width: 6+ columns just turn
   * into chopped text. */
  const int MIN_COL_W = 80;
  int gross_w = container_w - (nchildren - 1) * gap;
  if (gross_w / nchildren < MIN_COL_W) {
    int first = 1;
    for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
      if (c->type != DOM_NODE_ELEMENT) { w_emit_node(w, c); continue; }
      if (!first && gap > 0) layout_top()->y += gap;
      w_emit_node(w, c);
      first = 0;
    }
    w_flush(w);
    return;
  }

  /* Compute each child's column width. */
  int widths[16] = {0};
  int starts[16] = {0};
  int first_idx[16];
  int counts[16];
  int heights[16] = {0};

  if (nchildren > 16) nchildren = 16; /* clamp; rare in practice */

  /* Pass 1: gather props. */
  int sum_basis = 0;
  int sum_grow  = 0;
  int n_auto    = 0;
  flex_item_props_t props[16] = {{0}};
  {
    int idx = 0;
    for (dom_node_t *c = n->first_child; c && idx < nchildren;
         c = c->next_sibling) {
      if (c->type != DOM_NODE_ELEMENT) continue;
      lookup_flex_item_props(c, &props[idx]);
      if (props[idx].has_basis) sum_basis += props[idx].basis;
      if (props[idx].has_grow ) sum_grow  += props[idx].grow;
      if (!props[idx].has_basis && !props[idx].has_grow) n_auto++;
      idx++;
    }
  }

  int remaining = container_w - sum_basis - (nchildren - 1) * gap;
  if (remaining < 0) remaining = 0;
  /* Pool to distribute to grow-children. If no child has grow but
   * some are "auto" (neither grow nor basis), treat each auto as
   * flex:1 — this is the dominant case (3-column "div / div / div"
   * without explicit flex on any item). */
  int eff_sum_grow = sum_grow;
  if (eff_sum_grow == 0 && n_auto > 0) eff_sum_grow = n_auto;

  /* Pass 2: assign widths. */
  for (int i = 0; i < nchildren; i++) {
    int wpx = props[i].has_basis ? props[i].basis : 0;
    if (props[i].has_grow) {
      wpx += (remaining * props[i].grow) / (eff_sum_grow > 0 ? eff_sum_grow : 1);
    } else if (!props[i].has_basis && n_auto > 0) {
      wpx += (remaining * 1) / (eff_sum_grow > 0 ? eff_sum_grow : 1);
    }
    if (wpx < MIN_COL_W) wpx = MIN_COL_W;
    widths[i] = wpx;
  }

  /* Justify-content: compute extra horizontal offset per column. */
  int total_used = (nchildren - 1) * gap;
  for (int i = 0; i < nchildren; i++) total_used += widths[i];
  int leftover = container_w - total_used;
  if (leftover < 0) leftover = 0;

  int extra_gap = gap;
  int x_cursor  = container_x;
  if (justify == 2 /*center*/) {
    x_cursor += leftover / 2;
  } else if (justify == 1 /*flex-end*/) {
    x_cursor += leftover;
  } else if (justify == 3 /*space-between*/ && nchildren > 1) {
    extra_gap = gap + (leftover / (nchildren - 1));
  }
  for (int i = 0; i < nchildren; i++) {
    starts[i] = x_cursor;
    x_cursor += widths[i] + extra_gap;
  }

  /* Pass 3: render each child into its column. We anchor each
   * column at (starts[i], container_y, widths[i]); render_subtree
   * pushes that as a private frame so emitted lines get correct
   * box_x/box_y. */
  int max_h = 0;
  {
    int idx = 0;
    for (dom_node_t *c = n->first_child; c && idx < nchildren;
         c = c->next_sibling) {
      if (c->type != DOM_NODE_ELEMENT) continue;
      int first = -1, count = 0, h = 0;
      render_subtree(w, c, starts[idx], container_y, widths[idx],
                     &first, &count, &h);
      first_idx[idx] = first;
      counts[idx]    = count;
      heights[idx]   = h;
      if (h > max_h) max_h = h;
      idx++;
    }
  }

  /* Pass 4: align-items vertical fixup. */
  if (align != 0) {
    for (int i = 0; i < nchildren; i++) {
      if (counts[i] <= 0) continue;
      int dy = 0;
      if      (align == 3 /*center*/) dy = (max_h - heights[i]) / 2;
      else if (align == 2 /*end*/)    dy = (max_h - heights[i]);
      /* align == 1 (start): dy = 0 */
      if (dy > 0) layout_translate_range(first_idx[i], counts[i], 0, dy);
    }
  }

  /* Advance the parent frame's cursor by the row's max height. */
  parent->y = container_y + max_h;
  w_flush(w);
}

/* ─────────────────────────────────────────────────────────────────
 * R6/L5: display:grid container layout.
 *
 * Supports the grid-template-columns vocabulary the real
 * EquinoxOS landing page (equinoxos.duckdns.org) uses:
 *
 *   grid-template-columns: 1fr 1fr
 *   grid-template-columns: 1.05fr .95fr
 *   grid-template-columns: 200px 1fr
 *   grid-template-columns: repeat(3, 1fr)
 *   grid-template-columns: repeat(2, 1fr)
 *   grid-template-columns: repeat(auto-fit, minmax(160px, 1fr))
 *
 * Flow is row-major: child i lands in column (i % n_cols), row
 * (i / n_cols). Row height is max of its cells' heights. After a
 * row finishes, y advances by row_h + gap (per-row gap; we don't
 * yet split row-gap vs column-gap).
 *
 * Track distribution:
 *   - parse the template into a list of tracks; each track is
 *     either a fixed px (rule_kind=PX) or a fractional fr
 *     (rule_kind=FR, with a "fr count" — 1, 1.05, .95 etc. scaled
 *     by 100 to stay integer).
 *   - For auto-fit / auto-fill minmax(min,1fr) we compute
 *     n_cols = max(1, container_w / min_px) and synthesise N
 *     equal 1fr tracks.
 *   - widths: sum_px = Σ PX tracks; remaining = container_w
 *     − sum_px − (n_cols−1) * col_gap; sum_fr = Σ fr counts;
 *     each FR track gets remaining * fr_count / sum_fr. Final
 *     widths clamped to at least MIN_COL_W = 50px so single-char
 *     columns aren't created at narrow widths.
 *
 * Like flex, grid degrades to vertical stacking when the
 * container is too narrow for the requested number of columns.
 * ────────────────────────────────────────────────────────────── */

typedef struct { int kind; int px; int fr_x100; } grid_track_t;

/* Parse a single track token (e.g. "1fr", "1.05fr", "200px",
 * "auto", "minmax(160px,1fr)"). For minmax we keep the *fr*
 * side as the track but remember the min-px in `min_px_out` —
 * the caller uses that for auto-fit column-count math. */
static bool grid_parse_track(const char *p, const char *end,
                             grid_track_t *out, int *min_px_out) {
  while (p < end && (*p == ' ' || *p == ',')) p++;
  if (p >= end) return false;

  /* minmax(...) — read the second argument and keep the inner
   * track; the first arg (min) feeds auto-fit column count. */
  if (strncmp(p, "minmax(", 7) == 0) {
    const char *open = p + 7;
    const char *comma = strchr(open, ',');
    const char *close = strchr(open, ')');
    if (!comma || !close || comma >= close) return false;
    /* min part */
    int mn = 0; const char *q = open;
    while (q < comma && *q == ' ') q++;
    while (q < comma && *q >= '0' && *q <= '9') { mn = mn*10 + (*q-'0'); q++; }
    if (min_px_out) *min_px_out = mn > 0 ? mn : 0;
    /* recurse on the second part */
    return grid_parse_track(comma + 1, close, out, NULL);
  }
  /* "Xfr" or "X.Yfr" */
  /* digits, optional dot, optional digits */
  int int_part = 0, frac_part = 0, frac_div = 1;
  const char *q = p;
  bool had_digit = false;
  while (q < end && *q >= '0' && *q <= '9') {
    int_part = int_part * 10 + (*q - '0'); q++; had_digit = true;
  }
  if (q < end && *q == '.') {
    q++;
    while (q < end && *q >= '0' && *q <= '9') {
      frac_part = frac_part * 10 + (*q - '0');
      frac_div  = frac_div  * 10;
      q++; had_digit = true;
    }
  }
  /* skip whitespace */
  while (q < end && *q == ' ') q++;
  if (q + 2 <= end && strncmp(q, "fr", 2) == 0) {
    int x100 = int_part * 100 + (frac_part * 100) / frac_div;
    if (x100 <= 0) x100 = 100;
    out->kind = 1; /* FR */
    out->fr_x100 = x100;
    out->px = 0;
    return true;
  }
  if (q + 2 <= end && strncmp(q, "px", 2) == 0) {
    out->kind = 0; /* PX */
    out->px = int_part;
    out->fr_x100 = 0;
    return true;
  }
  /* "auto" or anything else → treat as 1fr */
  if (!had_digit && q + 4 <= end && strncmp(q, "auto", 4) == 0) {
    out->kind = 1; out->fr_x100 = 100; out->px = 0;
    return true;
  }
  return false;
}

/* Parse grid-template-columns string. Fills `tracks[]` with
 * decoded tracks, returns track count. Handles repeat(N, X)
 * and repeat(auto-fit/auto-fill, minmax(Y, 1fr)). */
static int grid_parse_template(const char *spec, int container_w, int gap,
                               grid_track_t *tracks, int max_tracks) {
  int n = 0;
  const char *p = spec;
  while (*p && n < max_tracks) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    /* repeat(...) */
    if (strncmp(p, "repeat(", 7) == 0) {
      const char *open  = p + 7;
      const char *comma = strchr(open, ',');
      const char *close = strchr(open, ')');
      if (!comma || !close) break;
      /* Count token */
      char count_tok[16] = {0};
      int cl = (int)(comma - open);
      if (cl > 15) cl = 15;
      memcpy(count_tok, open, cl);
      /* trim */
      char *ct = count_tok;
      while (*ct == ' ') ct++;
      int reps = 0;
      bool auto_fit = false;
      if (strncmp(ct, "auto-fit", 8) == 0 ||
          strncmp(ct, "auto-fill", 9) == 0) {
        auto_fit = true;
      } else {
        while (*ct >= '0' && *ct <= '9') { reps = reps*10 + (*ct-'0'); ct++; }
      }
      grid_track_t inner;
      int min_px = 0;
      if (!grid_parse_track(comma + 1, close, &inner, &min_px)) break;
      if (auto_fit) {
        int mp = min_px > 0 ? min_px : 100;
        /* how many fit: (W + gap) / (mp + gap)  (since N tracks
         * need (N-1) gaps between them: total = N*mp + (N-1)*gap
         * ≤ W → N ≤ (W + gap)/(mp + gap)) */
        reps = (container_w + gap) / (mp + gap);
        if (reps < 1) reps = 1;
        if (reps > max_tracks - n) reps = max_tracks - n;
      }
      for (int i = 0; i < reps && n < max_tracks; i++) tracks[n++] = inner;
      p = close + 1;
      continue;
    }
    /* single track */
    const char *q = p;
    while (*q && *q != ' ' && *q != ',') q++;
    grid_track_t one;
    if (!grid_parse_track(p, q, &one, NULL)) break;
    tracks[n++] = one;
    p = q;
  }
  return n;
}

static void emit_grid_container(walk_ctx_t *w, dom_node_t *n) {
  layout_frame_t *parent = layout_top();
  int container_x = parent->x;
  int container_y = parent->y;
  int container_w = parent->w;
  int gap         = style_stack[style_depth].gap_px;
  int align       = style_stack[style_depth].align_items;
  const char *spec = style_stack[style_depth].grid_cols;

  w_flush(w);

  /* No template → fall back to plain block recursion. */
  if (!spec[0]) {
    for (dom_node_t *c = n->first_child; c; c = c->next_sibling)
      w_emit_node(w, c);
    return;
  }

  grid_track_t tracks[8];
  int n_cols = grid_parse_template(spec, container_w, gap, tracks, 8);
  if (n_cols <= 0) {
    /* Unparseable template → treat as block. */
    for (dom_node_t *c = n->first_child; c; c = c->next_sibling)
      w_emit_node(w, c);
    return;
  }

  /* Compute track widths. */
  int widths[8] = {0};
  int starts[8] = {0};
  const int MIN_COL_W = 50;
  int sum_px = 0;
  int sum_fr = 0;
  for (int i = 0; i < n_cols; i++) {
    if (tracks[i].kind == 0) sum_px += tracks[i].px;
    else                      sum_fr += tracks[i].fr_x100;
  }
  int remaining = container_w - sum_px - (n_cols - 1) * gap;
  if (remaining < 0) remaining = 0;

  /* If even one column would be < MIN_COL_W, degrade to vertical
   * stack of element children with `gap` between rows. */
  bool too_narrow = false;
  for (int i = 0; i < n_cols; i++) {
    if (tracks[i].kind == 0) widths[i] = tracks[i].px;
    else if (sum_fr > 0)     widths[i] = (remaining * tracks[i].fr_x100) / sum_fr;
    else                      widths[i] = remaining / n_cols;
    if (widths[i] < MIN_COL_W) too_narrow = true;
  }

  if (too_narrow) {
    int first = 1;
    for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
      if (c->type != DOM_NODE_ELEMENT) { w_emit_node(w, c); continue; }
      if (!first && gap > 0) layout_top()->y += gap;
      w_emit_node(w, c);
      first = 0;
    }
    w_flush(w);
    return;
  }

  /* x positions: simple left-to-right, no justify-content
   * (grid centring is rare on the target page). */
  int x_cursor = container_x;
  for (int i = 0; i < n_cols; i++) {
    starts[i] = x_cursor;
    x_cursor += widths[i] + gap;
  }

  /* Collect element children (text/comment children inside a
   * grid container are uncommon — we just skip them in the
   * cell-layout pass; the existing block recursion already
   * absorbs them through render_subtree per cell). */
  dom_node_t *kids[32];
  int n_kids = 0;
  for (dom_node_t *c = n->first_child; c && n_kids < 32;
       c = c->next_sibling) {
    if (c->type == DOM_NODE_ELEMENT) kids[n_kids++] = c;
  }
  if (n_kids == 0) {
    /* Fall through: render any text children. */
    for (dom_node_t *c = n->first_child; c; c = c->next_sibling)
      w_emit_node(w, c);
    return;
  }

  int n_rows = (n_kids + n_cols - 1) / n_cols;
  int row_y  = container_y;
  for (int r = 0; r < n_rows; r++) {
    int row_first[8];
    int row_count[8];
    int row_h    [8] = {0};
    int max_h = 0;
    for (int c = 0; c < n_cols; c++) {
      int idx = r * n_cols + c;
      if (idx >= n_kids) { row_first[c] = -1; row_count[c] = 0; continue; }
      int first = -1, count = 0, h = 0;
      render_subtree(w, kids[idx], starts[c], row_y, widths[c],
                     &first, &count, &h);
      row_first[c] = first;
      row_count[c] = count;
      row_h[c]     = h;
      if (h > max_h) max_h = h;
    }
    /* align-items inside the row (mirrors flex). */
    if (align != 0) {
      for (int c = 0; c < n_cols; c++) {
        if (row_count[c] <= 0) continue;
        int dy = 0;
        if      (align == 3 /*center*/) dy = (max_h - row_h[c]) / 2;
        else if (align == 2 /*end*/)    dy = (max_h - row_h[c]);
        if (dy > 0) layout_translate_range(row_first[c], row_count[c], 0, dy);
      }
    }
    row_y += max_h;
    if (r < n_rows - 1) row_y += gap;
  }
  parent->y = row_y;
  w_flush(w);
}

static void w_emit_node(walk_ctx_t *w, dom_node_t *n) {
  if (!n) return;

  if (n->type == DOM_NODE_TEXT) { w_emit_text(w, n->text); return; }
  if (n->type == DOM_NODE_COMMENT) return;
  if (n->type == DOM_NODE_DOCUMENT) {
    for (dom_node_t *c = n->first_child; c; c = c->next_sibling)
      w_emit_node(w, c);
    return;
  }
  /* ELEMENT */
  const char *tag = n->tag_name;

  /* Skip metadata / non-rendered subtrees. */
  if (tag_eq(tag, "head")     || tag_eq(tag, "script")   ||
      tag_eq(tag, "style")    || tag_eq(tag, "noscript") ||
      tag_eq(tag, "title")    || tag_eq(tag, "meta")     ||
      tag_eq(tag, "link")) {
    /* <link rel="stylesheet" href="..."> — fetch local CSS like the
     * legacy parser did. */
    if (tag_eq(tag, "link")) {
      const char *rel  = dom_get_attr(n, "rel");
      const char *href = dom_get_attr(n, "href");
      if (rel && strstr(rel, "stylesheet") && href && href[0] &&
          !strstr(href, "http")) {
        uint32_t css_size = 0;
        void *css_data = (void *)_syscall(SYS_READ_FILE, (uint64_t)href,
                                          (uint64_t)&css_size, 0, 0, 0);
        if (css_data) parse_css_block((const char *)css_data, css_size);
      }
    }
    return;
  }

  push_style_state();
  apply_css_for_node(n);
  if (style_stack[style_depth].display_none) { pop_style_state(); return; }

  style_save_t save = { w->style, w->in_pre };

  /* ---- per-tag prelude ----------------------------------------- */
  if (tag_eq(tag, "body")) {
    if (style_stack[style_depth].bg) body_bg = style_stack[style_depth].bg;
  } else if (tag_eq(tag, "h1")) {
    w_flush(w);
    blank_line();
    w->style = STYLE_H1;
  } else if (tag_eq(tag, "h2") || tag_eq(tag, "h3") || tag_eq(tag, "h4") ||
             tag_eq(tag, "h5") || tag_eq(tag, "h6")) {
    w_flush(w);
    blank_line();
    w->style = STYLE_H2;
    style_stack[style_depth].bold = true;
  } else if (tag_eq(tag, "p")       || tag_eq(tag, "div")     ||
             tag_eq(tag, "section") || tag_eq(tag, "header")  ||
             tag_eq(tag, "footer")  || tag_eq(tag, "article") ||
             tag_eq(tag, "main")    || tag_eq(tag, "nav")     ||
             tag_eq(tag, "aside")   || tag_eq(tag, "blockquote")) {
    w_flush(w);
    int pad = style_stack[style_depth].padding;
    if (pad > 0) {
      for (int p = 0; p < pad; p++) push_line("", 0, STYLE_NORMAL, false);
    } else if (line_count > 0) {
      blank_line();
    }
  } else if (tag_eq(tag, "center")) {
    w_flush(w);
    style_stack[style_depth].align = ALIGN_CENTER;
  } else if (tag_eq(tag, "br")) {
    w_flush(w);
  } else if (tag_eq(tag, "hr")) {
    w_flush(w);
    push_line("", 0, STYLE_HR, false);
  } else if (tag_eq(tag, "img")) {
    w_flush(w);
    /* R6/B4: try to fetch + decode. On success push a STYLE_IMAGE
     * line carrying the image index; on failure fall back to the
     * old text placeholder so the slot is still visible. */
    const char *src = dom_get_attr(n, "src");
    int idx = src ? load_image_for_src(src) : -1;
    if (idx >= 0) {
      push_line("", 0, STYLE_IMAGE, w->in_list);
      lines[line_count - 1].image_idx = idx;
#ifdef BROWSER_BUILD
      /* L1: replace the default LINE_H placeholder with the
       * image's real on-screen advance — image + 2*PAD card + 6
       * px gap, matching render()'s tail bump. */
      const int IMG_PAD = 6;
      int img_h = g_images[idx].h + IMG_PAD * 2 + 6;
      layout_set_last_height(img_h);
#endif
    } else {
      push_line("[ IMAGE ]", 9, STYLE_MUTED, w->in_list);
    }
  } else if (tag_eq(tag, "ul") || tag_eq(tag, "ol")) {
    w_flush(w);
    w->in_list = true;
  } else if (tag_eq(tag, "li")) {
    w_flush(w);
    w->style = STYLE_BULLET;
    w->at_li_start = true;
  } else if (tag_eq(tag, "b") || tag_eq(tag, "strong")) {
    style_stack[style_depth].bold = true;
  } else if (tag_eq(tag, "i") || tag_eq(tag, "em")) {
    style_stack[style_depth].color = CLR_MUTED;
  } else if (tag_eq(tag, "u")) {
    style_stack[style_depth].underline = true;
  } else if (tag_eq(tag, "a")) {
    /* R6: force a word boundary on link open so consecutive inline
     * <a>…</a><a>…</a> pairs without surrounding whitespace
     * ("FeaturesRepos…") don't get glued into one giant word.
     * append_word will re-introduce a separating space the next
     * time something is pushed because current_len > 0. */
    if (w->word_len > 0) {
      append_word(w->current, &w->current_len, w->word, w->word_len,
                  w->style, w->in_list);
      w->word_len = 0;
    }
    w_set_link_context(n);
    w->style = STYLE_LINK;
    style_stack[style_depth].underline = true;
  } else if (tag_eq(tag, "code") || tag_eq(tag, "pre")) {
    w->style = STYLE_CODE;
    w->in_pre = true;
#ifdef BROWSER_BUILD
  } else if (tag_eq(tag, "button")) {
    /* R4/F0: emit a widget line. The button's label is its
     * concatenated text content; we collect it from descendant TEXT
     * nodes (good enough for `<button>Click me</button>` and the
     * common `<button><span>Click</span></button>` patterns). */
    w_flush(w);
    char label[LINE_CHARS + 1]; int li = 0;
    /* Tiny in-order TEXT collector; bounded by LINE_CHARS. */
    dom_node_t *stack[32]; int sp = 0;
    stack[sp++] = n;
    while (sp > 0 && li < LINE_CHARS) {
      dom_node_t *cur = stack[--sp];
      if (!cur) continue;
      if (cur->type == DOM_NODE_TEXT && cur->text) {
        for (const char *t = cur->text; *t && li < LINE_CHARS; t++) {
          if (ascii_isspace(*t)) { if (li && label[li-1] != ' ') label[li++] = ' '; }
          else label[li++] = *t;
        }
      } else if (cur->type == DOM_NODE_ELEMENT) {
        /* push children in reverse so we visit in document order */
        int n_children = 0;
        for (dom_node_t *c = cur->first_child; c; c = c->next_sibling) n_children++;
        dom_node_t *kids[16]; int k = 0;
        for (dom_node_t *c = cur->first_child; c && k < 16; c = c->next_sibling) kids[k++] = c;
        while (k > 0 && sp < 32) stack[sp++] = kids[--k];
      }
    }
    while (li > 0 && label[li-1] == ' ') li--;
    label[li] = 0;
    if (li == 0) { strcpy(label, "Button"); li = 6; }

    blank_line();
    push_line(label, li, STYLE_BUTTON, false);
    lines[line_count - 1].widget_node = n;
    layout_extend_last(6);  /* L1: render() advances by LINE_H+6 */
    /* Don't descend — the label is already captured. */
    pop_style_state();
    w->style = save.style;
    w->in_pre = save.in_pre;
    return;
  } else if (tag_eq(tag, "input")) {
    /* R4/F1: emit an editable input line. We render text-style inputs
     * as STYLE_INPUT widgets, and (R4/F2) type="submit" as STYLE_BUTTON
     * widgets carrying the input node so the click handler can submit
     * the enclosing form. Other types (hidden / file / image / reset /
     * button) are skipped at render time but still participate in
     * form_collect (except where excluded). <input> is void in HTML,
     * so don't descend. */
    const char *type = dom_get_attr(n, "type");
    bool is_textish = (!type ||
                       strcasecmp(type, "text")   == 0 ||
                       strcasecmp(type, "search") == 0 ||
                       strcasecmp(type, "url")    == 0 ||
                       strcasecmp(type, "email")  == 0 ||
                       strcasecmp(type, "tel")    == 0 ||
                       strcasecmp(type, "password") == 0);
    bool is_submit   = type && strcasecmp(type, "submit") == 0;
    bool is_checkbox = type && strcasecmp(type, "checkbox") == 0;
    bool is_radio    = type && strcasecmp(type, "radio") == 0;
    if (is_checkbox || is_radio) {
      w_flush(w);
      /* The toggle box itself has no inline text; the surrounding
       * <label>/text flows naturally on its own line. */
      blank_line();
      push_line("", 0, STYLE_CHECKBOX, false);
      lines[line_count - 1].widget_node = n;
      layout_extend_last(6);  /* L1: render() advances by LINE_H+6 */
    } else if (is_textish) {
      w_flush(w);
      const char *val = dom_get_attr(n, "value");
      char vbuf[LINE_CHARS + 1]; int vlen = 0;
      if (val) {
        for (const char *t = val; *t && vlen < LINE_CHARS; t++) vbuf[vlen++] = *t;
      }
      vbuf[vlen] = 0;
      blank_line();
      push_line(vbuf, vlen, STYLE_INPUT, false);
      lines[line_count - 1].widget_node = n;
      layout_extend_last(6);  /* L1: render() advances by LINE_H+6 */
    } else if (is_submit) {
      w_flush(w);
      const char *val = dom_get_attr(n, "value");
      const char *label = (val && val[0]) ? val : "Submit";
      int li = (int)strlen(label);
      if (li > LINE_CHARS) li = LINE_CHARS;
      char lbuf[LINE_CHARS + 1];
      memcpy(lbuf, label, li); lbuf[li] = 0;
      blank_line();
      push_line(lbuf, li, STYLE_BUTTON, false);
      lines[line_count - 1].widget_node = n;
      layout_extend_last(6);  /* L1: render() advances by LINE_H+6 */
    }
    pop_style_state();
    w->style = save.style;
    w->in_pre = save.in_pre;
    return;
  } else if (tag_eq(tag, "select")) {
    /* R5/N2: emit a STYLE_SELECT widget showing the currently-
     * selected <option>'s visible text. Initialise the select's
     * `value` attr from the first <option selected> (or the first
     * option) if it isn't set yet. Don't descend — option children
     * would otherwise render as plain text. */
    w_flush(w);

    /* Locate the currently-selected option. */
    dom_node_t *selected = NULL;
    dom_node_t *first    = NULL;
    for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
      if (c->type == DOM_NODE_ELEMENT && c->tag_name &&
          strcmp(c->tag_name, "option") == 0) {
        if (!first) first = c;
        if (dom_get_attr(c, "selected") && !selected) selected = c;
      }
    }
    if (!selected) selected = first;

    /* Sync select's `value` if it's not set yet. */
    const char *cur_v = dom_get_attr(n, "value");
    if ((!cur_v || !cur_v[0]) && selected) {
      const char *ov = dom_get_attr(selected, "value");
      /* If <option> has no value attribute, the option text is the
       * value. Collect option text below into a buffer either way. */
      char otxt[64]; int ol = 0;
      for (dom_node_t *c = selected->first_child; c && ol < (int)sizeof(otxt) - 1; c = c->next_sibling) {
        if (c->type == DOM_NODE_TEXT && c->text) {
          for (const char *t = c->text; *t && ol < (int)sizeof(otxt) - 1; t++) otxt[ol++] = *t;
        }
      }
      otxt[ol] = 0;
      dom_set_attr(n, "value", (ov && ov[0]) ? ov : otxt);
    }

    /* Build the display label: the selected option's text. */
    char label[LINE_CHARS + 1]; int li = 0;
    if (selected) {
      for (dom_node_t *c = selected->first_child; c && li < LINE_CHARS; c = c->next_sibling) {
        if (c->type == DOM_NODE_TEXT && c->text) {
          for (const char *t = c->text; *t && li < LINE_CHARS; t++) label[li++] = *t;
        }
      }
    }
    if (li == 0) { strcpy(label, "(select)"); li = 8; }
    label[li] = 0;

    blank_line();
    push_line(label, li, STYLE_SELECT, false);
    lines[line_count - 1].widget_node = n;
    layout_extend_last(6);  /* L1: render() advances by LINE_H+6 */

    pop_style_state();
    w->style = save.style;
    w->in_pre = save.in_pre;
    return;
#endif
  }

  /* ---- recurse into children ----------------------------------- */
  /* R6/L4: dispatch to the flex layout instead of plain recursion
   * when the element resolved display:flex. Grid (display:2) will
   * land in L5. */
  int disp = style_stack[style_depth].display;
  if (disp == 1) {
    emit_flex_container(w, n);
  } else if (disp == 2) {
    emit_grid_container(w, n);
  } else {
    for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
      w_emit_node(w, c);
    }
  }

  /* ---- per-tag postlude ---------------------------------------- */
  if (tag_eq(tag, "h1") ||
      tag_eq(tag, "h2") || tag_eq(tag, "h3") || tag_eq(tag, "h4") ||
      tag_eq(tag, "h5") || tag_eq(tag, "h6")) {
    w_flush(w);
    w->style = STYLE_NORMAL;
    blank_line();
  } else if (tag_eq(tag, "p")       || tag_eq(tag, "div")     ||
             tag_eq(tag, "section") || tag_eq(tag, "header")  ||
             tag_eq(tag, "footer")  || tag_eq(tag, "article") ||
             tag_eq(tag, "main")    || tag_eq(tag, "nav")     ||
             tag_eq(tag, "aside")   || tag_eq(tag, "blockquote")) {
    w_flush(w);
    int pad = style_stack[style_depth].padding;
    if (pad > 0) {
      for (int p = 0; p < pad; p++) push_line("", 0, STYLE_NORMAL, false);
    } else {
      blank_line();
    }
  } else if (tag_eq(tag, "center")) {
    w_flush(w);
  } else if (tag_eq(tag, "ul") || tag_eq(tag, "ol")) {
    w_flush(w);
    w->in_list = false;
    blank_line();
  } else if (tag_eq(tag, "li")) {
    w_flush(w);
    w->style = STYLE_NORMAL;
    w->at_li_start = false;
  } else if (tag_eq(tag, "a")) {
    /* R6: mirror the open-side flush so the next inline run starts
     * fresh and append_word inserts a separator. */
    if (w->word_len > 0) {
      append_word(w->current, &w->current_len, w->word, w->word_len,
                  w->style, w->in_list);
      w->word_len = 0;
    }
    w->style = w->in_list ? STYLE_BULLET : STYLE_NORMAL;
    tag_context[0] = 0;
  } else if (tag_eq(tag, "code") || tag_eq(tag, "pre")) {
    w_flush(w);
    w->style = w->in_list ? STYLE_BULLET : STYLE_NORMAL;
    w->in_pre = false;
  }
  (void)save;   /* most paths overwrite explicitly; struct kept for clarity */

  pop_style_state();
}

/* Renamed-forward declaration. Original definition below this block. */
static void parse_html_legacy(const char *html, uint32_t size);

/* Selects the parser. Flipped to true via the --legacy CLI flag. */
static bool g_use_legacy_parser = false;

/* --------------------------------------------------------------------
 * R4/F0: keep the parsed tree + QuickJS session alive across frames so
 * widget events (button click → JS handler) can fire post-load. The
 * old "parse → walk → free" pipeline is now split into:
 *
 *   parse_html(html, size)
 *     dom_free(prev) + qjs_page_free(prev)
 *     dom_parse → walk → install JS session
 *
 *   rebuild_lines_from_dom()
 *     walks the live tree again into a fresh lines[] (after a JS
 *     mutation, or after a renderer-side widget value change).
 * ------------------------------------------------------------------ */

static dom_node_t *g_doc;
#ifdef BROWSER_BUILD
static qjs_page_t *g_page;

/* R5/N2: track which text <input> is currently focused so we can fire
 * a `change` event when the user navigates away with a different
 * value. Cleared whenever a new page is parsed (since the old node
 * pointer goes away with dom_free). */
static dom_node_t *focus_input_node = NULL;
static char        focus_input_snapshot[64] = {0};

/* R5/N1: act on a navigation request the page's JS produced via
 * location.assign/replace/reload or history.back/forward/go.
 *   kind 1 ASSIGN   relative url → resolve, load, push.
 *   kind 2 REPLACE  resolve, load, *do not* push.
 *   kind 3 RELOAD   reload current url without pushing.
 *   kind 4 HISTORY  jump history_ptr by delta (clamped to populated
 *                   slots), reload that url without pushing. */
static void apply_nav_request(int kind, const char *url, int delta) {
  switch (kind) {
    case 1:
    case 2: {
      if (!url || !*url) return;
      char resolved[512];
      resolve_url(current_url, url, resolved);
      strncpy(current_url, resolved, sizeof(current_url) - 1);
      current_url[sizeof(current_url) - 1] = 0;
      if (kind == 2) is_navigating_history = true; /* suppress push */
      load_page(current_url);
      is_navigating_history = false;
      return;
    }
    case 3:
      is_navigating_history = true;
      load_page(current_url);
      is_navigating_history = false;
      return;
    case 4: {
      int target = history_ptr + delta;
      if (target < 0) target = 0;
      if (target > 15) target = 15;
      if (target == history_ptr) return;
      /* Forward (target > current) is only valid if the slot was
       * filled by an earlier push that we haven't overwritten. */
      if (target > history_ptr && history[target][0] == 0) return;
      history_ptr = target;
      strcpy(current_url, history[history_ptr]);
      is_navigating_history = true;
      load_page(current_url);
      is_navigating_history = false;
      qjs_window_set_history_length(history_ptr + 1);
      return;
    }
    default:
      return;
  }
}

/* Drain one nav request from the JS side (if any) and act on it.
 * Returns 1 if a navigation was triggered (caller should bail out of
 * its current paint/event loop), 0 if there was nothing to do. */
static int drain_pending_nav(void) {
  if (!g_page) return 0;
  int kind = 0, delta = 0;
  char url[512] = {0};
  if (!qjs_page_take_nav(g_page, &kind, url, sizeof url, &delta)) return 0;
  apply_nav_request(kind, url, delta);
  return 1;
}
#endif

/* ─────────────────────────────────────────────────────────────────
 * Phase R6/L3: render_subtree — walk a DOM subtree under a private
 * layout frame and report which lines were produced and how tall
 * the resulting block is.
 *
 * Why this exists: flex / grid containers (L4 + L5) need to lay
 * out children *side by side* in columns whose widths are smaller
 * than the parent's full content width. The natural way to do
 * that with the existing w_emit_node machinery is to:
 *   1. push a sub-frame at (column_x, parent_y, column_w),
 *   2. emit the child subtree into lines[] under that frame,
 *   3. note (first_line, line_count_in_subtree, sub_height),
 *   4. pop the sub-frame,
 *   5. either splice the lines as-is (if the column's left edge is
 *      already correct from the sub-frame) or translate their
 *      box_x/box_y in place (when reflowing for align/justify).
 *
 * This function does steps 1–4. The caller decides what to do with
 * the captured range in step 5; L3 itself just gives flex/grid the
 * primitive they'll build on. Not called yet — pure scaffolding,
 * exercised first by L4.
 *
 * Note: walk_ctx_t style/in_pre/in_list are shared with the
 * caller because they reflect CSS inheritance from the *parent*
 * (e.g. a flex item inside <body> still inherits body's font).
 * The caller is responsible for save/restore around render_subtree
 * if it wants to isolate them; in practice the standard
 * pop_style_state() flow already handles per-element styles.
 *
 * Out-params (may be NULL):
 *   *out_first — index of the first line emitted (line_count at
 *                entry), or -1 if no lines were produced.
 *   *out_count — number of lines emitted by the subtree.
 *   *out_h     — vertical advance consumed by the sub-frame, in px.
 * ────────────────────────────────────────────────────────────── */
static void render_subtree(walk_ctx_t *w, dom_node_t *node,
                           int frame_x, int frame_y, int frame_w,
                           int *out_first, int *out_count, int *out_h) {
  int first_before = line_count;

  /* Push a private frame; on overflow degrade to the parent frame
   * so the subtree still emits *somewhere*, just not isolated.
   * Better than silently dropping content on deeply-nested HTML. */
  layout_frame_t *f = layout_push(frame_x, frame_y, frame_w, /*flow=*/0);
  bool pushed = (f != NULL);

  /* w_flush before and after so any pending inline text run from
   * the parent doesn't leak into / out of the sub-frame's lines. */
  w_flush(w);
  if (node) w_emit_node(w, node);
  w_flush(w);

  int captured_h = 0;
  if (pushed) {
    captured_h = layout_pop();
  }
  /* Frame-push overflow path: emission still happened on the
   * parent frame, but we can't cleanly report a height for it,
   * so leave captured_h at 0 and let the caller fall back to
   * a "skip this container" path. */

  int captured_n = line_count - first_before;
  if (out_first) *out_first = (captured_n > 0) ? first_before : -1;
  if (out_count) *out_count = captured_n;
  if (out_h)     *out_h     = captured_h;
}

/* R6/L3 helper: shift an already-emitted range of lines by
 * (dx, dy) pixels. Used by flex/grid containers in L4+L5 after
 * they know the final column / cell position. Safe to call with
 * (0, 0) — becomes a no-op. */
static void layout_translate_range(int first, int count, int dx, int dy) {
  if (first < 0 || count <= 0) return;
  for (int i = 0; i < count; i++) {
    int idx = first + i;
    if (idx < 0 || idx >= line_count) break;
    lines[idx].box_x += dx;
    lines[idx].box_y += dy;
  }
}

static void rebuild_lines_from_dom(void) {
  /* Preserve scroll across in-page rebuilds (e.g. when a click on a
   * checkbox / radio / <select> mutates the DOM). Callers that want
   * to jump to the top — load_page / parse_html OOM, history nav —
   * reset scroll_line explicitly. */
  int saved_scroll = scroll_line;
  line_count  = 0;
  layout_reset();
  reset_style_stack();
  body_bg = CLR_BG;
  tag_context[0] = 0;

  if (!g_doc) {
    push_line("(no document)", 13, STYLE_MUTED, false);
    return;
  }

  walk_ctx_t w;
  memset(&w, 0, sizeof(w));
  w.style = STYLE_NORMAL;
  w_emit_node(&w, g_doc);
  w_flush(&w);

  if (line_count == 0)
    push_line("(empty HTML document)", 21, STYLE_MUTED, false);

  /* Clamp restored scroll to the new content height. */
  int v = visible_lines();
  int max_scroll = line_count - v;
  if (max_scroll < 0) max_scroll = 0;
  if (saved_scroll > max_scroll) saved_scroll = max_scroll;
  if (saved_scroll < 0)          saved_scroll = 0;
  scroll_line = saved_scroll;
}

static void parse_html(const char *html, uint32_t size) {
  if (g_use_legacy_parser) { parse_html_legacy(html, size); return; }

  /* Drop the previous page's runtime + tree before we leak. The free
   * order matters: JS first (it may hold borrowed DOM pointers via
   * unwrap_element), then the DOM tree itself. */
#ifdef BROWSER_BUILD
  if (g_page) { qjs_page_free(g_page); g_page = NULL; }
#endif
  if (g_doc)  { dom_free(g_doc); g_doc = NULL; }
#ifdef BROWSER_BUILD
  /* R5/N2: focus tracker pointed at the now-freed tree — reset. */
  focus_input_node = NULL;
  focus_input_snapshot[0] = 0;
  /* R6/B4: drop the previous page's decoded image buffers before
   * the upcoming walk re-fetches and re-decodes for the new page. */
  images_reset();
#endif

  copy_title_from_html(html, size);
  extract_css(html, size);

  g_doc = dom_parse(html, size);
  if (!g_doc) {
    line_count = 0; scroll_line = 0;
    push_line("(out of memory parsing HTML)", 28, STYLE_MUTED, false);
    return;
  }

#ifdef BROWSER_BUILD
  /* J6a + R4/F0: stand up the persistent JS session. Initial inline
   * scripts run inside qjs_page_create; addEventListener callbacks
   * registered there survive until the next navigation, so widget
   * events (post-paint) can dispatch into them. */
  g_page = qjs_page_create(g_doc, current_url, TAs_MOZ, TAs_MOZ_NUM);
  /* R5/N0+N1: an inline <script> can navigate via
   * location.href / assign / replace / reload / history.*; if so,
   * redirect immediately instead of rendering the now-stale tree. */
  if (drain_pending_nav()) return;
#endif

  rebuild_lines_from_dom();
}

#ifdef BROWSER_BUILD
/* --------------------------------------------------------------------
 * R4/F2 — HTML form submission
 *
 * Triggered by:
 *   - click on `<input type="submit">` (rendered as a STYLE_BUTTON)
 *   - click on `<button type="submit">`
 *   - Enter pressed while a STYLE_INPUT widget is focused
 *
 * Walks the form's descendants, collects name=value pairs from named
 * inputs (skipping submit/button/reset/image/file/no-name), URL-encodes
 * them, and navigates to `action`?...query. POST is downgraded to GET
 * for now (no body upload path in HTTP client yet) — same observable
 * behaviour for static pages.
 *
 * A `submit` event is dispatched to the form node before navigation so
 * scripts can observe; preventDefault is not implemented yet, so JS
 * cannot cancel.
 * ------------------------------------------------------------------ */
static dom_node_t *find_form_ancestor(dom_node_t *n) {
  for (dom_node_t *p = n ? n->parent : NULL; p; p = p->parent) {
    if (p->type == DOM_NODE_ELEMENT && p->tag_name &&
        strcmp(p->tag_name, "form") == 0) return p;
  }
  return NULL;
}

static int url_encode_into(const char *s, char *dst, int j, int cap) {
  static const char hex[] = "0123456789ABCDEF";
  for (int i = 0; s && s[i] && j + 3 < cap; i++) {
    unsigned char c = (unsigned char)s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
         c == '-' || c == '_' || c == '.' || c == '~') {
      dst[j++] = (char)c;
    } else if (c == ' ') {
      dst[j++] = '+';
    } else {
      dst[j++] = '%';
      dst[j++] = hex[(c >> 4) & 0xF];
      dst[j++] = hex[c & 0xF];
    }
  }
  if (j < cap) dst[j] = 0;
  return j;
}

static void form_collect(dom_node_t *node, char *qbuf, int *jp, int cap, int *first) {
  if (!node) return;
  if (node->type == DOM_NODE_ELEMENT && node->tag_name &&
      strcmp(node->tag_name, "input") == 0) {
    const char *name = dom_get_attr(node, "name");
    const char *type = dom_get_attr(node, "type");
    bool skip = false;
    if (type) {
      if (strcasecmp(type, "submit") == 0 ||
          strcasecmp(type, "button") == 0 ||
          strcasecmp(type, "reset")  == 0 ||
          strcasecmp(type, "image")  == 0 ||
          strcasecmp(type, "file")   == 0) skip = true;
      /* R5/N2: unchecked checkbox/radio are not submitted. */
      if ((strcasecmp(type, "checkbox") == 0 || strcasecmp(type, "radio") == 0) &&
          !dom_get_attr(node, "checked")) skip = true;
    }
    if (name && name[0] && !skip) {
      const char *val = dom_get_attr(node, "value");
      /* Checkbox/radio with no value attribute defaults to "on" — the
       * HTML standard. */
      if (type && (strcasecmp(type, "checkbox") == 0 || strcasecmp(type, "radio") == 0) &&
          (!val || !val[0])) val = "on";
      if (!*first && *jp < cap - 1) qbuf[(*jp)++] = '&';
      *first = 0;
      *jp = url_encode_into(name, qbuf, *jp, cap);
      if (*jp < cap - 1) qbuf[(*jp)++] = '=';
      *jp = url_encode_into(val ? val : "", qbuf, *jp, cap);
    }
  } else if (node->type == DOM_NODE_ELEMENT && node->tag_name &&
             strcmp(node->tag_name, "select") == 0) {
    /* R5/N2: submit the select's current value attribute. */
    const char *name = dom_get_attr(node, "name");
    const char *val  = dom_get_attr(node, "value");
    if (name && name[0]) {
      if (!*first && *jp < cap - 1) qbuf[(*jp)++] = '&';
      *first = 0;
      *jp = url_encode_into(name, qbuf, *jp, cap);
      if (*jp < cap - 1) qbuf[(*jp)++] = '=';
      *jp = url_encode_into(val ? val : "", qbuf, *jp, cap);
    }
    /* Don't descend into <option> — they aren't form controls. */
    return;
  }
  for (dom_node_t *c = node->first_child; c; c = c->next_sibling)
    form_collect(c, qbuf, jp, cap, first);
}

static void submit_form_for(dom_node_t *trigger) {
  dom_node_t *form = find_form_ancestor(trigger);
  if (!form) return;

  /* R4/F3: honour event.preventDefault() inside submit listeners.
   * If anyone called it, suppress the navigation but still let
   * subsequent paints reflect any DOM mutations the handler did. */
  if (g_page) {
    int prevented = qjs_page_dispatch_event(g_page, form, "submit");
    if (prevented) return;
  }

  const char *action_raw = dom_get_attr(form, "action");
  const char *action = (action_raw && action_raw[0]) ? action_raw : current_url;

  char qbuf[1024]; int jp = 0; int first = 1;
  form_collect(form, qbuf, &jp, (int)sizeof qbuf, &first);
  qbuf[jp] = 0;

  char resolved[256];
  resolve_url(current_url, action, resolved);

  if (jp > 0) {
    char sep = strchr(resolved, '?') ? '&' : '?';
    int rl = (int)strlen(resolved);
    if (rl + 1 + jp < (int)sizeof resolved) {
      resolved[rl++] = sep;
      memcpy(resolved + rl, qbuf, (size_t)jp);
      resolved[rl + jp] = 0;
    }
  }

  strcpy(current_url, resolved);
  load_page(current_url);
}

/* R5/N2: walk the form (or document if no enclosing form) and clear
 * the "checked" attribute on every other radio that shares `name`
 * with `chosen`. Mirrors the HTML radio-group behaviour. */
static void uncheck_radio_siblings(dom_node_t *chosen) {
  if (!chosen || !chosen->tag_name) return;
  const char *grp = dom_get_attr(chosen, "name");
  if (!grp || !*grp) return;

  dom_node_t *root = find_form_ancestor(chosen);
  if (!root) root = g_doc;
  if (!root) return;

  /* Iterative DFS. */
  dom_node_t *stack[64]; int sp = 0;
  stack[sp++] = root;
  while (sp > 0) {
    dom_node_t *cur = stack[--sp];
    if (!cur) continue;
    if (cur != chosen && cur->type == DOM_NODE_ELEMENT && cur->tag_name &&
        strcmp(cur->tag_name, "input") == 0) {
      const char *t = dom_get_attr(cur, "type");
      const char *m = dom_get_attr(cur, "name");
      if (t && strcasecmp(t, "radio") == 0 && m && strcmp(m, grp) == 0) {
        dom_remove_attr(cur, "checked");
      }
    }
    for (dom_node_t *c = cur->first_child; c && sp < 64; c = c->next_sibling)
      stack[sp++] = c;
  }
}

/* R5/N2: pick the next <option> after `cur` in `select`, wrapping
 * around to the first one. Returns NULL if `select` has no options. */
static dom_node_t *next_option(dom_node_t *select, dom_node_t *cur) {
  dom_node_t *first = NULL;
  dom_node_t *after = NULL;
  bool seen = false;
  for (dom_node_t *c = select->first_child; c; c = c->next_sibling) {
    if (c->type == DOM_NODE_ELEMENT && c->tag_name &&
        strcmp(c->tag_name, "option") == 0) {
      if (!first) first = c;
      if (seen && !after) { after = c; break; }
      if (c == cur) seen = true;
    }
  }
  return after ? after : first;
}

/* R5/N2: find the <option> whose value (or text if no value attr)
 * matches `v` inside `select`. NULL if not found. */
static dom_node_t *find_option_by_value(dom_node_t *select, const char *v) {
  if (!v) return NULL;
  for (dom_node_t *c = select->first_child; c; c = c->next_sibling) {
    if (c->type == DOM_NODE_ELEMENT && c->tag_name &&
        strcmp(c->tag_name, "option") == 0) {
      const char *ov = dom_get_attr(c, "value");
      if (ov && strcmp(ov, v) == 0) return c;
      if (!ov || !ov[0]) {
        /* No value attr — use option text. */
        char otxt[64]; int ol = 0;
        for (dom_node_t *t = c->first_child; t && ol < (int)sizeof(otxt) - 1; t = t->next_sibling) {
          if (t->type == DOM_NODE_TEXT && t->text) {
            for (const char *s = t->text; *s && ol < (int)sizeof(otxt) - 1; s++) otxt[ol++] = *s;
          }
        }
        otxt[ol] = 0;
        if (strcmp(otxt, v) == 0) return c;
      }
    }
  }
  return NULL;
}

/* True iff `node` is `<input type="submit">` or `<button type="submit">`.
 * Used to decide whether STYLE_BUTTON click should also trigger a form
 * submit on top of the JS 'click' dispatch. */
static bool is_submit_widget(dom_node_t *n) {
  if (!n || !n->tag_name) return false;
  const char *type = dom_get_attr(n, "type");
  if (strcmp(n->tag_name, "input") == 0) {
    return type && strcasecmp(type, "submit") == 0;
  }
  if (strcmp(n->tag_name, "button") == 0) {
    /* Real HTML default for `<button>` is type=submit, but to avoid
     * surprising scripted buttons we require the type to be explicit
     * OR for the button to live inside a <form>. Bare button outside
     * any form is always click-only. */
    if (type) return strcasecmp(type, "submit") == 0;
    return find_form_ancestor(n) != NULL;
  }
  return false;
}
#endif /* BROWSER_BUILD */

static void parse_html_legacy(const char *html, uint32_t size) {
  line_count = 0;
  layout_reset();
  scroll_line = 0;
  line_style_t style = STYLE_NORMAL;
  bool in_body = false;
  bool in_pre = false;
  bool in_list = false;
  bool skipping_head = false;
  bool at_li_start = false;
  bool in_style_tag = false;
  bool in_script_tag = false;
  bool in_noscript_tag = false;

  char current[LINE_CHARS + 1];
  char word[LINE_CHARS + 1];
  int current_len = 0;
  int word_len = 0;

  copy_title_from_html(html, size);
  extract_css(html, size);
  reset_style_stack();
  body_bg = CLR_BG;

  /* Debug: print CSS rule count to serial */
  char dbg[64];
  sprintf(dbg, "[CSS] Extracted %d rules\n", css_rule_count);
  print(dbg);
  for (int d = 0; d < css_rule_count && d < 10; d++) {
    sprintf(dbg, "  [%d] sel='%s' c=%06x bg=%06x\n", d, css_rules[d].selector,
            css_rules[d].color, css_rules[d].bg_color);
    print(dbg);
  }

  for (uint32_t i = 0; i < size;) {
    char c = html[i];

    if (c == '<') {
      char tag[256]; /* larger to capture class/id attrs */
      read_tag(html, size, &i, tag, sizeof(tag));
      strcpy(tag_context, tag);

      if (word_len > 0) {
        append_word(current, &current_len, word, word_len, style, in_list);
        word_len = 0;
      }

      /* Skip explicit ignored blocks */
      if (tag_eq(tag, "style")) {
        in_style_tag = true;
        continue;
      }
      if (tag_eq(tag, "/style")) {
        in_style_tag = false;
        continue;
      }
      if (tag_eq(tag, "script")) {
        in_script_tag = true;
        continue;
      }
      if (tag_eq(tag, "/script")) {
        in_script_tag = false;
        continue;
      }
      if (tag_eq(tag, "noscript")) {
        in_noscript_tag = true;
        continue;
      }
      if (tag_eq(tag, "/noscript")) {
        in_noscript_tag = false;
        continue;
      }
      if (tag_eq(tag, "head")) {
        skipping_head = true;
        continue;
      }
      if (tag_eq(tag, "/head")) {
        skipping_head = false;
        continue;
      }

      bool is_self_closing =
          (tag_eq(tag, "br") || tag_eq(tag, "img") || tag_eq(tag, "meta") ||
           tag_eq(tag, "link") || tag_eq(tag, "hr") || tag_eq(tag, "input"));
      bool is_close_tag = (tag[0] == '/');

      /* Maintain styling stack */
      if (!is_close_tag && !is_self_closing) {
        push_style_state();
        apply_css_for_element(tag);
      } else if (is_close_tag) {
        pop_style_state();
      }

      /* Still check for formatting tags (only affecting current line flow,
       * display) */
      if (!skipping_head && !style_stack[style_depth].display_none) {
        if (tag_eq(tag, "body")) {
          in_body = true;
          /* Look for background-color in inline style or CSS */
          apply_css_for_element(tag);
          if (style_stack[style_depth].bg) {
            body_bg = style_stack[style_depth].bg;
          }
        } else if (tag_eq(tag, "/body")) {
          in_body = false;
        } else if (tag_eq(tag, "center")) {
          flush_current(current, &current_len, style, in_list);
          style_stack[style_depth].align = ALIGN_CENTER;
        } else if (tag_eq(tag, "/center")) {
          flush_current(current, &current_len, style, in_list);
        } else if (tag_eq(tag, "div") || tag_eq(tag, "p") ||
                   tag_eq(tag, "header") || tag_eq(tag, "footer") ||
                   tag_eq(tag, "section") || tag_eq(tag, "nav") ||
                   tag_eq(tag, "article") || tag_eq(tag, "main") ||
                   tag_eq(tag, "aside") || tag_eq(tag, "blockquote")) {
          flush_current(current, &current_len, style, in_list);
          if (line_count > 0)
            blank_line();
        } else if (tag_eq(tag, "hr")) {
          flush_current(current, &current_len, style, in_list);
          push_line("", 0, STYLE_HR, false);
        } else if (tag_eq(tag, "img")) {
          flush_current(current, &current_len, style, in_list);
          push_line("[ IMAGE ]", 9, STYLE_MUTED, in_list);
        } else if (tag_eq(tag, "link")) {
          char rel[32], href[128];
          extract_attr(tag, "rel", rel, 31);
          extract_attr(tag, "href", href, 127);
          if (strstr(rel, "stylesheet") && href[0]) {
            /* External CSS fetch placeholder - simplified relative path
             * handling */
            uint32_t css_size = 0;
            void *css_data = NULL;
            if (strstr(href, "http")) {
              /* Fetch absolute HTTP CSS later? */
            } else {
              /* Relative path or local file */
              css_data = (void *)_syscall(SYS_READ_FILE, (uint64_t)href,
                                          (uint64_t)&css_size, 0, 0, 0);
              if (css_data) {
                parse_css_block((const char *)css_data, css_size);
              }
            }
          }
        } else if (tag_eq(tag, "h1")) {
          flush_current(current, &current_len, style, in_list);
          blank_line();
          style = STYLE_H1;
        } else if (tag_eq(tag, "/h1")) {
          flush_current(current, &current_len, style, false);
          style = STYLE_NORMAL;
          blank_line();
        } else if (tag_eq(tag, "h2") || tag_eq(tag, "h3") ||
                   tag_eq(tag, "h4") || tag_eq(tag, "h5") ||
                   tag_eq(tag, "h6")) {
          flush_current(current, &current_len, style, in_list);
          blank_line();
          style = STYLE_H2;
          style_stack[style_depth].bold = true;
        } else if (tag_eq(tag, "/h2") || tag_eq(tag, "/h3") ||
                   tag_eq(tag, "/h4") || tag_eq(tag, "/h5") ||
                   tag_eq(tag, "/h6")) {
          flush_current(current, &current_len, style, false);
          style = STYLE_NORMAL;
          blank_line();
        } else if (tag_eq(tag, "p") || tag_eq(tag, "div") ||
                   tag_eq(tag, "section") || tag_eq(tag, "header") ||
                   tag_eq(tag, "footer") || tag_eq(tag, "article") ||
                   tag_eq(tag, "main") || tag_eq(tag, "nav") ||
                   tag_eq(tag, "aside") || tag_eq(tag, "blockquote")) {
          flush_current(current, &current_len, style, in_list);
          /* Add padding blank lines if CSS padding is set */
          int pad = style_stack[style_depth].padding;
          if (pad > 0) {
            for (int p = 0; p < pad; p++)
              push_line("", 0, STYLE_NORMAL, false);
          } else {
            blank_line();
          }
        } else if (tag_eq(tag, "/p") || tag_eq(tag, "/div") ||
                   tag_eq(tag, "/section") || tag_eq(tag, "/header") ||
                   tag_eq(tag, "/footer") || tag_eq(tag, "/article") ||
                   tag_eq(tag, "/main") || tag_eq(tag, "/nav") ||
                   tag_eq(tag, "/aside") || tag_eq(tag, "/blockquote")) {
          flush_current(current, &current_len, style, in_list);
          int pad = style_stack[style_depth].padding;
          if (pad > 0) {
            for (int p = 0; p < pad; p++)
              push_line("", 0, STYLE_NORMAL, false);
          } else {
            blank_line();
          }
        } else if (tag_eq(tag, "br")) {
          flush_current(current, &current_len, style, in_list);
        } else if (tag_eq(tag, "ul") || tag_eq(tag, "ol")) {
          flush_current(current, &current_len, style, in_list);
          in_list = true;
        } else if (tag_eq(tag, "/ul") || tag_eq(tag, "/ol")) {
          flush_current(current, &current_len, style, in_list);
          in_list = false;
          blank_line();
        } else if (tag_eq(tag, "li")) {
          flush_current(current, &current_len, style, in_list);
          style = STYLE_BULLET;
          at_li_start = true;
        } else if (tag_eq(tag, "/li")) {
          flush_current(current, &current_len, style, true);
          style = STYLE_NORMAL;
          at_li_start = false;
        } else if (tag_eq(tag, "b") || tag_eq(tag, "strong")) {
          style_stack[style_depth].bold = true;
        } else if (tag_eq(tag, "/b") || tag_eq(tag, "/strong")) {
          /* pop handled by logic below */
        } else if (tag_eq(tag, "i") || tag_eq(tag, "em")) {
          /* no italic font, just mark it? */
          style_stack[style_depth].color = CLR_MUTED;
        } else if (tag_eq(tag, "u")) {
          style_stack[style_depth].underline = true;
        } else if (tag_eq(tag, "a")) {
          style = STYLE_LINK;
          style_stack[style_depth].underline = true;
        } else if (tag_eq(tag, "/a")) {
          style = in_list ? STYLE_BULLET : STYLE_NORMAL;
        } else if (tag_eq(tag, "code") || tag_eq(tag, "pre")) {
          style = STYLE_CODE;
          in_pre = true;
        } else if (tag_eq(tag, "/code") || tag_eq(tag, "/pre")) {
          flush_current(current, &current_len, style, in_list);
          style = in_list ? STYLE_BULLET : STYLE_NORMAL;
          in_pre = false;
        }
      }
      continue;
    }

    /* Skip content if hidden or in metadata tags */
    if (in_style_tag || in_script_tag || in_noscript_tag ||
        style_stack[style_depth].display_none) {
      i++;
      continue;
    }

    if (!in_body && !skipping_head && line_count == 0) {
      in_body = true;
    }

    if (skipping_head) {
      i++;
      continue;
    }

    if (at_li_start) {
      append_word(current, &current_len, "*", 1, STYLE_BULLET, true);
      at_li_start = false;
    }

    if (in_pre && (c == '\n' || c == '\r')) {
      if (word_len > 0) {
        append_word(current, &current_len, word, word_len, style, in_list);
        word_len = 0;
      }
      flush_current(current, &current_len, style, in_list);
      i++;
      continue;
    }

    /* HTML entity decoding */
    if (c == '&') {
      char decoded;
      int consumed = decode_entity(html + i, &decoded);
      if (consumed > 0) {
        if (word_len < LINE_CHARS)
          word[word_len++] = decoded;
        i += consumed;
        continue;
      }
    }

    if (ascii_isspace(c)) {
      if (word_len > 0) {
        append_word(current, &current_len, word, word_len, style, in_list);
        word_len = 0;
      }
    } else if (word_len < LINE_CHARS) {
      word[word_len++] = c;
    }

    i++;
  }

  if (word_len > 0)
    append_word(current, &current_len, word, word_len, style, in_list);
  flush_current(current, &current_len, style, in_list);

  if (line_count == 0)
    push_line("(empty HTML document)", 21, STYLE_MUTED, false);
}

static uint32_t color_for_style(line_style_t style) {
  switch (style) {
  case STYLE_H1:
    return CLR_H1;
  case STYLE_H2:
    return CLR_H2;
  case STYLE_LINK:
    return CLR_LINK;
  case STYLE_CODE:
    return CLR_CODE;
  case STYLE_MUTED:
    return CLR_MUTED;
  case STYLE_BULLET:
    return CLR_TEXT;
  case STYLE_HR:
    return CLR_BORDER;
  default:
    return CLR_TEXT;
  }
}

static void draw_text_line(int x, int y, const line_t *ln) {
  line_style_t style = ln->style;
  const char *text = ln->text;

  /* R6/B2c: width in pixels = visible cells × 8 (Cyrillic is 1 cell
   * per codepoint but 2 bytes per codepoint, so strlen overcounts). */
  int w = eid_text_width_utf8(text);
  /* Use TTF width estimate if using TTF font */
  bool use_ttf = false;
  eid_font_t *draw_font = NULL;
  if (style == STYLE_H1 && h_font_large) {
    use_ttf = true;
    draw_font = h_font_large;
    w = strlen(text) * 11; /* approximate TTF width */
  } else if ((style == STYLE_H1 || style == STYLE_H2) && h_font) {
    use_ttf = true;
    draw_font = h_font;
    w = strlen(text) * 9;
  } else if (ln->font_size >= 2 && h_font_large) {
    use_ttf = true;
    draw_font = h_font_large;
    w = strlen(text) * 11;
  } else if (ln->font_size >= 1 && h_font) {
    use_ttf = true;
    draw_font = h_font;
    w = strlen(text) * 9;
  }

  if (ln->css_align == ALIGN_CENTER) {
    x = CONTENT_X + (CONTENT_W - w) / 2;
  } else if (ln->css_align == ALIGN_RIGHT) {
    x = CONTENT_X + CONTENT_W - w;
  }

  uint32_t color = ln->css_color ? ln->css_color : color_for_style(style);

  /* CSS background override */
  if (ln->css_bg) {
    if (ln->full_width_bg) {
      /* Fill the entire window width for block-level backgrounds */
      eid_draw_rect(fb, WIN_W, WIN_H, 0, y - 2, WIN_W, LINE_H, ln->css_bg);
    } else {
      int bg_w = w + 12;
      if (bg_w < 24)
        bg_w = 24;
      eid_draw_rect(fb, WIN_W, WIN_H, x - 4, y - 2, bg_w, LINE_H, ln->css_bg);
    }
  } else if (style == STYLE_CODE) {
    int bg_w = w + 8;
    if (bg_w < 24)
      bg_w = 24;
    if (bg_w > CONTENT_W)
      bg_w = CONTENT_W;
    eid_draw_rect(fb, WIN_W, WIN_H, x - 4, y - 2, bg_w, LINE_H, CLR_CODE_BG);
  }

  if (style == STYLE_HR) {
    eid_draw_line(fb, WIN_W, WIN_H, CONTENT_X, y + 8, CONTENT_X + CONTENT_W,
                  y + 8, CLR_BORDER);
    return;
  }

  /* Bold: draw twice with offset if we don't have a bold font */
  if (ln->css_bold) {
    if (use_ttf && draw_font) {
      eid_draw_text_ttf(&ui, draw_font, x + 1, y, text, color);
    } else {
      eid_draw_text(fb, WIN_W, WIN_H, x + 1, y, text, color);
    }
  }

  if (use_ttf && draw_font) {
    eid_draw_text_ttf(&ui, draw_font, x, y, text, color);
  } else {
    eid_draw_text(fb, WIN_W, WIN_H, x, y, text, color);
  }

  /* Underline: from CSS or from default style rules */
  if (ln->css_underline || style == STYLE_LINK) {
    uint32_t ul_color = ln->css_color ? ln->css_color : CLR_LINK;
    eid_draw_line(fb, WIN_W, WIN_H, x, y + 15, x + w, y + 15, ul_color);
  }
}

static void render(const char *filename) {
  /* Immediate-mode focus reset: any fresh click clears focus_id at
   * the top of the frame. Per-widget eid_process_interaction then
   * re-sets focus_id for whichever widget the click landed on, if
   * any. As a result, clicking on plain page text drops focus from
   * a previously-focused <input> (and our STYLE_INPUT branch sees
   * !focused → dispatches the synthetic `change` event). */
  if (ui.m_clicked) ui.focus_id = 0;

  /* Main background (page) */
  eid_draw_rect(fb, WIN_W, WIN_H, 0, 0, WIN_W, WIN_H, body_bg);

  /* Browser Chrome (Header) */
  eid_draw_rect(fb, WIN_W, WIN_H, 0, 0, WIN_W, 42, CLR_CHROME);
  eid_draw_line(fb, WIN_W, WIN_H, 0, 42, WIN_W, 42, CLR_BORDER);

  /* Title Bar and Navigation */
  eid_draw_text(fb, WIN_W, WIN_H, 14, 12, "EQX", CLR_ACCENT);

  /* Back Button */
  uint32_t back_id = eid_get_id("back", 60, 10);
  uint32_t back_state = eid_process_interaction(&ui, back_id, 60, 10, 24, 22);
  eid_draw_rect(fb, WIN_W, WIN_H, 60, 10, 24, 22,
                (back_state & EID_STATE_HOVER) ? 0x3C4043 : CLR_CHROME_2);
  eid_draw_text(fb, WIN_W, WIN_H, 68, 15, "<", 0xFFFFFF);
  if (back_state & EID_STATE_CLICKED && history_ptr > 0) {
    history_ptr--;
    is_navigating_history = true;
    strcpy(current_url, history[history_ptr]);
    load_page(current_url);
    is_navigating_history = false;
#ifdef BROWSER_BUILD
    qjs_window_set_history_length(history_ptr + 1);
#endif
  }

  /* Refresh Button */
  uint32_t re_id = eid_get_id("refresh", 90, 10);
  uint32_t re_state = eid_process_interaction(&ui, re_id, 90, 10, 24, 22);
  eid_draw_rect(fb, WIN_W, WIN_H, 90, 10, 24, 22,
                (re_state & EID_STATE_HOVER) ? 0x3C4043 : CLR_CHROME_2);
  eid_draw_text(fb, WIN_W, WIN_H, 98, 15, "R", 0xFFFFFF);
  if (re_state & EID_STATE_CLICKED) {
    load_page(current_url);
  }

  /* URL Bar */
  int url_w = WIN_W - 180;
  eid_draw_rect(fb, WIN_W, WIN_H, 140, 8, url_w, 26, CLR_CHROME_2);
  eid_draw_rect(fb, WIN_W, WIN_H, 140, 8, url_w, 1,
                0x3C4043); /* inner shadow hint */

  char url_display[140];
  if (is_typing_url) {
    sprintf(url_display, "%s_", current_url);
    eid_draw_text(fb, WIN_W, WIN_H, 150, 14, url_display, 0xFFFFFF);
  } else {
    eid_draw_text(fb, WIN_W, WIN_H, 150, 14, current_url, 0xBDC1C6);
  }

  /* Page Title Bar */
  eid_draw_rect(fb, WIN_W, WIN_H, 0, 43, WIN_W, 22, 0xEDF2FA);
  eid_draw_text(fb, WIN_W, WIN_H, CONTENT_X, 47, page_title, 0x444746);
  eid_draw_line(fb, WIN_W, WIN_H, 0, 65, WIN_W, 65, CLR_BORDER);

  int v_lines = visible_lines();
  int max_scroll = line_count - v_lines;
  if (max_scroll < 0)
    max_scroll = 0;
  if (scroll_line > max_scroll)
    scroll_line = max_scroll;

  /* R6/L1: pixel-anchored render. Each line carries its own
   * (box_x, box_y, box_h) so we no longer accumulate cur_y per
   * style; instead we project box_y onto the screen via a single
   * scroll offset.
   *
   * scroll_line is still in line units (it indexes the first
   * visible line); we translate it to pixels by reading that
   * line's box_y. Lines with box_y < scroll_y_px are clipped
   * out (above the fold); lines whose top is past content_bottom
   * are clipped out (below the fold). */
  const int content_top    = CONTENT_Y + 14;
  const int content_bottom = WIN_H - 18;
  int scroll_y_px = 0;
  if (scroll_line > 0 && scroll_line < line_count)
    scroll_y_px = lines[scroll_line].box_y;
  /* Compatibility alias for branches further down that still
   * advance a `cur_y` of their own (notably the link
   * eid_process_interaction rect below). */
  int cur_y = content_top;
  (void)cur_y;
  for (int i = 0; i < v_lines; i++) {
    int idx = scroll_line + i;
    if (idx >= line_count)
      break;
    /* cur_y for this line, derived from box_y rather than a
     * running accumulator. */
    int line_y = content_top + lines[idx].box_y - scroll_y_px;
    cur_y = line_y;
    int line_h = lines[idx].box_h > 0 ? lines[idx].box_h : LINE_H;
    if (line_y + line_h > content_bottom)
      break;

    /* box_x already includes the LAYOUT_DEFAULT_INDENT offset for
     * indented lines (see push_line). Don't add it again. */
    int cur_x = CONTENT_X + lines[idx].box_x;

#ifdef BROWSER_BUILD
    /* R4/F0: <button> widgets render as eid buttons and dispatch a
     * 'click' event into the page JS session when clicked. */
    if (lines[idx].style == STYLE_BUTTON && lines[idx].widget_node) {
      int btn_w = eid_text_width_utf8(lines[idx].text) + 24;
      if (btn_w < 80)  btn_w = 80;
      if (btn_w > CONTENT_W) btn_w = CONTENT_W;
      int btn_h = LINE_H + 4;
      uint32_t state = eid_button(&ui, lines[idx].text,
                                  cur_x, cur_y - 2, btn_w, btn_h);
      if (state & EID_STATE_CLICKED) {
        dom_node_t *wn = (dom_node_t *)lines[idx].widget_node;
        int click_prevented = 0;
        if (g_page) {
          click_prevented = qjs_page_dispatch_event(g_page, wn, "click");
          /* R5/N0+N1: handler may have navigated via location.* or
           * history.*; if so, act on it and bail this frame. */
          if (drain_pending_nav()) return;
          if (qjs_page_consume_dirty(g_page)) {
            rebuild_lines_from_dom();
            /* Don't continue painting against the now-stale loop —
             * next frame paints the fresh tree. */
            return;
          }
        }
        /* R4/F2: after the script handlers (if any) had their say,
         * a submit-type widget navigates the enclosing form. R4/F3:
         * unless the click was preventDefault'd, in which case neither
         * the click default nor the implicit submit fires. */
        if (!click_prevented && is_submit_widget(wn)) {
          submit_form_for(wn);
          return;
        }
      }
      cur_y += LINE_H + 6;
      continue;
    }
    /* R4/F1: <input type=text> widgets. The DOM `value` attribute
     * holds the canonical text; we copy it into a local buffer,
     * hand that to eid_text_input (which mutates it based on
     * focused keyboard input), then if the buffer changed we
     * write it back to the DOM and dispatch a synthetic 'input'
     * event into JS. We don't trigger rebuild_lines_from_dom on
     * every keystroke — only if JS itself mutated the DOM in
     * response (qjs_page_consume_dirty). The input line's own
     * `text[]` field re-syncs on the next full rebuild. */
    if (lines[idx].style == STYLE_INPUT && lines[idx].widget_node) {
      dom_node_t *n = (dom_node_t *)lines[idx].widget_node;
      int in_w = CONTENT_W - 4;
      if (in_w > 360) in_w = 360;
      /* 8x16 PSF1 font ⇒ make the box just tall enough that the
       * glyphs sit centred with ~2 px breathing room on each side.
       * LINE_H+4 left visible vertical padding because the page-line
       * advance is only LINE_H+6 below, which made the input look
       * like it cropped the text. */
      int in_h = 20;

      char buf[LINE_CHARS + 1];
      const char *cur_val = dom_get_attr(n, "value");
      int  bn = 0;
      if (cur_val) {
        for (const char *t = cur_val; *t && bn < LINE_CHARS; t++) buf[bn++] = *t;
      }
      buf[bn] = 0;

      /* Use the node pointer as part of the eid label so two empty
       * inputs at the same column don't share an ID. */
      char id_label[24];
      sprintf(id_label, "input@%p", (void *)n);

      /* R4/F2: snapshot last_key BEFORE eid_text_input — it consumes
       * the key while focused, including Enter (which scancode_to_ascii
       * maps to '\n' but eid_text_input filters as non-printable).
       * If we saw Enter and the widget ends up focused, treat it as
       * "submit the enclosing form". */
      uint8_t key_before = ui.last_key;

      uint32_t in_state = eid_text_input(&ui, id_label, cur_x, cur_y - 2,
                                         in_w, in_h, buf, (int)sizeof buf);

      if (!cur_val || strcmp(buf, cur_val) != 0) {
        dom_set_attr(n, "value", buf);
        if (g_page) {
          qjs_page_dispatch_event(g_page, n, "input");
          if (drain_pending_nav()) return;
          if (qjs_page_consume_dirty(g_page)) {
            rebuild_lines_from_dom();
            return;
          }
        }
      }
      if ((in_state & EID_STATE_FOCUSED) && key_before == 0x1C) {
        /* Enter while focused → submit enclosing form (if any). */
        submit_form_for(n);
        return;
      }

      /* R5/N2: emit a `change` event when focus leaves the input
       * and the value differs from the snapshot taken at focus-in. */
      bool now_focused = (in_state & EID_STATE_FOCUSED) != 0;
      if (now_focused && focus_input_node != n) {
        focus_input_node = n;
        strncpy(focus_input_snapshot, buf, sizeof(focus_input_snapshot) - 1);
        focus_input_snapshot[sizeof(focus_input_snapshot) - 1] = 0;
      } else if (!now_focused && focus_input_node == n) {
        if (strcmp(buf, focus_input_snapshot) != 0 && g_page) {
          qjs_page_dispatch_event(g_page, n, "change");
          focus_input_node = NULL;
          focus_input_snapshot[0] = 0;
          if (drain_pending_nav()) return;
          if (qjs_page_consume_dirty(g_page)) {
            rebuild_lines_from_dom();
            return;
          }
        } else {
          focus_input_node = NULL;
          focus_input_snapshot[0] = 0;
        }
      }
      cur_y += LINE_H + 6;
      continue;
    }
    /* R5/N2: checkbox / radio toggle. */
    if (lines[idx].style == STYLE_CHECKBOX && lines[idx].widget_node) {
      dom_node_t *n = (dom_node_t *)lines[idx].widget_node;
      const char *type = dom_get_attr(n, "type");
      bool is_radio = type && strcasecmp(type, "radio") == 0;
      bool checked  = dom_get_attr(n, "checked") != NULL;
      bool prev     = checked;

      /* eid_checkbox renders `label` as visible text next to the
       * box. We want the surrounding HTML text (typically a sibling
       * <label> or trailing text node) to provide the visible
       * caption, so pass an empty string here. The id is derived
       * from (label, x, y) — different checkboxes naturally get
       * different ids via their y coordinate. */
      eid_checkbox(&ui, "", cur_x, cur_y - 2, &checked);

      if (checked != prev) {
        bool fire_change = true;
        if (is_radio) {
          if (checked) {
            /* Newly selected radio: uncheck siblings, persist. */
            uncheck_radio_siblings(n);
            dom_set_attr(n, "checked", "");
          } else {
            /* Radio can't be untoggled by a second click — restore
             * and suppress the synthetic change. */
            dom_set_attr(n, "checked", "");
            fire_change = false;
          }
        } else {
          if (checked) dom_set_attr(n, "checked", "");
          else         dom_remove_attr(n, "checked");
        }
        if (fire_change && g_page) {
          qjs_page_dispatch_event(g_page, n, "click");
          qjs_page_dispatch_event(g_page, n, "change");
          if (drain_pending_nav()) return;
          if (qjs_page_consume_dirty(g_page)) {
            rebuild_lines_from_dom();
            return;
          }
        }
      }
      cur_y += LINE_H + 6;
      continue;
    }
    /* R5/N2: <select> — click cycles to the next option. */
    if (lines[idx].style == STYLE_SELECT && lines[idx].widget_node) {
      dom_node_t *n = (dom_node_t *)lines[idx].widget_node;
      char btn_label[LINE_CHARS + 4];
      int  ll = (int)strlen(lines[idx].text);
      if (ll > LINE_CHARS) ll = LINE_CHARS;
      memcpy(btn_label, lines[idx].text, ll);
      /* Append " v" so it visually reads as a dropdown. */
      btn_label[ll++] = ' ';
      btn_label[ll++] = 'v';
      btn_label[ll]   = 0;

      int btn_w = ll * 8 + 24;
      if (btn_w < 100) btn_w = 100;
      if (btn_w > CONTENT_W) btn_w = CONTENT_W;
      int btn_h = LINE_H + 4;
      uint32_t state = eid_button(&ui, btn_label, cur_x, cur_y - 2, btn_w, btn_h);
      if (state & EID_STATE_CLICKED) {
        const char *cur_v = dom_get_attr(n, "value");
        dom_node_t *cur_opt = find_option_by_value(n, cur_v);
        dom_node_t *nxt = next_option(n, cur_opt);
        if (nxt && nxt != cur_opt) {
          const char *nv = dom_get_attr(nxt, "value");
          char otxt[64]; int ol = 0;
          for (dom_node_t *t = nxt->first_child; t && ol < (int)sizeof(otxt) - 1; t = t->next_sibling) {
            if (t->type == DOM_NODE_TEXT && t->text) {
              for (const char *s = t->text; *s && ol < (int)sizeof(otxt) - 1; s++) otxt[ol++] = *s;
            }
          }
          otxt[ol] = 0;
          dom_set_attr(n, "value", (nv && nv[0]) ? nv : otxt);
          /* Keep DOM in sync for scripts that inspect option.selected. */
          for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
            if (c->type == DOM_NODE_ELEMENT && c->tag_name &&
                strcmp(c->tag_name, "option") == 0) dom_remove_attr(c, "selected");
          }
          dom_set_attr(nxt, "selected", "");
          /* Rebuild lines so the visible label reflects the new
           * selection right away. */
          rebuild_lines_from_dom();
          if (g_page) {
            qjs_page_dispatch_event(g_page, n, "change");
            if (drain_pending_nav()) return;
            if (qjs_page_consume_dirty(g_page)) {
              rebuild_lines_from_dom();
            }
          }
          return;
        }
      }
      cur_y += LINE_H + 6;
      continue;
    }
#endif

    /* R6/B4: image lines take their natural height from the decoded
     * image rather than the fixed LINE_H. Blit directly into the
     * framebuffer and skip the rest of the per-line tail. */
#ifdef BROWSER_BUILD
    if (lines[idx].style == STYLE_IMAGE) {
      int ii = lines[idx].image_idx;
      if (ii >= 0 && ii < g_image_count && g_images[ii].rgba) {
        const eq_image_t *im = &g_images[ii];
        int draw_x = CONTENT_X;
        int draw_y = cur_y;
        /* Honour center alignment if the parent set it (e.g. an
         * <img> inside a <center> or text-align:center container). */
        if (lines[idx].css_align == ALIGN_CENTER &&
            im->w < CONTENT_W)
          draw_x = CONTENT_X + (CONTENT_W - im->w) / 2;

        /* R6/B4 follow-up: paint a soft-grey "card" behind the
         * image first, then alpha-composite the pixels on top.
         * Without this a transparent PNG logo whose foreground is
         * pure white (very common — site logos targeting dark
         * headers) disappears completely on our white page
         * background. The card gives those white pixels something
         * to contrast against, and the proper alpha blend keeps
         * antialiased edges smooth instead of speckled. */
        const uint32_t CLR_IMG_CARD = 0xF1F3F4;  /* Google grey 50 */
        const int      PAD          = 6;
        eid_draw_rect(fb, WIN_W, WIN_H,
                      draw_x - PAD, draw_y - PAD,
                      im->w + PAD * 2, im->h + PAD * 2, CLR_IMG_CARD);

        for (int py = 0; py < im->h; py++) {
          int fy = draw_y + py;
          if (fy < 0 || fy >= WIN_H - 18) continue; /* clip status bar */
          const uint8_t *src = im->rgba + (size_t)py * im->w * 4;
          uint32_t      *dst = fb + (size_t)fy * WIN_W + draw_x;
          for (int px = 0; px < im->w; px++) {
            int fx = draw_x + px;
            if (fx < 0 || fx >= WIN_W) { dst++; src += 4; continue; }
            uint32_t a = src[3];
            if (a == 0) { dst++; src += 4; continue; }
            if (a == 255) {
              *dst = ((uint32_t)src[0] << 16) |
                     ((uint32_t)src[1] <<  8) |
                     ((uint32_t)src[2]);
            } else {
              /* source-over over the already-drawn card pixel */
              uint32_t bg = *dst;
              uint32_t br = (bg >> 16) & 0xFF;
              uint32_t bgc= (bg >>  8) & 0xFF;
              uint32_t bb = (bg      ) & 0xFF;
              uint32_t r  = (src[0] * a + br * (255 - a)) / 255;
              uint32_t g  = (src[1] * a + bgc* (255 - a)) / 255;
              uint32_t b  = (src[2] * a + bb * (255 - a)) / 255;
              *dst = (r << 16) | (g << 8) | b;
            }
            dst++; src += 4;
          }
        }
        cur_y += im->h + PAD * 2 + 6;
      } else {
        cur_y += LINE_H;
      }
      continue;
    }
#endif

    draw_text_line(cur_x, cur_y, &lines[idx]);

    /* Handle link interaction */
    if (lines[idx].style == STYLE_LINK && lines[idx].link_url[0]) {
      uint32_t id = eid_get_id(lines[idx].link_url, cur_x, cur_y);
      uint32_t state = eid_process_interaction(
          &ui, id, cur_x, cur_y, eid_text_width_utf8(lines[idx].text), LINE_H);
      if (state & EID_STATE_CLICKED) {
        char resolved[128];
        resolve_url(current_url, lines[idx].link_url, resolved);
        strcpy(current_url, resolved);
        load_page(current_url);
        return; /* Avoid drawing more in this frame */
      }
    }

    cur_y += LINE_H;
  }

  /* Scrollbar */
  int scroll_track_y = 66;
  int scroll_track_h = WIN_H - scroll_track_y - 20;
  eid_draw_rect(fb, WIN_W, WIN_H, WIN_W - 10, scroll_track_y, 4, scroll_track_h,
                0xE8EAED);

  if (line_count > v_lines) {
    int knob_h = (v_lines * scroll_track_h) / line_count;
    if (knob_h < 12)
      knob_h = 12;

    int denom = (max_scroll > 0) ? max_scroll : 1;
    int knob_y =
        scroll_track_y + (scroll_line * (scroll_track_h - knob_h)) / denom;

    eid_draw_rect(fb, WIN_W, WIN_H, WIN_W - 10, knob_y, 4, knob_h, 0x9AA0A6);
  }

  /* Status Bar */
  eid_draw_rect(fb, WIN_W, WIN_H, 0, WIN_H - 18, WIN_W, 18, 0xF1F3F4);
  eid_draw_line(fb, WIN_W, WIN_H, 0, WIN_H - 18, WIN_W, WIN_H - 18, CLR_BORDER);
  eid_draw_text(fb, WIN_W, WIN_H, CONTENT_X, WIN_H - 14,
                "L: Edit URL  Up/Down: Scroll  Esc: Exit", CLR_MUTED);

  /* Software cursor.
   *
   * When the app calls SYS_DRAW_BUFFER it becomes the foreground app
   * (see src/kernel.c sys_draw_app_buffer), and the kernel routes
   * SYS_GET_MOUSE_FULL exclusively to it — sysgui's compositor stops
   * receiving mouse coords, so its hardware-emulated cursor sprite
   * goes stale / invisible inside our window. Draw our own arrow at
   * the local mouse position so the user can aim at widgets. The
   * coords in ui.mx/ui.my are already in window-local space (the
   * main loop subtracts the window offset above). Clip to the
   * window rect — eid_draw_rect/line already do bounds checks. */
  {
    int cx = ui.mx;
    int cy = ui.my;
    if (cx >= 0 && cx < WIN_W && cy >= 0 && cy < WIN_H) {
      /* 12 px L-shaped arrow, white fill + 1 px black outline. */
      for (int i = 0; i < 12; i++) {
        int w = 12 - i;            /* triangle row width */
        eid_draw_rect(fb, WIN_W, WIN_H, cx,         cy + i, 1, 1, 0x000000);
        eid_draw_rect(fb, WIN_W, WIN_H, cx + 1,     cy + i, w - 2 > 0 ? w - 2 : 0, 1, 0xFFFFFF);
        eid_draw_rect(fb, WIN_W, WIN_H, cx + w - 1, cy + i, 1, 1, 0x000000);
      }
      /* bottom edge */
      eid_draw_line(fb, WIN_W, WIN_H, cx, cy + 12, cx + 6, cy + 12, 0x000000);
    }
  }
}

#ifdef BROWSER_BUILD
/* ----------------------------------------------------------------------- *
 * BROWSER_BUILD load_page() — used by browser.elf (phase 6).
 *
 * Goes through the proper phase-5 HTTP/HTTPS client. Handles both http://
 * and https:// URLs end-to-end with redirect following; falls back to the
 * SYS_READ_FILE path for argv[1]s that aren't URLs (e.g. local *.html in
 * iso_root/res). Replaces the legacy net_http_get() path inline below.
 * ----------------------------------------------------------------------- */
#include <http_client.h>
#include <url.h>
#include "../third_party/ca_bundle/ca_bundle.h"

/* Optional IP override applied to the FIRST eq_http_get() call only.
 * Convenient for working around the flaky QEMU SLIRP DNS proxy: pass a
 * dotted-quad IP as argv[2] (`run bin/browser.elf <url> <ip>`). Subsequent
 * navigations (via L: edit URL or clicked links) go through normal DNS.
 *
 * Stored in network byte order, zero = unused. */
static uint32_t g_first_load_ip_override_be = 0;

/* ── Phase R6/B4: <img> fetch + decode ───────────────────────────────
 * Called from w_emit_node() while the DOM is being walked into the
 * line stream. We block here on the network for the duration of one
 * GET — that's OK for the kind of toy pages this browser is meant
 * to render and keeps the renderer single-threaded. */
static int load_image_for_src(const char *src) {
  if (!src || !src[0]) return -1;
  if (g_image_count >= MAX_DOC_IMAGES) return -1;

  /* Resolve against current_url so site-relative paths like
   * "/static/logo.png" become absolute. Local file:// style paths
   * (anything that isn't http(s)://) fall back to SYS_READ_FILE. */
  char resolved[256];
  resolve_url(current_url, src, resolved);

  uint8_t  *bytes  = NULL;
  size_t    nbytes = 0;
  eq_http_response_t resp;
  memset(&resp, 0, sizeof resp);

  if (strncasecmp(resolved, "http://",  7) == 0 ||
      strncasecmp(resolved, "https://", 8) == 0) {
    eq_http_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.trust_anchors     = TAs_MOZ;
    opts.trust_anchors_num = TAs_MOZ_NUM;
    opts.follow_redirects  = 3;
    opts.recv_timeout_ms   = 10000;
    opts.body_limit_bytes  = 1u * 1024u * 1024u;   /* 1 MiB cap */
    opts.verbose           = 0;
    int rc = eq_http_get(resolved, &opts, &resp);
    if (rc != EQ_HTTP_OK || resp.status_code / 100 != 2 ||
        !resp.body || resp.body_len == 0) {
      eq_http_response_free(&resp);
      return -1;
    }
    bytes  = (uint8_t *)resp.body;
    nbytes = resp.body_len;
  } else {
    /* Local resource (iso_root/res/foo.png or similar). Strip any
     * accidental query string. */
    char fpath[128];
    int fl = 0;
    for (const char *t = resolved; *t && *t != '?' &&
                                   fl < (int)sizeof fpath - 1; t++)
      fpath[fl++] = *t;
    fpath[fl] = 0;
    uint32_t fsize = 0;
    void *data = (void *)_syscall(SYS_READ_FILE, (uint64_t)fpath,
                                  (uint64_t)&fsize, 0, 0, 0);
    if (!data || fsize == 0) return -1;
    bytes  = (uint8_t *)data;
    nbytes = fsize;
  }

  eq_image_t img = {0};
  int rc = eq_image_decode(bytes, nbytes, &img);
  /* Drop the original encoded bytes immediately — the decoder
   * already produced an owned RGBA copy. */
  eq_http_response_free(&resp);
  if (rc != 0 || !img.rgba) return -1;

  /* Downscale by 2x repeatedly until it fits the content area. We
   * cap the on-screen height at 320 px so a single hero image
   * doesn't push the entire page below the fold. Nearest-neighbour
   * halving in-place — sufficient for line art and screenshots and
   * keeps the code one tight loop. */
  const int max_w = CONTENT_W;
  const int max_h = 320;
  while ((img.w > max_w || img.h > max_h) && img.w > 1 && img.h > 1) {
    int nw = img.w / 2; if (nw < 1) nw = 1;
    int nh = img.h / 2; if (nh < 1) nh = 1;
    uint8_t *dst = (uint8_t *)malloc((size_t)nw * nh * 4);
    if (!dst) break;
    for (int y = 0; y < nh; y++) {
      const uint8_t *srow = img.rgba + (size_t)(y * 2) * img.w * 4;
      uint8_t       *drow = dst       + (size_t) y      * nw   * 4;
      for (int x = 0; x < nw; x++) {
        const uint8_t *sp = srow + (size_t)(x * 2) * 4;
        drow[x * 4 + 0] = sp[0];
        drow[x * 4 + 1] = sp[1];
        drow[x * 4 + 2] = sp[2];
        drow[x * 4 + 3] = sp[3];
      }
    }
    eq_image_free(&img);
    img.rgba = dst; img.w = nw; img.h = nh;
  }

  int idx = g_image_count++;
  g_images[idx] = img;
  return idx;
}

static void load_page(const char *url) {
  print("[BROWSER] Navigating to: ");
  print(url);
  print("\n");

  /* --- File path (local resource) -------------------------------- */
  if (strncasecmp(url, "http://", 7) != 0 &&
      strncasecmp(url, "https://", 8) != 0) {
    /* Form submission can append `?key=val` to a local-file action
     * (R4/F2). The filesystem doesn't know what to do with the query,
     * so strip it before SYS_READ_FILE while leaving `current_url`
     * unchanged (so back/forward replay the same path). */
    char fpath[128];
    int fl = 0;
    for (const char *t = url; *t && *t != '?' && fl < (int)sizeof fpath - 1; t++)
      fpath[fl++] = *t;
    fpath[fl] = 0;
    uint32_t fsize = 0;
    char *data = (char *)_syscall(SYS_READ_FILE, (uint64_t)fpath,
                                  (uint64_t)&fsize, 0, 0, 0);
    if (!data) {
      line_count = 0;
      push_line("File not found", 14, STYLE_H1, false);
      push_line(url, strlen(url), STYLE_MUTED, false);
      return;
    }
    if (!is_navigating_history) push_history(url);
    parse_html(data, fsize);
    return;
  }

  /* --- HTTP/HTTPS via the phase-5 client library ----------------- */
  eq_http_options_t opts;
  memset(&opts, 0, sizeof opts);
  opts.trust_anchors     = TAs_MOZ;
  opts.trust_anchors_num = TAs_MOZ_NUM;
  opts.follow_redirects  = 5;
  opts.recv_timeout_ms   = 20000;
  opts.body_limit_bytes  = 1u * 1024u * 1024u;
  opts.verbose           = 1;
  if (g_first_load_ip_override_be) {
    opts.ip_override_be = g_first_load_ip_override_be;
    g_first_load_ip_override_be = 0;  /* one-shot: only the boot URL */
  }

  eq_http_response_t resp;
  memset(&resp, 0, sizeof resp);
  int rc = eq_http_get(url, &opts, &resp);

  if (rc != EQ_HTTP_OK || !resp.body || resp.body_len == 0) {
    line_count = 0;
    char errbuf[80];
    sprintf(errbuf, "Network error  rc=%d  http=%d  tls=%d",
            rc, resp.status_code, resp.tls_last_err);
    push_line(errbuf, strlen(errbuf), STYLE_H1, false);
    push_line(url, strlen(url), STYLE_MUTED, false);
    eq_http_response_free(&resp);
    return;
  }

  if (!is_navigating_history) push_history(url);

  /* If the server redirected us, surface the final URL in the address
   * bar so subsequent relative links resolve correctly. The URL field
   * is sized at 127 bytes — truncate quietly rather than overflow. */
  if (resp.final_url && resp.redirects_followed > 0) {
    size_t flen = strlen(resp.final_url);
    if (flen >= sizeof current_url) flen = sizeof current_url - 1;
    memcpy(current_url, resp.final_url, flen);
    current_url[flen] = '\0';
  }

  parse_html(resp.body, (uint32_t)resp.body_len);
  eq_http_response_free(&resp);
}

#else  /* !BROWSER_BUILD — original htmlview load_page() */

static void load_page(const char *url) {
  print("[BROWSER] Navigating to: ");
  print(url);
  print("\n");

  char *html = NULL;
  uint32_t size = 0;

  /* Treat as URL only if there's an explicit scheme — the old
   * heuristic ("contains a dot") fired on "index.html" and other
   * local files, sending them through DNS. Pure-local htmlview.elf
   * never has a use for implicit HTTP, so require "://". */
  bool is_url = (strstr(url, "://") != NULL);
  if (is_url) {
    const char *host = url;
    if (strncmp(url, "http://", 7) == 0)
      host += 7;

    char hostname[64];
    int i = 0;
    while (host[i] && host[i] != '/' && i < 63) {
      hostname[i] = host[i];
      i++;
    }
    hostname[i] = '\0';

    print("[BROWSER] Resolving ");
    print(hostname);
    print("...\n");

    uint32_t ip = net_dns_resolve(hostname);
    if (ip == 0) {
      line_count = 0;
      push_line("DNS Resolution Failed", 21, STYLE_H1, false);
      push_line(hostname, strlen(hostname), STYLE_MUTED, false);
      return;
    }

    print("[BROWSER] Fetching from IP...\n");
    html = (char *)net_http_get(ip, &size);
  } else {
    html = (char *)_syscall(SYS_READ_FILE, (uint64_t)url, (uint64_t)&size, 0, 0,
                            0);
  }

  if (!html) {
    line_count = 0;
    push_line("404 Not Found / Connection Failed", 33, STYLE_H1, false);
    push_line(url, strlen(url), STYLE_MUTED, false);
  } else {
    if (!is_navigating_history) {
      push_history(url);
    }
    char *body = strstr(html, "\r\n\r\n");
    if (body) {
      body += 4;
      parse_html(body, size - (body - html));
    } else {
      parse_html(html, size);
    }
  }
}

#endif /* BROWSER_BUILD */

int main(int argc, char **argv) {
  eid_init();

  /* Load header font */
  uint32_t fsize = 0;
  void *f_data = (void *)_syscall(SYS_READ_FILE, (uint64_t)"Inter.ttf",
                                  (uint64_t)&fsize, 0, 0, 0);
  if (f_data) {
    h_font = eid_load_font((unsigned char *)f_data, 16.0f);
    h_font_large = eid_load_font((unsigned char *)f_data, 22.0f);
  }

  /* Scan argv for the global --legacy flag (compact pre-pass so the
   * existing positional argv[1]/argv[2] semantics are preserved). */
  int positional[8]; int p_count = 0;
  for (int ai = 1; ai < argc && p_count < 8; ai++) {
    if (argv[ai] && strcmp(argv[ai], "--legacy") == 0) {
      g_use_legacy_parser = true;
    } else {
      positional[p_count++] = ai;
    }
  }
  if (p_count > 0) {
    char *arg1 = argv[positional[0]];
    size_t alen = strlen(arg1);
    if (alen >= sizeof current_url) alen = sizeof current_url - 1;
    memcpy(current_url, arg1, alen);
    current_url[alen] = '\0';
  }
#ifdef BROWSER_BUILD
  if (p_count == 0) {
    /* browser.elf defaults to a real internet page; htmlview.elf keeps
     * its original "index.html" local-file default. */
    strcpy(current_url, "http://example.com/");
  }
  /* Optional 2nd positional arg: dotted-quad IP override for the first
   * load_page() (same convention as urlget — useful while QEMU SLIRP
   * DNS is flaky). */
  if (p_count > 1 && argv[positional[1]] != 0) {
    uint32_t ip_be = net_dns_resolve(argv[positional[1]]);
    if (ip_be) g_first_load_ip_override_be = ip_be;
  }
#endif

  for (int i = 0; i < WIN_W * WIN_H; i++)
    fb[i] = CLR_BG;

  load_page(current_url);

  while (1) {
    eid_begin(&ui, fb, WIN_W, WIN_H);
    ui.mx -= 120;
    ui.my -= 90;

    uint8_t key = ui.last_key;
    int max_scroll = line_count - visible_lines();
    if (max_scroll < 0)
      max_scroll = 0;

    /* Esc is reserved as the universal "quit the browser" hotkey and
     * is NOT exposed to JS — otherwise a runaway handler could trap
     * the user inside a page. Check it before the keydown dispatch
     * so preventDefault() can never block exit. */
    if (key == 0x01)
      break;

#ifdef BROWSER_BUILD
    /* R5/N5: per-frame timer tick. setInterval / setTimeout drive UI
     * animation now that the host clock is plumbed in. Any DOM
     * mutations from a timer callback get picked up immediately. */
    if (g_page) {
      uint64_t now_ms = _syscall(SYS_GET_TIME, 0, 0, 0, 0, 0);
      qjs_page_tick(g_page, now_ms);
      if (drain_pending_nav()) return 0;
      if (qjs_page_consume_dirty(g_page)) {
        rebuild_lines_from_dom();
        max_scroll = line_count - visible_lines();
        if (max_scroll < 0) max_scroll = 0;
      }
    }

    /* R5/N5: dispatch keydown / keyup to a focused <input>. PS/2
     * make codes are 0x01..0x80, break codes are 0x80+make. We
     * report keyCode = scancode and key = scancode_to_ascii(make).
     * If a handler preventDefault()'s, swallow the key so the
     * default input widget doesn't also consume it. */
    if (g_page && focus_input_node && key != 0 &&
        key != 0x2A && key != 0x36 && key != 0xAA && key != 0xB6) {
      bool is_break = (key & 0x80) != 0;
      uint8_t make  = (uint8_t)(key & 0x7F);
      char ascii    = scancode_to_ascii(make);
      char keystr[2] = { ascii ? ascii : ' ', 0 };
      const char *evname = is_break ? "keyup" : "keydown";
      int prevented = qjs_page_dispatch_key(g_page, focus_input_node,
                                            evname, keystr, (int)make);
      if (drain_pending_nav()) return 0;
      if (qjs_page_consume_dirty(g_page)) {
        rebuild_lines_from_dom();
      }
      if (prevented) {
        /* Hide the key from every widget this frame. */
        ui.last_key = 0;
        key = 0;
      }
    }
#endif

    /* Track Shift across frames so typing ':', '/', '?', '#', etc. in the
     * URL bar works. The PS/2 driver delivers both make and break codes
     * via the ring buffer; we just watch for 0x2A/0x36 (Shift down) and
     * 0xAA/0xB6 (Shift up). */
    static bool shift_held = false;
    if (key == 0x2A || key == 0x36) shift_held = true;
    else if (key == 0xAA || key == 0xB6) shift_held = false;

    if (is_typing_url) {
      if (key == 0x1C) {
        is_typing_url = false;
        load_page(current_url);
      } else if (key == 0x0E) {
        if (url_cursor > 0) {
          url_cursor--;
          current_url[url_cursor] = '\0';
        }
      } else {
        /* Shifted symbols needed for URLs: ':' = Shift+';', '?' = Shift+'/',
         * '#' = Shift+'3', '&' = Shift+'7', '=' is unshifted. Cover the
         * full upper-row remap inline. */
        char c = scancode_to_ascii(key);
        if (shift_held && c) {
          static const char shift_map[128] = {
            [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
            [0x07]='^', [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')',
            [0x0C]='_', [0x0D]='+',
            [0x10]='Q', [0x11]='W', [0x12]='E', [0x13]='R', [0x14]='T',
            [0x15]='Y', [0x16]='U', [0x17]='I', [0x18]='O', [0x19]='P',
            [0x1A]='{', [0x1B]='}',
            [0x1E]='A', [0x1F]='S', [0x20]='D', [0x21]='F', [0x22]='G',
            [0x23]='H', [0x24]='J', [0x25]='K', [0x26]='L',
            [0x27]=':', [0x28]='"', [0x29]='~', [0x2B]='|',
            [0x2C]='Z', [0x2D]='X', [0x2E]='C', [0x2F]='V', [0x30]='B',
            [0x31]='N', [0x32]='M',
            [0x33]='<', [0x34]='>', [0x35]='?',
          };
          if (key < sizeof shift_map && shift_map[key])
            c = shift_map[key];
        }
        if (c >= 32 && c < 127 && url_cursor < (int)(sizeof current_url - 1)) {
          current_url[url_cursor++] = c;
          current_url[url_cursor] = '\0';
        }
      }
    } else {
      /* R5/N2: if a text <input> currently has focus, drop the global
       * shortcuts — the input widget will consume the keystroke this
       * frame. Otherwise typing 'l' / 'j' / 'k' inside an input would
       * fire L=Edit URL / J=ScrollDown / K=ScrollUp first. */
#ifdef BROWSER_BUILD
      bool input_focused = (focus_input_node != NULL);
#else
      bool input_focused = false;
#endif
      if (!input_focused && key == 0x26) {
        is_typing_url = true;
        url_cursor = 0;
        current_url[0] = '\0';
      }
      if (!input_focused && (key == 0x50 || key == 0x1F) && scroll_line < max_scroll)
        scroll_line++;
      if (!input_focused && (key == 0x48 || key == 0x11) && scroll_line > 0)
        scroll_line--;
      if (!input_focused && key == 0x51) {
        scroll_line += visible_lines();
        if (scroll_line > max_scroll)
          scroll_line = max_scroll;
      }
      if (!input_focused && key == 0x49) {
        scroll_line -= visible_lines();
        if (scroll_line < 0)
          scroll_line = 0;
      }
    }

    render(current_url);
    eid_end(&ui, 120, 90);
    sleep(20);
  }

  _syscall(SYS_EXIT, 0, 0, 0, 0, 0);
  return 0;
}
