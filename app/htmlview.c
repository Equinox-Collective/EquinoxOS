#include <eid.h>
#include <eid_ext.h>
#include <equos.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/* ── CSS Engine ────────────────────────────────────────────────── */

#define MAX_CSS_RULES 128
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
  /* Phase R4/F0: widget hookup. NULL for plain text lines.
   * Stored as void* to avoid a forward decl above dom.h's include. */
  void *widget_node;
} line_t;

static uint32_t fb[WIN_W * WIN_H];
static eid_ctx_t ui;
static line_t lines[MAX_LINES];
static int line_count = 0;
static int scroll_line = 0;
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
    }
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
static void apply_css_for_element(const char *tag) {
  char elem[MAX_SELECTOR];
  char cls[MAX_CSS_CLASS];
  char id[MAX_CSS_CLASS];
  char inline_style[256];

  extract_element_name(tag, elem, MAX_SELECTOR);
  extract_attr(tag, "class", cls, MAX_CSS_CLASS);
  extract_attr(tag, "id", id, MAX_CSS_CLASS);
  extract_attr(tag, "style", inline_style, sizeof(inline_style));

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
  push_line("", 0, STYLE_NORMAL, false);
}

static void append_word(char *line, int *len, const char *word, int word_len,
                        line_style_t style, bool indent) {
  int max_chars = indent ? (LINE_CHARS - 4) : LINE_CHARS;
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
      for (uint32_t j = start; j < end && out < 63; j++) {
        if (!ascii_isspace(html[j]) || (out > 0 && page_title[out - 1] != ' '))
          page_title[out++] = ascii_isspace(html[j]) ? ' ' : html[j];
      }
      page_title[out] = '\0';
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
#include "../third_party/ca_bundle/ca_bundle.h"
#endif

typedef struct {
  char         current[LINE_CHARS + 1];
  int          current_len;
  char         word[LINE_CHARS + 1];
  int          word_len;
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
  for (const char *p = text; *p; p++) {
    char c = *p;
    if (w->at_li_start) {
      append_word(w->current, &w->current_len, "*", 1, STYLE_BULLET, true);
      w->at_li_start = false;
    }
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
      int consumed = decode_entity(p, &decoded);
      if (consumed > 0) {
        if (w->word_len < LINE_CHARS) w->word[w->word_len++] = decoded;
        p += consumed - 1;   /* loop increment compensates */
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
  apply_css_for_element(tag);
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
    push_line("[ IMAGE ]", 9, STYLE_MUTED, w->in_list);
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
    bool is_submit  = type && strcasecmp(type, "submit") == 0;
    if (is_textish) {
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
    }
    pop_style_state();
    w->style = save.style;
    w->in_pre = save.in_pre;
    return;
#endif
  }

  /* ---- recurse into children ----------------------------------- */
  for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
    w_emit_node(w, c);
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
#endif

static void rebuild_lines_from_dom(void) {
  line_count  = 0;
  scroll_line = 0;
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
    }
    if (name && name[0] && !skip) {
      const char *val = dom_get_attr(node, "value");
      if (!*first && *jp < cap - 1) qbuf[(*jp)++] = '&';
      *first = 0;
      *jp = url_encode_into(name, qbuf, *jp, cap);
      if (*jp < cap - 1) qbuf[(*jp)++] = '=';
      *jp = url_encode_into(val ? val : "", qbuf, *jp, cap);
    }
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
  for (int i = 0; i < v_lines; i++) {
    int idx = scroll_line + i;
    if (idx >= line_count)
      break;

    int cur_x = CONTENT_X + (lines[idx].indent ? 18 : 0);

#ifdef BROWSER_BUILD
    /* R4/F0: <button> widgets render as eid buttons and dispatch a
     * 'click' event into the page JS session when clicked. */
    if (lines[idx].style == STYLE_BUTTON && lines[idx].widget_node) {
      int btn_w = (int)strlen(lines[idx].text) * 8 + 24;
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
      int in_h = LINE_H + 4;

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
      cur_y += LINE_H + 6;
      continue;
    }
#endif

    draw_text_line(cur_x, cur_y, &lines[idx]);

    /* Handle link interaction */
    if (lines[idx].style == STYLE_LINK && lines[idx].link_url[0]) {
      uint32_t id = eid_get_id(lines[idx].link_url, cur_x, cur_y);
      uint32_t state = eid_process_interaction(
          &ui, id, cur_x, cur_y, strlen(lines[idx].text) * 8, LINE_H);
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
