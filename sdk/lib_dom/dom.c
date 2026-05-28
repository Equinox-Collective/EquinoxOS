/* sdk/lib_dom/dom.c — lenient HTML → DOM tree parser.
 *
 * Not full HTML5. Handles what real pages and our own res/index.html
 * actually use:
 *   - open / close / self-closing tags
 *   - void elements (br, hr, img, meta, link, input, area, base, col,
 *     embed, source, track, wbr)
 *   - attributes: bare, =unquoted, ='single', ="double"
 *   - comments <!-- ... -->
 *   - <!doctype ...> skipped (still produces top-level <html>)
 *   - raw text in <script> and <style> (consume bytes until matching
 *     close tag, no nested tag parsing)
 *   - text content between tags is collapsed into a TEXT child
 *
 * Error handling is "lenient": malformed input never makes the
 * parser hang or read out of bounds, it just produces a degenerate
 * tree. Out of memory anywhere → returns NULL and leaks whatever
 * was already allocated to the partial tree (caller's process is
 * dying anyway).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "dom.h"

/* ---- tiny char helpers (no <ctype.h> in our SDK) ----------------- */

static bool d_isspace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
static char d_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}
static int d_strcasecmp(const char *a, const char *b) {
  while (*a && *b) {
    char ca = d_lower(*a), cb = d_lower(*b);
    if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
    a++; b++;
  }
  return (unsigned char)d_lower(*a) - (unsigned char)d_lower(*b);
}

static char *d_strndup_lower(const char *s, uint32_t n) {
  char *p = (char*)malloc(n + 1);
  if (!p) return NULL;
  for (uint32_t i = 0; i < n; i++) p[i] = d_lower(s[i]);
  p[n] = 0;
  return p;
}
static char *d_strndup(const char *s, uint32_t n) {
  char *p = (char*)malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}
static char *d_strdup_empty(void) {
  char *p = (char*)malloc(1);
  if (p) p[0] = 0;
  return p;
}

/* ---- node + attr lifecycle -------------------------------------- */

static dom_node_t *node_new(dom_node_type_t t) {
  dom_node_t *n = (dom_node_t*)malloc(sizeof(dom_node_t));
  if (!n) return NULL;
  memset(n, 0, sizeof(*n));
  n->type = t;
  return n;
}

static void node_append_child(dom_node_t *parent, dom_node_t *child) {
  child->parent = parent;
  child->prev_sibling = parent->last_child;
  child->next_sibling = NULL;
  if (parent->last_child) parent->last_child->next_sibling = child;
  else                    parent->first_child = child;
  parent->last_child = child;
}

void dom_free(dom_node_t *node) {
  if (!node) return;
  dom_node_t *c = node->first_child;
  while (c) { dom_node_t *next = c->next_sibling; dom_free(c); c = next; }
  dom_attr_t *a = node->attrs;
  while (a) { dom_attr_t *na = a->next; free(a->name); free(a->value); free(a); a = na; }
  free(node->tag_name);
  free(node->text);
  free(node);
}

/* ---- void / raw-text element tables ----------------------------- */

static const char *const VOID_ELEMENTS[] = {
  "area","base","br","col","embed","hr","img","input","link","meta",
  "source","track","wbr", NULL
};
static bool tag_is_void(const char *tag) {
  for (int i = 0; VOID_ELEMENTS[i]; i++)
    if (d_strcasecmp(tag, VOID_ELEMENTS[i]) == 0) return true;
  return false;
}
static bool tag_is_raw_text(const char *tag) {
  return d_strcasecmp(tag, "script") == 0 || d_strcasecmp(tag, "style") == 0;
}

/* ---- parser state ----------------------------------------------- */

typedef struct {
  const char *src;
  uint32_t    size;
  uint32_t    pos;
} ps_t;

static int peek(const ps_t *p) {
  return (p->pos < p->size) ? (unsigned char)p->src[p->pos] : -1;
}
static int peek2(const ps_t *p, uint32_t off) {
  return (p->pos + off < p->size) ? (unsigned char)p->src[p->pos + off] : -1;
}
static void advance(ps_t *p, uint32_t n) {
  p->pos += n; if (p->pos > p->size) p->pos = p->size;
}
static bool starts_with_ci(const ps_t *p, const char *s) {
  uint32_t n = (uint32_t)strlen(s);
  if (p->pos + n > p->size) return false;
  for (uint32_t i = 0; i < n; i++) {
    char a = d_lower(p->src[p->pos + i]);
    char b = d_lower(s[i]);
    if (a != b) return false;
  }
  return true;
}
static void skip_ws(ps_t *p) { while (p->pos < p->size && d_isspace(p->src[p->pos])) p->pos++; }

/* ---- tag/attr parsing ------------------------------------------- */

/* Read an unquoted attribute-name-like token (letters, digits, '-', '_', ':').
 * Returns malloc'd lowercased copy or NULL if empty. */
static char *read_name(ps_t *p) {
  uint32_t start = p->pos;
  while (p->pos < p->size) {
    char c = p->src[p->pos];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':') {
      p->pos++;
    } else break;
  }
  if (p->pos == start) return NULL;
  return d_strndup_lower(p->src + start, p->pos - start);
}

/* Read attribute value (after '='). Quoted or unquoted. */
static char *read_attr_value(ps_t *p) {
  if (p->pos >= p->size) return d_strdup_empty();
  char q = p->src[p->pos];
  if (q == '"' || q == '\'') {
    p->pos++;
    uint32_t start = p->pos;
    while (p->pos < p->size && p->src[p->pos] != q) p->pos++;
    char *v = d_strndup(p->src + start, p->pos - start);
    if (p->pos < p->size) p->pos++;  /* consume closing quote */
    return v;
  }
  /* unquoted */
  uint32_t start = p->pos;
  while (p->pos < p->size) {
    char c = p->src[p->pos];
    if (d_isspace(c) || c == '>' || c == '/') break;
    p->pos++;
  }
  return d_strndup(p->src + start, p->pos - start);
}

/* Parse a tag starting at the '<'. Caller already verified peek() == '<'.
 * Populates *out_tag (malloc'd, lowercased) and a linked list at *out_attrs.
 * Sets *out_close=true if it's a </tag>, *out_self_close=true if it ends with /.
 * Advances past the closing '>'. Returns false only on EOF mid-tag. */
static bool parse_tag(ps_t *p, char **out_tag, dom_attr_t **out_attrs,
                      bool *out_close, bool *out_self_close) {
  *out_tag = NULL; *out_attrs = NULL;
  *out_close = false; *out_self_close = false;

  if (peek(p) != '<') return false;
  p->pos++;  /* consume '<' */

  if (peek(p) == '/') { *out_close = true; p->pos++; }

  *out_tag = read_name(p);
  if (!*out_tag) {
    /* malformed '<' — treat the '<' as text by rewinding; caller handles. */
    p->pos--;
    return false;
  }

  /* attributes */
  dom_attr_t *head = NULL, *tail = NULL;
  for (;;) {
    skip_ws(p);
    int c = peek(p);
    if (c < 0) break;
    if (c == '>') { p->pos++; break; }
    if (c == '/') {
      p->pos++;
      skip_ws(p);
      if (peek(p) == '>') { *out_self_close = true; p->pos++; break; }
      continue;
    }
    char *name = read_name(p);
    if (!name) {
      /* stray char in tag — skip it */
      if (p->pos < p->size) p->pos++;
      continue;
    }
    char *value;
    skip_ws(p);
    if (peek(p) == '=') {
      p->pos++;
      skip_ws(p);
      value = read_attr_value(p);
    } else {
      value = d_strdup_empty();
    }
    dom_attr_t *a = (dom_attr_t*)malloc(sizeof(dom_attr_t));
    if (!a) { free(name); free(value); break; }
    a->name = name; a->value = value; a->next = NULL;
    if (tail) tail->next = a; else head = a;
    tail = a;
  }
  *out_attrs = head;
  return true;
}

/* Append a TEXT node for the byte slice [start,end). Skips creation when
 * the slice is empty or all-whitespace AND parent already has non-text
 * trailing whitespace (keeps the tree compact). */
static void append_text(dom_node_t *parent, const char *src, uint32_t start, uint32_t end) {
  if (start >= end) return;
  /* Trim purely-whitespace blocks at element boundaries — keeps the tree
   * close to what a reasonable browser would emit after whitespace
   * normalisation. Internal whitespace inside non-empty text is kept. */
  bool all_ws = true;
  for (uint32_t i = start; i < end; i++) {
    if (!d_isspace(src[i])) { all_ws = false; break; }
  }
  if (all_ws) return;

  dom_node_t *t = node_new(DOM_NODE_TEXT);
  if (!t) return;
  t->text = d_strndup(src + start, end - start);
  if (!t->text) { free(t); return; }
  node_append_child(parent, t);
}

/* Find ancestor with matching tag, return NULL if none. */
static dom_node_t *find_open_ancestor(dom_node_t *node, const char *tag) {
  while (node && node->type == DOM_NODE_ELEMENT) {
    if (d_strcasecmp(node->tag_name, tag) == 0) return node;
    node = node->parent;
  }
  return NULL;
}

/* ---- main parser ------------------------------------------------ */

dom_node_t *dom_parse(const char *html, uint32_t size) {
  dom_node_t *doc = node_new(DOM_NODE_DOCUMENT);
  if (!doc) return NULL;

  ps_t p = { html, size, 0 };
  dom_node_t *cur = doc;
  uint32_t text_start = p.pos;

  while (p.pos < p.size) {
    /* Comment */
    if (starts_with_ci(&p, "<!--")) {
      append_text(cur, html, text_start, p.pos);
      p.pos += 4;
      uint32_t cstart = p.pos;
      while (p.pos + 3 <= p.size &&
             !(html[p.pos] == '-' && html[p.pos+1] == '-' && html[p.pos+2] == '>')) {
        p.pos++;
      }
      uint32_t cend = p.pos;
      if (p.pos + 3 <= p.size) p.pos += 3; else p.pos = p.size;
      dom_node_t *cn = node_new(DOM_NODE_COMMENT);
      if (cn) {
        cn->text = d_strndup(html + cstart, cend - cstart);
        if (cn->text) node_append_child(cur, cn);
        else          free(cn);
      }
      text_start = p.pos;
      continue;
    }
    /* DOCTYPE / other <! decls — skip to '>' */
    if (peek(&p) == '<' && peek2(&p, 1) == '!') {
      append_text(cur, html, text_start, p.pos);
      while (p.pos < p.size && html[p.pos] != '>') p.pos++;
      if (p.pos < p.size) p.pos++;
      text_start = p.pos;
      continue;
    }
    /* Processing instruction <?xml ... ?> — skip */
    if (peek(&p) == '<' && peek2(&p, 1) == '?') {
      append_text(cur, html, text_start, p.pos);
      while (p.pos < p.size && html[p.pos] != '>') p.pos++;
      if (p.pos < p.size) p.pos++;
      text_start = p.pos;
      continue;
    }
    /* Tag */
    if (peek(&p) == '<' &&
        (peek2(&p, 1) == '/' ||
         (peek2(&p, 1) >= 'a' && peek2(&p, 1) <= 'z') ||
         (peek2(&p, 1) >= 'A' && peek2(&p, 1) <= 'Z'))) {
      append_text(cur, html, text_start, p.pos);

      char *tag = NULL;
      dom_attr_t *attrs = NULL;
      bool is_close = false, self_close = false;
      uint32_t saved = p.pos;
      if (!parse_tag(&p, &tag, &attrs, &is_close, &self_close)) {
        /* couldn't parse — treat the '<' as text */
        p.pos = saved + 1;
        text_start = saved;
        continue;
      }

      if (is_close) {
        /* find nearest open ancestor matching tag and pop to its parent */
        dom_node_t *anc = find_open_ancestor(cur, tag);
        if (anc) cur = anc->parent ? anc->parent : doc;
        free(tag);
        /* close tags don't carry attrs but the parser may have read some;
         * free them. */
        while (attrs) { dom_attr_t *n = attrs->next; free(attrs->name); free(attrs->value); free(attrs); attrs = n; }
        text_start = p.pos;
        continue;
      }

      /* opening tag */
      dom_node_t *el = node_new(DOM_NODE_ELEMENT);
      if (!el) { free(tag); /* leak attrs */ goto done; }
      el->tag_name = tag;
      el->attrs = attrs;
      node_append_child(cur, el);

      if (self_close || tag_is_void(el->tag_name)) {
        /* don't descend */
      } else if (tag_is_raw_text(el->tag_name)) {
        /* consume bytes verbatim until </tagname> */
        uint32_t raw_start = p.pos;
        const char *close_target;
        char buf[16];
        snprintf(buf, sizeof(buf), "</%s", el->tag_name);
        close_target = buf;
        while (p.pos < p.size) {
          if (starts_with_ci(&p, close_target)) {
            /* check it's followed by '>' or whitespace */
            uint32_t after = p.pos + (uint32_t)strlen(close_target);
            if (after >= p.size || html[after] == '>' || d_isspace(html[after])) {
              break;
            }
          }
          p.pos++;
        }
        uint32_t raw_end = p.pos;
        if (raw_end > raw_start) {
          dom_node_t *tn = node_new(DOM_NODE_TEXT);
          if (tn) {
            tn->text = d_strndup(html + raw_start, raw_end - raw_start);
            if (tn->text) node_append_child(el, tn);
            else          free(tn);
          }
        }
        /* skip past closing tag */
        while (p.pos < p.size && html[p.pos] != '>') p.pos++;
        if (p.pos < p.size) p.pos++;
      } else {
        cur = el;
      }
      text_start = p.pos;
      continue;
    }
    /* normal byte — accumulate as text */
    p.pos++;
  }
  /* trailing text */
  append_text(cur, html, text_start, p.pos);

done:
  return doc;
}

/* ---- attr / search helpers -------------------------------------- */

const char *dom_get_attr(const dom_node_t *node, const char *name) {
  if (!node || node->type != DOM_NODE_ELEMENT) return NULL;
  for (dom_attr_t *a = node->attrs; a; a = a->next) {
    if (d_strcasecmp(a->name, name) == 0) return a->value;
  }
  return NULL;
}

dom_node_t *dom_get_element_by_id(dom_node_t *root, const char *id) {
  if (!root || !id) return NULL;
  if (root->type == DOM_NODE_ELEMENT) {
    const char *v = dom_get_attr(root, "id");
    if (v && strcmp(v, id) == 0) return root;
  }
  for (dom_node_t *c = root->first_child; c; c = c->next_sibling) {
    dom_node_t *r = dom_get_element_by_id(c, id);
    if (r) return r;
  }
  return NULL;
}

dom_node_t *dom_get_first_element_by_tag(dom_node_t *root, const char *tag) {
  if (!root || !tag) return NULL;
  if (root->type == DOM_NODE_ELEMENT &&
      d_strcasecmp(root->tag_name, tag) == 0) {
    return root;
  }
  for (dom_node_t *c = root->first_child; c; c = c->next_sibling) {
    dom_node_t *r = dom_get_first_element_by_tag(c, tag);
    if (r) return r;
  }
  return NULL;
}

/* ---- debug dump ------------------------------------------------- */

static void put_indent(int n) { for (int i = 0; i < n; i++) putchar(' '); }

void dom_print(const dom_node_t *node, int indent) {
  if (!node) return;
  switch (node->type) {
    case DOM_NODE_DOCUMENT:
      put_indent(indent); printf("#document\n");
      break;
    case DOM_NODE_ELEMENT:
      put_indent(indent); printf("<%s", node->tag_name);
      for (dom_attr_t *a = node->attrs; a; a = a->next) {
        printf(" %s=\"%s\"", a->name, a->value);
      }
      printf(">\n");
      break;
    case DOM_NODE_TEXT: {
      put_indent(indent);
      printf("#text \"");
      /* print at most 60 chars, escape newlines */
      int n = 0;
      for (const char *s = node->text; *s && n < 60; s++, n++) {
        if (*s == '\n') printf("\\n");
        else if (*s == '\r') printf("\\r");
        else putchar(*s);
      }
      if (node->text && node->text[n]) printf("...");
      printf("\"\n");
      break;
    }
    case DOM_NODE_COMMENT:
      put_indent(indent); printf("<!-- %s -->\n", node->text ? node->text : "");
      break;
  }
  for (dom_node_t *c = node->first_child; c; c = c->next_sibling) {
    dom_print(c, indent + 2);
  }
}
