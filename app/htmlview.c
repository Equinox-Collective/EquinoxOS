#include <eid.h>
#include <equos.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WIN_W 640
#define WIN_H 420
#define CONTENT_X 18
#define CONTENT_Y 56
#define CONTENT_W (WIN_W - 36)
#define LINE_H 18
#define MAX_LINES 4096
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

/* ── CSS Engine ────────────────────────────────────────────────── */

#define MAX_CSS_RULES 512
#define MAX_SELECTOR 256
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
  /* Layout — set by `display:grid|flex` and
   * `grid-template-columns:repeat(N,...)` (or `1fr 1fr ...`). When
   * this element is rendered, its direct children get laid out
   * side-by-side, each owning a 1/N slice of the content area. */
  int grid_cols;  /* 0 = not a grid/flex container, N = N columns */
} css_rule_t;

static css_rule_t css_rules[MAX_CSS_RULES];
static int css_rule_count = 0;
static uint32_t body_bg = CLR_BG;

/* ── CSS custom properties (variables) ──────────────────────────
 * Captured from any rule whose declaration block contains
 * `--name: value;`. Most sites put them in `:root { --bg:#... }`,
 * but we record them from any selector — at this level we don't
 * scope by selector, we just need to know what `var(--foo)`
 * resolves to so that the rest of the parser sees a real value.
 *
 * Names are stored without the leading "--".
 */
#define MAX_CSS_VARS 128
#define CSS_VAR_NAME_LEN 48
#define CSS_VAR_VALUE_LEN 96
typedef struct {
  char name[CSS_VAR_NAME_LEN];
  char value[CSS_VAR_VALUE_LEN];
} css_var_t;
static css_var_t css_vars[MAX_CSS_VARS];
static int css_var_count = 0;

static const char *css_var_lookup(const char *name, int name_len) {
  for (int i = 0; i < css_var_count; i++) {
    if ((int)strlen(css_vars[i].name) == name_len &&
        strncmp(css_vars[i].name, name, name_len) == 0)
      return css_vars[i].value;
  }
  return NULL;
}

static void css_var_define(const char *name, int name_len, const char *value) {
  if (name_len <= 0 || name_len >= CSS_VAR_NAME_LEN)
    return;
  /* Replace if already defined */
  for (int i = 0; i < css_var_count; i++) {
    if ((int)strlen(css_vars[i].name) == name_len &&
        strncmp(css_vars[i].name, name, name_len) == 0) {
      strncpy(css_vars[i].value, value, CSS_VAR_VALUE_LEN - 1);
      css_vars[i].value[CSS_VAR_VALUE_LEN - 1] = '\0';
      return;
    }
  }
  if (css_var_count >= MAX_CSS_VARS)
    return;
  strncpy(css_vars[css_var_count].name, name, name_len);
  css_vars[css_var_count].name[name_len] = '\0';
  strncpy(css_vars[css_var_count].value, value, CSS_VAR_VALUE_LEN - 1);
  css_vars[css_var_count].value[CSS_VAR_VALUE_LEN - 1] = '\0';
  css_var_count++;
}

/* Substitute every occurrence of `var(--name)` in `val` with the
 * value previously stored via css_var_define(). Unknown vars are
 * left as-is so downstream parsers can still spot them. Output is
 * written to `out` (size `out_size`) and may be the same memory as
 * `val`, since we copy through a small bounce buffer.
 *
 * Only handles one level of indirection — chained vars
 * (var(--a)  where --a: var(--b)) need a second pass; we don't
 * bother today, the site doesn't use it. */
static void css_resolve_vars(const char *val, char *out, int out_size) {
  int oi = 0;
  for (int i = 0; val[i] && oi < out_size - 1;) {
    if (val[i] == 'v' && val[i + 1] == 'a' && val[i + 2] == 'r' &&
        val[i + 3] == '(') {
      const char *p = val + i + 4;
      while (*p == ' ')
        p++;
      if (p[0] == '-' && p[1] == '-') {
        p += 2;
        const char *name_start = p;
        while (*p && *p != ')' && *p != ',' && *p != ' ')
          p++;
        int name_len = (int)(p - name_start);
        /* Skip optional fallback ", value" */
        const char *q = p;
        while (*q && *q != ')')
          q++;
        if (*q == ')') {
          const char *resolved = css_var_lookup(name_start, name_len);
          if (resolved) {
            for (int k = 0; resolved[k] && oi < out_size - 1; k++)
              out[oi++] = resolved[k];
            i = (int)(q - val) + 1;
            continue;
          }
        }
      }
    }
    out[oi++] = val[i++];
  }
  out[oi] = '\0';
}

/* ── Line model (extended) ─────────────────────────────────────── */

typedef enum {
  STYLE_NORMAL,
  STYLE_H1,
  STYLE_H2,
  STYLE_LINK,
  STYLE_CODE,
  STYLE_MUTED,
  STYLE_BULLET,
  STYLE_HR
} line_style_t;

typedef struct {
  char text[LINE_CHARS + 1];
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
  /* Grid layout — see style_state_t. 0 (or 1) means full-width;
   * 2..6 places this line inside one cell of an N-column grid. */
  int grid_cols;
  int grid_col;
  int grid_row;
} line_t;

static uint32_t fb[WIN_W * WIN_H];
static eid_ctx_t ui;
static line_t lines[MAX_LINES];
static int line_count = 0;
static int scroll_line = 0;
static char page_title[64] = "index.html";
static char current_url[128] = "index.html";
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
  /* Grid layout state ----------------------------------------------
   * `grid_cols`     – if THIS element is itself a grid/flex container,
   *                   how many columns its direct children should be
   *                   placed into. 0 = not a container.
   * `grid_child_idx`– running counter of direct-child blocks already
   *                   handed out a column. Incremented when a child
   *                   push_style_state()'s under us.
   * `my_cols/my_col/my_row` – the cell THIS element is rendered in
   *                   (inherited downward so text deep inside a cell
   *                    still knows its column). 0 / 0 / 0 outside any
   *                    grid.
   */
  int grid_cols;
  int grid_child_idx;
  int my_cols;
  int my_col;
  int my_row;
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
    style_state_t *parent = &style_stack[style_depth];
    style_state_t *child = &style_stack[style_depth + 1];
    *child = *parent;
    /* The child is a fresh element — by default it is not itself a
     * grid container and has no children yet. (apply_css_for_element
     * may later set child->grid_cols when this element is the next
     * grid container down.) */
    child->grid_cols = 0;
    child->grid_child_idx = 0;
    /* If our parent IS a grid container, take the next cell. */
    if (parent->grid_cols > 1) {
      int idx = parent->grid_child_idx;
      child->my_cols = parent->grid_cols;
      child->my_col = idx % parent->grid_cols;
      child->my_row = idx / parent->grid_cols;
      parent->grid_child_idx = idx + 1;
    }
    /* else: my_cols/my_col/my_row already copied from parent, so
     * grandchildren stay in the same cell as their grand-parent. */
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
  /* Grid-container declaration: turn THIS element into a grid so the
   * next direct-child push_style_state() splits its kids by column. */
  if (r->grid_cols > 1) {
    style_stack[style_depth].grid_cols = r->grid_cols;
    style_stack[style_depth].grid_child_idx = 0;
  }
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
    } else if (strstr(val, "grid") || strstr(val, "flex")) {
      /* Tentative — `grid-template-columns` (or counting `1fr` tokens
       * for plain `display:flex` rows) refines `grid_cols` below.
       * Default 1 means "container exists but we don't know N yet" —
       * children won't be split unless a later prop sets N > 1. */
      if (rule->grid_cols == 0)
        rule->grid_cols = 1;
    }
  } else if (strncmp(prop, "grid-template-columns", 21) == 0) {
    int n = 0;
    const char *rep = strstr(val, "repeat(");
    if (rep) {
      rep += 7;
      while (*rep == ' ')
        rep++;
      while (*rep >= '0' && *rep <= '9') {
        n = n * 10 + (*rep - '0');
        rep++;
      }
    } else {
      /* Count whitespace-separated tokens — "1fr 1fr 1fr" → 3.
       * Auto-fit / auto-fill we give up on and stick with 0. */
      if (!strstr(val, "auto-fit") && !strstr(val, "auto-fill")) {
        bool in_tok = false;
        for (const char *v = val; *v; v++) {
          if (*v == ' ' || *v == '\t') {
            in_tok = false;
          } else {
            if (!in_tok)
              n++;
            in_tok = true;
          }
        }
      }
    }
    if (n > 0 && n <= 6)
      rule->grid_cols = n;
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
    /* Skip a CSS slash-star comment that may sit between properties:
     * declaration blocks often have inline comments after a value,
     * e.g. "--accent:#7dd3fc;  (* sky-300 *)" (using parens here so
     * this C comment doesn't terminate early). Without skipping them
     * the comment bleeds into the next property name and we lose all
     * subsequent --variable captures. */
    if (c == '/' && i + 1 < dlen && decl[i + 1] == '*') {
      i += 2;
      while (i + 1 < dlen && !(decl[i] == '*' && decl[i + 1] == '/'))
        i++;
      if (i + 1 < dlen)
        i++;
      continue;
    }
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

      /* Trim leading whitespace (spaces, tabs, newlines — rules
       * inside `:root { ... }` etc. are typically multi-line) */
      const char *pp = prop;
      while (*pp && ascii_isspace(*pp))
        pp++;
      const char *vv = val;
      while (*vv && ascii_isspace(*vv))
        vv++;

      if (pp[0] && vv[0]) {
        /* CSS custom property declaration: --name: value; */
        if (pp[0] == '-' && pp[1] == '-') {
          const char *name = pp + 2;
          int name_len = (int)strlen(name);
          /* Strip !important / trailing whitespace from value */
          char clean_val[CSS_VAR_VALUE_LEN];
          int cv = 0;
          for (int k = 0; vv[k] && cv < CSS_VAR_VALUE_LEN - 1; k++) {
            if (vv[k] == '!')
              break;
            clean_val[cv++] = vv[k];
          }
          while (cv > 0 && clean_val[cv - 1] == ' ')
            cv--;
          clean_val[cv] = '\0';
          css_var_define(name, name_len, clean_val);
        } else if (strstr(vv, "var(")) {
          /* Resolve var(--foo) in value before applying */
          char resolved[CSS_VAR_VALUE_LEN * 2];
          css_resolve_vars(vv, resolved, sizeof(resolved));
          apply_css_property(rule, pp, resolved);
        } else {
          apply_css_property(rule, pp, vv);
        }
      }

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

    /* Read selector (preserve spaces for compound selector splitting).
     * If the selector is too long for our buffer, keep advancing p so
     * we still find the next `{` — that way one giant comma-list like
     * `body.lang-out [data-i18n], body.lang-out #lang-top, ...` no
     * longer aborts the whole stylesheet. */
    char sel[MAX_SELECTOR];
    int si = 0;
    bool prev_space = false;
    bool sel_truncated = false;
    while (p < end && *p != '{') {
      if (ascii_isspace(*p)) {
        if (si > 0 && si < MAX_SELECTOR - 1)
          prev_space = true;
      } else {
        if (si >= MAX_SELECTOR - 1) {
          sel_truncated = true;
        } else {
          if (prev_space && si > 0 && si < MAX_SELECTOR - 1)
            sel[si++] = ' ';
          prev_space = false;
          sel[si++] = ascii_lower(*p);
        }
      }
      p++;
    }
    sel[si] = '\0';
    if (p >= end || *p != '{')
      break;
    p++; /* skip { */
    /* Keep parsing the declaration block even when the selector was
     * truncated — we still want to capture any `--var: value` lines
     * inside it. The (oversized) recorded selector probably won't
     * match anything, but that's fine. */
    (void)sel_truncated;

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
  css_var_count = 0;

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
static void apply_css_for_element(const char *tag) {
  char elem[MAX_SELECTOR];
  char cls[MAX_CSS_CLASS];
  char id[MAX_CSS_CLASS];
  char inline_style[256];

  extract_element_name(tag, elem, MAX_SELECTOR);
  extract_attr(tag, "class", cls, MAX_CSS_CLASS);
  extract_attr(tag, "id", id, MAX_CSS_CLASS);
  extract_attr(tag, "style", inline_style, sizeof(inline_style));

  /* 1. Stylesheet rules (element, .class, #id, element.class selectors).
   *
   * For descendant combinators ('.stats .grid', 'header nav a', ...) we
   * collapse to matching just the right-most simple selector — i.e. we
   * pretend the rule is '.grid' / 'a' here. That's looser than real CSS
   * but on real sites the right-most token is usually specific enough
   * (e.g. `.grid` only appears inside `.stats`), and properties like
   * `display:grid` need to fire on the page or layout falls apart. */
  for (int i = 0; i < css_rule_count; i++) {
    const css_rule_t *r = &css_rules[i];
    bool match = false;
    const char *sel = r->selector;
    const char *last_space = NULL;
    for (const char *q = sel; *q; q++)
      if (*q == ' ')
        last_space = q;
    if (last_space)
      sel = last_space + 1;

    if (sel[0] == '.') {
      if (cls[0] && has_class(cls, sel + 1))
        match = true;
    } else if (sel[0] == '#') {
      if (id[0] && strcmp(sel + 1, id) == 0)
        match = true;
    } else {
      /* Element or Element.class or Element#id */
      char sel_elem[MAX_SELECTOR];
      int dot_idx = -1;
      int hash_idx = -1;
      for (int k = 0; sel[k]; k++) {
        if (sel[k] == '.') {
          dot_idx = k;
          break;
        }
        if (sel[k] == '#') {
          hash_idx = k;
          break;
        }
      }

      if (dot_idx != -1) {
        strncpy(sel_elem, sel, dot_idx);
        sel_elem[dot_idx] = '\0';
        if (strcmp(sel_elem, elem) == 0 &&
            has_class(cls, sel + dot_idx + 1))
          match = true;
      } else if (hash_idx != -1) {
        strncpy(sel_elem, sel, hash_idx);
        sel_elem[hash_idx] = '\0';
        if (strcmp(sel_elem, elem) == 0 &&
            strcmp(id, sel + hash_idx + 1) == 0)
          match = true;
      } else {
        if (elem[0] && strcmp(sel, elem) == 0)
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
  if (inline_style[0]) {
    parse_css_declarations_to_state(inline_style);
  }
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
  if (len > LINE_CHARS)
    len = LINE_CHARS;

  for (int i = 0; i < len; i++)
    lines[line_count].text[i] = text[i];
  lines[line_count].text[len] = '\0';

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
  lines[line_count].grid_cols = style_stack[style_depth].my_cols;
  lines[line_count].grid_col = style_stack[style_depth].my_col;
  lines[line_count].grid_row = style_stack[style_depth].my_row;

  if (style == STYLE_LINK) {
    extract_attr(tag_context, "href", lines[line_count].link_url, 127);
  } else {
    lines[line_count].link_url[0] = '\0';
  }

  line_count++;
}

static void blank_line(void) {
  if (line_count == 0)
    return;
  if (line_count > 0 && lines[line_count - 1].text[0] == '\0')
    return;
  /* If we are currently AT a grid container (between cells), an
   * empty placeholder line gets tagged as non-grid (my_cols=0) and
   * would force the draw loop to exit the grid run, then re-enter
   * — which trashes per-column Y cursors and stacks the right
   * column below the left one. Skip the blank in that case. */
  if (style_stack[style_depth].grid_cols > 1)
    return;
  push_line("", 0, STYLE_NORMAL, false);
}

static void append_word(char *line, int *len, const char *word, int word_len,
                        line_style_t style, bool indent) {
  /* Cell-aware wrap: if we're inside a grid cell, our usable width
   * is only a 1/N slice of the screen. Without this, text from a
   * single card would still wrap at the full 74-char window and
   * spill into other cells when the column-aware draw loop offsets
   * it to cell_x. */
  int cols = style_stack[style_depth].my_cols;
  int base = LINE_CHARS;
  if (cols > 1)
    base = LINE_CHARS / cols;
  if (base < 8)
    base = 8;
  int max_chars = indent ? (base - 4) : base;
  if (word_len <= 0)
    return;

  if (*len > 0 && *len + 1 + word_len > max_chars) {
    push_line(line, *len, style, indent);
    *len = 0;
  }

  if (*len > 0)
    line[(*len)++] = ' ';

  while (word_len > max_chars) {
    int room = max_chars - *len;
    if (room <= 0) {
      push_line(line, *len, style, indent);
      *len = 0;
      room = max_chars;
    }
    for (int i = 0; i < room; i++)
      line[(*len)++] = *word++;
    word_len -= room;
    push_line(line, *len, style, indent);
    *len = 0;
  }

  for (int i = 0; i < word_len && *len < max_chars; i++)
    line[(*len)++] = word[i];
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

static int visible_lines(void) { return (WIN_H - CONTENT_Y - 20) / LINE_H; }

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

      int out = 0;
      uint32_t j = start;
      while (j < end && out < 63) {
        unsigned char b0 = (unsigned char)html[j];

        /* Same UTF-8 → ASCII fallback as in the body text loop. The
         * <title> ends up rendered to the page-title strip via plain
         * eid_draw_text with a single-byte font, so leaving raw multi-
         * byte sequences in produces 2-3 garbage glyphs (e.g. "—"
         * shows as "ÔÇö"). */
        if (b0 >= 0x80) {
          unsigned char b1 = (j + 1 < end) ? (unsigned char)html[j + 1] : 0;
          unsigned char b2 = (j + 2 < end) ? (unsigned char)html[j + 2] : 0;
          const char *r = "?";
          int eaten = 1;
          char tmp[2] = {0, 0};
          if (b0 == 0xC2 && b1 == 0xA0) { r = " ";  eaten = 2; }
          else if (b0 == 0xC2 && b1 == 0xB7) { r = "*"; eaten = 2; }
          else if (b0 == 0xE2 && b1 == 0x80) {
            eaten = 3;
            switch (b2) {
            case 0x90: case 0x91: case 0x92: case 0x93:
            case 0x94: case 0x95: r = "-"; break;
            case 0x98: case 0x99: case 0x9A: case 0x9B:
              tmp[0] = '\''; r = tmp; break;
            case 0x9C: case 0x9D: case 0x9E: case 0x9F: r = "\""; break;
            case 0xA2: r = "*"; break;
            case 0xA6: r = "..."; break;
            default:   r = "?"; break;
            }
          } else if (b0 == 0xE2 && b1 == 0x86) {
            eaten = 3;
            switch (b2) {
            case 0x90: r = "<-"; break;
            case 0x91: r = "^";  break;
            case 0x92: r = "->"; break;
            case 0x93: r = "v";  break;
            default:   r = "->"; break;
            }
          } else if (b0 >= 0xF0) { eaten = 4; }
          else if (b0 >= 0xE0)   { eaten = 3; }
          else if (b0 >= 0xC0)   { eaten = 2; }
          for (int k = 0; r[k] && out < 63; k++)
            page_title[out++] = r[k];
          j += eaten;
          continue;
        }

        if (!ascii_isspace((char)b0) || (out > 0 && page_title[out - 1] != ' '))
          page_title[out++] = ascii_isspace((char)b0) ? ' ' : (char)b0;
        j++;
      }
      page_title[out] = '\0';
      return;
    }
  }
}

static void parse_html(const char *html, uint32_t size) {
  line_count = 0;
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
  bool in_svg_tag = false;

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
      /* SVG: treat as opaque. Inline SVG icons would otherwise
       * push_style_state for every <svg>, <path/>, <g> etc. since
       * our self-closing detector only knows about a few HTML
       * void elements (not the `/>` XML notation). Each unmatched
       * push leaks one level of depth, which then mis-tags the
       * next sibling element's grid_col/grid_row. */
      if (tag_eq(tag, "svg")) {
        in_svg_tag = true;
        continue;
      }
      if (tag_eq(tag, "/svg")) {
        in_svg_tag = false;
        continue;
      }
      if (in_svg_tag) {
        /* Eat every other tag — open, close, self-close — inside
         * the SVG without touching the style stack. */
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
          /* Add padding blank lines if CSS padding is set —
           * but only if we are NOT currently sitting on a grid
           * container level (otherwise those blanks land between
           * cells with grid_cols=0 and snap the right column
           * down underneath the left one). */
          int pad = style_stack[style_depth].padding;
          if (style_stack[style_depth].grid_cols > 1) {
            /* between cells of a grid: emit nothing */
          } else if (pad > 0) {
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
          if (style_stack[style_depth].grid_cols > 1) {
            /* between cells of a grid: emit nothing */
          } else if (pad > 0) {
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
        in_svg_tag ||
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

    /* UTF-8 → ASCII fallback for the punctuation that actually shows
     * up on the EquinoxOS landing page. The terminal font is single-
     * byte, so without this every literal "—", "…", "→", curly quote
     * gets rendered as 2-3 garbage glyphs. We don't ship a real
     * Unicode font, so substitute a sensible ASCII equivalent. */
    if ((unsigned char)c >= 0x80) {
      unsigned char b0 = (unsigned char)c;
      unsigned char b1 = (i + 1 < size) ? (unsigned char)html[i + 1] : 0;
      unsigned char b2 = (i + 2 < size) ? (unsigned char)html[i + 2] : 0;
      int eaten = 1;
      const char *replacement = "?";
      char tmp[2] = {0, 0};
      if (b0 == 0xC2 && b1 == 0xA0) { /* nbsp */
        replacement = " ";
        eaten = 2;
      } else if (b0 == 0xC2 && b1 == 0xB7) { /* middle dot */
        replacement = "*";
        eaten = 2;
      } else if (b0 == 0xE2 && b1 == 0x80) {
        eaten = 3;
        switch (b2) {
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: /* hyphens / dashes */
          replacement = "-";
          break;
        case 0x98: case 0x99: case 0x9A: case 0x9B: /* single quotes */
          tmp[0] = '\'';
          replacement = tmp;
          break;
        case 0x9C: case 0x9D: case 0x9E: case 0x9F: /* double quotes */
          replacement = "\"";
          break;
        case 0xA2: /* bullet */
          replacement = "*";
          break;
        case 0xA6: /* ellipsis */
          replacement = "...";
          break;
        default:
          replacement = "?";
          break;
        }
      } else if (b0 == 0xE2 && b1 == 0x86) {
        /* arrows */
        eaten = 3;
        switch (b2) {
        case 0x90: replacement = "<-"; break;
        case 0x91: replacement = "^";  break;
        case 0x92: replacement = "->"; break;
        case 0x93: replacement = "v";  break;
        case 0xB5: replacement = "^";  break;
        case 0xB7: replacement = "v";  break;
        default:   replacement = "->"; break;
        }
      } else if (b0 >= 0xF0) {
        eaten = 4; /* 4-byte UTF-8 (most emoji) */
      } else if (b0 >= 0xE0) {
        eaten = 3;
      } else if (b0 >= 0xC0) {
        eaten = 2;
      }
      for (int k = 0; replacement[k] && word_len < LINE_CHARS; k++)
        word[word_len++] = replacement[k];
      i += eaten;
      continue;
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

  /* If this line belongs to a grid cell, all the "full-width"
   * decorations below (HR rule, full-width background, align:center)
   * have to be clamped to the cell — otherwise card 1's background
   * spills across cards 2/3/4 and the layout looks like one giant
   * blob. */
  int cell_left = CONTENT_X;
  int cell_width = CONTENT_W;
  if (ln->grid_cols > 1) {
    cell_width = (WIN_W - 36) / ln->grid_cols;
    cell_left = CONTENT_X + ln->grid_col * cell_width;
  }

  int w = strlen(text) * 8;
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
    x = cell_left + (cell_width - w) / 2;
  } else if (ln->css_align == ALIGN_RIGHT) {
    x = cell_left + cell_width - w;
  }

  uint32_t color = ln->css_color ? ln->css_color : color_for_style(style);

  /* CSS background override */
  if (ln->css_bg) {
    if (ln->full_width_bg) {
      /* Fill the entire window width for block-level backgrounds
       * (or the cell when this line lives inside a grid cell). */
      int bgx = (ln->grid_cols > 1) ? cell_left - 4 : 0;
      int bgw = (ln->grid_cols > 1) ? cell_width : WIN_W;
      eid_draw_rect(fb, WIN_W, WIN_H, bgx, y - 2, bgw, LINE_H, ln->css_bg);
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
    eid_draw_line(fb, WIN_W, WIN_H, cell_left, y + 8,
                  cell_left + cell_width, y + 8, CLR_BORDER);
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

  int cur_y = CONTENT_Y + 14; /* Offset for the page title bar */
  /* Per-column Y cursors for the grid renderer. per_col_y[c] tracks
   * how far down column c has already been filled within the current
   * grid run. When we hit a non-grid line (or the run ends), we
   * sync cur_y up to max(per_col_y[*]) so the next full-width line
   * starts below all cells. */
  int per_col_y[6];
  for (int c = 0; c < 6; c++)
    per_col_y[c] = cur_y;
  int last_grid_cols = 0;
  int last_grid_row = -1;
  /* The content area ends here. We can't just stop after v_lines
   * iterations the way we used to — inside a grid run the lines
   * stack column-by-column (col 0 first, then col 1...), so a
   * naive "v_lines lines = v_lines * LINE_H pixels" assumption
   * would chop off the entire right column. Instead, iterate every
   * line that could plausibly be on screen and stop only when both
   * the master cur_y AND every column cursor are past the bottom. */
  int content_end_y = WIN_H - 20;
  for (int i = 0;; i++) {
    int idx = scroll_line + i;
    if (idx >= line_count)
      break;

    const line_t *ln = &lines[idx];
    int g_cols = (ln->grid_cols > 1) ? ln->grid_cols : 0;

    /* Grid-run transitions: when the column count or row index
     * changes, sync cur_y so the next row / non-grid section
     * starts BELOW everything that was already drawn. */
    if (g_cols != last_grid_cols ||
        (g_cols > 0 && ln->grid_row != last_grid_row)) {
      int sync_y = cur_y;
      int span = last_grid_cols > 0 ? last_grid_cols : 1;
      for (int c = 0; c < span && c < 6; c++)
        if (per_col_y[c] > sync_y)
          sync_y = per_col_y[c];
      cur_y = sync_y;
      for (int c = 0; c < 6; c++)
        per_col_y[c] = cur_y;
      last_grid_cols = g_cols;
      last_grid_row = ln->grid_row;
    }

    int cur_x;
    int draw_y;
    if (g_cols > 0) {
      int cell_w = (WIN_W - 36) / g_cols;
      cur_x = CONTENT_X + ln->grid_col * cell_w + (ln->indent ? 18 : 0);
      draw_y = per_col_y[ln->grid_col];
    } else {
      cur_x = CONTENT_X + (ln->indent ? 18 : 0);
      draw_y = cur_y;
    }
    draw_text_line(cur_x, draw_y, ln);

    /* Handle link interaction */
    if (ln->style == STYLE_LINK && ln->link_url[0]) {
      uint32_t id = eid_get_id(ln->link_url, cur_x, draw_y);
      uint32_t state = eid_process_interaction(
          &ui, id, cur_x, draw_y, strlen(ln->text) * 8, LINE_H);
      if (state & EID_STATE_CLICKED) {
        char resolved[128];
        resolve_url(current_url, ln->link_url, resolved);
        strcpy(current_url, resolved);
        load_page(current_url);
        return; /* Avoid drawing more in this frame */
      }
    }

    if (g_cols > 0) {
      per_col_y[ln->grid_col] += LINE_H;
    } else {
      cur_y += LINE_H;
      for (int c = 0; c < 6; c++)
        per_col_y[c] = cur_y;
    }

    /* Early-exit when every cursor is past the bottom of the
     * visible area — both the master cur_y and every per-column
     * cursor inside an active grid run. */
    bool all_below = cur_y > content_end_y;
    if (all_below && g_cols > 0) {
      for (int c = 0; c < g_cols && c < 6; c++)
        if (per_col_y[c] <= content_end_y) {
          all_below = false;
          break;
        }
    }
    if (all_below)
      break;
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

static void load_page(const char *url) {
  print("[BROWSER] Navigating to: ");
  print(url);
  print("\n");

  /* --- File path (local resource) -------------------------------- */
  if (strncasecmp(url, "http://", 7) != 0 &&
      strncasecmp(url, "https://", 8) != 0) {
    uint32_t fsize = 0;
    char *data = (char *)_syscall(SYS_READ_FILE, (uint64_t)url,
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

  if (strstr(url, "http://") || strchr(url, '.')) {
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

  if (argc > 1 && argv[1] != 0) {
    /* User passed a target. Truncate quietly into current_url[]. */
    size_t alen = strlen(argv[1]);
    if (alen >= sizeof current_url) alen = sizeof current_url - 1;
    memcpy(current_url, argv[1], alen);
    current_url[alen] = '\0';
  }
#ifdef BROWSER_BUILD
  else {
    /* browser.elf defaults to a real internet page; htmlview.elf keeps
     * its original "index.html" local-file default. */
    strcpy(current_url, "http://example.com/");
  }
  /* Optional argv[2]: dotted-quad IP override for the first load_page().
   * Same convention as urlget — useful while QEMU SLIRP DNS is flaky. */
  if (argc > 2 && argv[2] != 0) {
    uint32_t ip_be = net_dns_resolve(argv[2]);  /* parses dotted-quad too */
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

    /* Track Shift across frames so typing ':', '/', '?', '#', etc. in the
     * URL bar works. The PS/2 driver delivers both make and break codes
     * via the ring buffer; we just watch for 0x2A/0x36 (Shift down) and
     * 0xAA/0xB6 (Shift up). */
    static bool shift_held = false;
    if (key == 0x2A || key == 0x36) shift_held = true;
    else if (key == 0xAA || key == 0xB6) shift_held = false;

    if (key == 0x01)
      break;

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
      if (key == 0x26) {
        is_typing_url = true;
        url_cursor = 0;
        current_url[0] = '\0';
      }
      if ((key == 0x50 || key == 0x1F) && scroll_line < max_scroll)
        scroll_line++;
      if ((key == 0x48 || key == 0x11) && scroll_line > 0)
        scroll_line--;
      if (key == 0x51) {
        scroll_line += visible_lines();
        if (scroll_line > max_scroll)
          scroll_line = max_scroll;
      }
      if (key == 0x49) {
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
