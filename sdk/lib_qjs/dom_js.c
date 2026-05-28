/* sdk/lib_qjs/dom_js.c — QuickJS bindings over sdk/lib_dom (phase J4).
 *
 * Exposes:
 *   document
 *     .title                       string
 *     .documentElement             Element (<html> or document root)
 *     .body, .head                 Element or null
 *     .getElementById(id)          Element or null
 *     .getElementsByTagName(tag)   Array<Element>
 *     .querySelector(s)            "#id" or plain "tag" — Element or null
 *
 *   Element.prototype:
 *     .tagName        uppercase string
 *     .id             string ("" if absent)
 *     .className      string ("" if absent)
 *     .textContent    string — concatenated descendant text
 *     .children       Array<Element>  (element children only)
 *     .parentNode     Element or null
 *     .getAttribute(name)            string or null
 *     .hasAttribute(name)            bool
 *     .getElementsByTagName(tag)     Array<Element>  (descendants)
 *
 * Storage model: each Element JSValue holds the dom_node_t* as opaque.
 * The tree is owned by the C side (dom_parse + dom_free). JS never
 * frees nodes. Multiple wrappers can point at the same node — fine,
 * they're just borrowed handles.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "quickjs.h"
#include "qjs_helpers.h"
#include "dom.h"
#include "dom_js.h"

/* ------------------------------------------------------------------ */

static JSClassID dom_element_class_id;

/* --------------------------------------------------------------------
 * Per-element event listener pool (phase R4/F0).
 *
 * Module-static linked list of (target node, event name, JS callback)
 * triples. Each Element.addEventListener pushes here; dispatcher
 * walks the list looking for matching (node, name) pairs.
 *
 * The pool is reset by qjs_dom_teardown() — call before
 * JS_FreeContext or QuickJS will assert on the leftover JSValues.
 * ------------------------------------------------------------------ */

typedef struct el_listener {
  dom_node_t *target;     /* borrowed; lifetime == DOM tree */
  char       *event;
  JSValue     fn;
  struct el_listener *next;
} el_listener_t;

static el_listener_t *g_el_listeners;
static int            g_dom_dirty;    /* set by any mutation binding */

static void el_listeners_reset(JSContext *ctx) {
  el_listener_t *l = g_el_listeners;
  while (l) {
    el_listener_t *n = l->next;
    JS_FreeValue(ctx, l->fn);
    free(l->event); free(l); l = n;
  }
  g_el_listeners = NULL;
}

static char *xstrdup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char *o = (char *)malloc(n); if (o) memcpy(o, s, n);
  return o;
}

int qjs_dom_consume_dirty(void) { int d = g_dom_dirty; g_dom_dirty = 0; return d; }
static void dom_mark_dirty(void) { g_dom_dirty = 1; }


static void dom_element_finalizer(JSRuntime *rt, JSValue val) {
  (void)rt; (void)val;
  /* Borrowed pointer — owner (the C side) frees via dom_free(). */
}

static JSClassDef dom_element_class = {
  "Element",
  .finalizer = dom_element_finalizer,
};

/* Build a JS wrapper around `n`. Returns JS_NULL if n is NULL or not
 * an ELEMENT node. */
static JSValue wrap_element(JSContext *ctx, dom_node_t *n) {
  if (!n || n->type != DOM_NODE_ELEMENT) return JS_NULL;
  JSValue obj = JS_NewObjectClass(ctx, dom_element_class_id);
  if (JS_IsException(obj)) return obj;
  JS_SetOpaque(obj, n);
  return obj;
}

static dom_node_t *unwrap_element(JSValueConst v) {
  return (dom_node_t *)JS_GetOpaque(v, dom_element_class_id);
}

/* ------------------------------------------------------------------ */
/* Element getters                                                     */
/* ------------------------------------------------------------------ */

static JSValue dom_el_get_tagName(JSContext *ctx, JSValueConst this_val) {
  dom_node_t *n = unwrap_element(this_val);
  if (!n || !n->tag_name) return JS_NULL;
  char buf[64]; int i;
  for (i = 0; i < 63 && n->tag_name[i]; i++) {
    char c = n->tag_name[i];
    buf[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
  }
  buf[i] = 0;
  return JS_NewString(ctx, buf);
}

static JSValue dom_el_get_attr_str(JSContext *ctx, JSValueConst this_val,
                                   const char *name) {
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_NewString(ctx, "");
  const char *v = dom_get_attr(n, name);
  return JS_NewString(ctx, v ? v : "");
}

static JSValue dom_el_get_id(JSContext *ctx, JSValueConst this_val) {
  return dom_el_get_attr_str(ctx, this_val, "id");
}

static JSValue dom_el_get_className(JSContext *ctx, JSValueConst this_val) {
  return dom_el_get_attr_str(ctx, this_val, "class");
}

/* Recursive text concatenation with whitespace collapse, matching the
 * shape of `node_text_content` in app/domtest.c. */
static void text_collect(const dom_node_t *n, char *buf, int *w, int cap,
                         bool *prev_ws) {
  if (!n) return;
  if (n->type == DOM_NODE_TEXT && n->text) {
    for (const char *s = n->text; *s; s++) {
      char c = *s;
      bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
      if (ws) {
        if (!*prev_ws && *w < cap - 1) { buf[(*w)++] = ' '; *prev_ws = true; }
      } else if (*w < cap - 1) {
        buf[(*w)++] = c; *prev_ws = false;
      }
    }
  }
  for (dom_node_t *c = n->first_child; c; c = c->next_sibling)
    text_collect(c, buf, w, cap, prev_ws);
}

static JSValue dom_el_get_textContent(JSContext *ctx, JSValueConst this_val) {
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_NewString(ctx, "");
  enum { CAP = 8192 };
  char *buf = (char *)malloc(CAP);
  if (!buf) return JS_NewString(ctx, "");
  int w = 0;
  bool prev_ws = true;   /* collapses leading whitespace */
  text_collect(n, buf, &w, CAP, &prev_ws);
  while (w > 0 && buf[w-1] == ' ') w--;
  buf[w] = 0;
  JSValue r = JS_NewString(ctx, buf);
  free(buf);
  return r;
}

static JSValue dom_el_get_children(JSContext *ctx, JSValueConst this_val) {
  dom_node_t *n = unwrap_element(this_val);
  JSValue arr = JS_NewArray(ctx);
  if (!n) return arr;
  uint32_t i = 0;
  for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
    if (c->type != DOM_NODE_ELEMENT) continue;
    JS_SetPropertyUint32(ctx, arr, i++, wrap_element(ctx, c));
  }
  return arr;
}

static JSValue dom_el_get_parentNode(JSContext *ctx, JSValueConst this_val) {
  dom_node_t *n = unwrap_element(this_val);
  if (!n || !n->parent || n->parent->type != DOM_NODE_ELEMENT) return JS_NULL;
  return wrap_element(ctx, n->parent);
}

/* ------------------------------------------------------------------ */
/* Element methods                                                     */
/* ------------------------------------------------------------------ */

static JSValue dom_el_getAttribute(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)argc;
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_NULL;
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) return JS_EXCEPTION;
  const char *v = dom_get_attr(n, name);
  JS_FreeCString(ctx, name);
  return v ? JS_NewString(ctx, v) : JS_NULL;
}

static JSValue dom_el_hasAttribute(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)argc;
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_FALSE;
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) return JS_EXCEPTION;
  const char *v = dom_get_attr(n, name);
  JS_FreeCString(ctx, name);
  return v ? JS_TRUE : JS_FALSE;
}

/* Case-insensitive tag match (mirrors dom_get_first_element_by_tag's
 * lowercased tag_name vs the caller-supplied tag). */
static bool tag_eq_ci(const char *a, const char *b) {
  while (*a && *b) {
    char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
    if (ca != cb) return false;
    a++; b++;
  }
  return *a == 0 && *b == 0;
}

static void getByTag_collect(JSContext *ctx, dom_node_t *root, const char *tag,
                             JSValue arr, uint32_t *i) {
  if (!root) return;
  if (root->type == DOM_NODE_ELEMENT && tag_eq_ci(root->tag_name, tag)) {
    JS_SetPropertyUint32(ctx, arr, (*i)++, wrap_element(ctx, root));
  }
  for (dom_node_t *c = root->first_child; c; c = c->next_sibling) {
    getByTag_collect(ctx, c, tag, arr, i);
  }
}

static JSValue dom_el_getElementsByTagName(JSContext *ctx,
                                           JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
  (void)argc;
  dom_node_t *n = unwrap_element(this_val);
  JSValue arr = JS_NewArray(ctx);
  if (!n) return arr;
  const char *tag = JS_ToCString(ctx, argv[0]);
  if (!tag) { JS_FreeValue(ctx, arr); return JS_EXCEPTION; }
  uint32_t i = 0;
  /* match descendants only — skip self */
  for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
    getByTag_collect(ctx, c, tag, arr, &i);
  }
  JS_FreeCString(ctx, tag);
  return arr;
}

/* ------------------------------------------------------------------ */
/* Mutation bindings (phase J6b)                                       */
/* ------------------------------------------------------------------ */

static JSValue dom_el_setAttribute(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 2) return JS_UNDEFINED;
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_UNDEFINED;
  const char *name = JS_ToCString(ctx, argv[0]);
  const char *val  = JS_ToCString(ctx, argv[1]);
  if (name && val) { dom_set_attr(n, name, val); dom_mark_dirty(); }
  if (name) JS_FreeCString(ctx, name);
  if (val)  JS_FreeCString(ctx, val);
  return JS_UNDEFINED;
}

static JSValue dom_el_removeAttribute(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  (void)argc;
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_UNDEFINED;
  const char *name = JS_ToCString(ctx, argv[0]);
  if (name) {
    dom_remove_attr(n, name); dom_mark_dirty();
    JS_FreeCString(ctx, name);
  }
  return JS_UNDEFINED;
}

static JSValue dom_el_appendChild(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  (void)argc;
  dom_node_t *parent = unwrap_element(this_val);
  dom_node_t *child  = unwrap_element(argv[0]);
  if (parent && child) { dom_append_child(parent, child); dom_mark_dirty(); }
  /* Return the child per DOM spec — caller pattern `parent.appendChild(c)` */
  return JS_DupValue(ctx, argv[0]);
}

static JSValue dom_el_removeChild(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  (void)argc;
  dom_node_t *parent = unwrap_element(this_val);
  dom_node_t *child  = unwrap_element(argv[0]);
  if (parent && child) { dom_remove_child(parent, child); dom_mark_dirty(); }
  return JS_DupValue(ctx, argv[0]);
}

static JSValue dom_el_set_textContent(JSContext *ctx, JSValueConst this_val,
                                      JSValueConst v) {
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_UNDEFINED;
  const char *s = JS_ToCString(ctx, v);
  if (!s) return JS_EXCEPTION;
  dom_set_text_content(n, s); dom_mark_dirty();
  JS_FreeCString(ctx, s);
  return JS_UNDEFINED;
}

/* --------------------------------------------------------------------
 * Element.value (phase R4/F1)
 *
 * In real browsers `input.value` is a *live* property that mirrors the
 * `value` attribute. Scripts set the attribute via `setAttribute("value",
 * x)` or assign to `.value` and expect both paths to behave the same.
 * We model that with a plain attribute proxy: getter reads the `value`
 * attr (empty string if unset, NOT null — matches HTMLInputElement);
 * setter writes the attr and marks the DOM dirty so the next renderer
 * tick rebuilds the line list with the new buffer contents.
 *
 * This is also how the renderer round-trips user keystrokes back to JS:
 * htmlview copies the attr into a local buffer, hands it to
 * eid_text_input, then writes the mutated buffer back via dom_set_attr
 * and dispatches an 'input' event. The JS-visible value is therefore
 * always consistent with what was last drawn on screen.
 * ------------------------------------------------------------------ */
static JSValue dom_el_get_value(JSContext *ctx, JSValueConst this_val) {
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_NewString(ctx, "");
  const char *v = dom_get_attr(n, "value");
  return JS_NewString(ctx, v ? v : "");
}

static JSValue dom_el_set_value(JSContext *ctx, JSValueConst this_val,
                                JSValueConst v) {
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_UNDEFINED;
  const char *s = JS_ToCString(ctx, v);
  if (!s) return JS_EXCEPTION;
  dom_set_attr(n, "value", s);
  dom_mark_dirty();
  JS_FreeCString(ctx, s);
  return JS_UNDEFINED;
}

static JSValue dom_el_set_innerHTML(JSContext *ctx, JSValueConst this_val,
                                    JSValueConst v) {
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_UNDEFINED;
  size_t len = 0;
  const char *s = JS_ToCStringLen(ctx, &len, v);
  if (!s) return JS_EXCEPTION;
  dom_set_inner_html(n, s, (uint32_t)len); dom_mark_dirty();
  JS_FreeCString(ctx, s);
  return JS_UNDEFINED;
}

/* --------------------------------------------------------------------
 * Element.addEventListener / removeEventListener (phase R4/F0)
 * ------------------------------------------------------------------ */

static JSValue dom_el_addEventListener(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_UNDEFINED;
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) return JS_UNDEFINED;
  el_listener_t *l = (el_listener_t *)malloc(sizeof(*l));
  if (l) {
    l->target = n;
    l->event  = xstrdup(name);
    l->fn     = JS_DupValue(ctx, argv[1]);
    l->next   = g_el_listeners;
    g_el_listeners = l;
  }
  JS_FreeCString(ctx, name);
  return JS_UNDEFINED;
}

static JSValue dom_el_removeEventListener(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  if (argc < 2) return JS_UNDEFINED;
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_UNDEFINED;
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) return JS_UNDEFINED;
  el_listener_t *prev = NULL;
  for (el_listener_t *l = g_el_listeners; l; prev = l, l = l->next) {
    if (l->target == n && strcmp(l->event, name) == 0 &&
        JS_VALUE_GET_PTR(l->fn) == JS_VALUE_GET_PTR(argv[1])) {
      if (prev) prev->next = l->next; else g_el_listeners = l->next;
      JS_FreeValue(ctx, l->fn);
      free(l->event); free(l); break;
    }
  }
  JS_FreeCString(ctx, name);
  return JS_UNDEFINED;
}

/* Internal dispatcher with an optional "decorate this event before
 * dispatch" callback for event-type-specific extras (KeyboardEvent
 * .key/.keyCode, MouseEvent .clientX/Y, …). `decorate` may be NULL. */
typedef void (*event_decorator_t)(JSContext *ctx, JSValue ev, void *ud);

static int dispatch_event_decorated(JSContext *ctx, dom_node_t *target,
                                    const char *name,
                                    event_decorator_t decorate, void *ud) {
  if (!ctx || !target || !name) return 0;
  int prevented = 0;
  for (el_listener_t *l = g_el_listeners; l; l = l->next) {
    if (l->target == target && strcmp(l->event, name) == 0) {
      JSValue ev = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, ev, "type",   JS_NewString(ctx, name));
      JS_SetPropertyStr(ctx, ev, "target", wrap_element(ctx, target));
      JS_SetPropertyStr(ctx, ev, "defaultPrevented", JS_FALSE);
      if (decorate) decorate(ctx, ev, ud);
      /* preventDefault flips defaultPrevented. */
      const char *pd_src =
        "(function(e){e.preventDefault = function(){"
        "  e.defaultPrevented = true; };"
        "  e.stopPropagation = function(){};"
        "  return e; })";
      JSValue setup = JS_Eval(ctx, pd_src, strlen(pd_src),
                              "<eventInit>", JS_EVAL_TYPE_GLOBAL);
      if (!JS_IsException(setup)) {
        JSValue dummy = JS_Call(ctx, setup, JS_UNDEFINED, 1, (JSValueConst *)&ev);
        JS_FreeValue(ctx, dummy);
        JS_FreeValue(ctx, setup);
      } else { JS_FreeValue(ctx, setup); }

      JSValueConst args[] = { ev };
      JSValue ret = JS_Call(ctx, l->fn, JS_UNDEFINED, 1, args);
      if (JS_IsException(ret)) qjs_dump_exception(ctx);
      JS_FreeValue(ctx, ret);

      /* Read defaultPrevented back after the handler ran so the
       * caller can suppress the default action (form navigation,
       * link follow, …). One sticky prevent across multiple
       * listeners — DOM-compatible. */
      JSValue pf = JS_GetPropertyStr(ctx, ev, "defaultPrevented");
      if (JS_ToBool(ctx, pf) > 0) prevented = 1;
      JS_FreeValue(ctx, pf);

      JS_FreeValue(ctx, ev);
    }
  }
  return prevented;
}

int qjs_dom_dispatch_event(JSContext *ctx, dom_node_t *target,
                           const char *name) {
  return dispatch_event_decorated(ctx, target, name, NULL, NULL);
}

typedef struct {
  const char *keystr;
  int         keycode;
} key_decorate_t;

static void key_decorate(JSContext *ctx, JSValue ev, void *ud) {
  key_decorate_t *kd = (key_decorate_t *)ud;
  JS_SetPropertyStr(ctx, ev, "key",
      JS_NewString(ctx, kd->keystr ? kd->keystr : ""));
  JS_SetPropertyStr(ctx, ev, "keyCode", JS_NewInt32(ctx, kd->keycode));
  /* `which` is a deprecated alias still used by some libs. */
  JS_SetPropertyStr(ctx, ev, "which",   JS_NewInt32(ctx, kd->keycode));
}

int qjs_dom_dispatch_key_event(JSContext *ctx, dom_node_t *target,
                               const char *name,
                               const char *keystr, int keycode) {
  key_decorate_t kd = { keystr, keycode };
  return dispatch_event_decorated(ctx, target, name, key_decorate, &kd);
}

void qjs_dom_teardown(JSContext *ctx) {
  el_listeners_reset(ctx);
  g_dom_dirty = 0;
}

/* --------------------------------------------------------------------
 * Element.prototype entries
 * ------------------------------------------------------------------ */

/* ====================================================================
 * R6/B3: querySelectorAll + selector engine
 *
 * A compact CSS selector engine that handles real-world selectors
 * found in modern hand-written sites, without dragging in a full
 * CSS-3 implementation:
 *
 *   #id                 id match
 *   tag                 tag match (case-insensitive)
 *   .class              class token match (space-separated list)
 *   [attr]              attribute presence
 *   [attr=value]        attribute equality
 *   [attr^=value]       prefix
 *   [attr$=value]       suffix
 *   [attr*=value]       substring
 *   tag.class[attr=v]   compound — all simple parts ANDed
 *   "a b"               descendant combinator (one or more spaces)
 *
 *  Not handled (uncommon in our target site):
 *   - >  +  ~           child / adjacent / sibling combinators
 *   - pseudo-classes    :hover :nth-child(...)
 *   - escape sequences  \. etc.
 *
 *  Memory: parses into a stack-allocated `sel_t` (≤8 compounds × ≤16
 *  simple parts). Selectors that don't fit return zero matches —
 *  good enough for the sites we target.
 * ================================================================== */

#define SEL_MAX_COMPOUND   8
#define SEL_MAX_SIMPLE    16

typedef enum {
  SIM_TAG = 0,
  SIM_ID,
  SIM_CLASS,
  SIM_ATTR_HAS,
  SIM_ATTR_EQ,
  SIM_ATTR_PREFIX,
  SIM_ATTR_SUFFIX,
  SIM_ATTR_CONTAINS,
} sim_kind_t;

typedef struct {
  sim_kind_t kind;
  char       a[64];   /* tag / class / id / attr name */
  char       b[64];   /* attr value */
} sim_t;

typedef struct {
  sim_t    parts[SEL_MAX_SIMPLE];
  int      n_parts;
} compound_t;

typedef struct {
  compound_t comps[SEL_MAX_COMPOUND];
  int        n_comps;
  int        ok;
} sel_t;

static void sel_skip_ws(const char **pp) {
  while (**pp == ' ' || **pp == '\t') (*pp)++;
}

/* Parse one compound selector starting at *pp. Stops on space or end. */
static int sel_parse_compound(const char **pp, compound_t *out) {
  const char *p = *pp;
  out->n_parts = 0;
  while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '>' &&
         *p != '+' && *p != '~') {
    if (out->n_parts >= SEL_MAX_SIMPLE) return -1;
    sim_t *s = &out->parts[out->n_parts];
    s->a[0] = 0; s->b[0] = 0;
    if (*p == '#' || *p == '.') {
      s->kind = (*p == '#') ? SIM_ID : SIM_CLASS;
      p++;
      int i = 0;
      while (*p && *p != ' ' && *p != '.' && *p != '#' && *p != '[' &&
             *p != ',' && i < 63) s->a[i++] = *p++;
      s->a[i] = 0;
      if (i == 0) return -1;
    } else if (*p == '[') {
      p++;
      int i = 0;
      while (*p && *p != '=' && *p != ']' && *p != '^' && *p != '$' &&
             *p != '*' && *p != '~' && *p != '|' && i < 63)
        s->a[i++] = *p++;
      s->a[i] = 0;
      if (i == 0) return -1;
      s->kind = SIM_ATTR_HAS;
      if (*p == '^' || *p == '$' || *p == '*') {
        char op = *p++;
        if (op == '^') s->kind = SIM_ATTR_PREFIX;
        else if (op == '$') s->kind = SIM_ATTR_SUFFIX;
        else s->kind = SIM_ATTR_CONTAINS;
      }
      if (*p == '=') {
        if (s->kind == SIM_ATTR_HAS) s->kind = SIM_ATTR_EQ;
        p++;
        char q = 0;
        if (*p == '"' || *p == '\'') { q = *p++; }
        int j = 0;
        while (*p && *p != ']' && (q ? *p != q : (*p != ' ' && *p != ']'))
               && j < 63)
          s->b[j++] = *p++;
        s->b[j] = 0;
        if (q && *p == q) p++;
      }
      if (*p == ']') p++;
    } else {
      /* bare tag */
      s->kind = SIM_TAG;
      int i = 0;
      while (*p && *p != ' ' && *p != '.' && *p != '#' && *p != '[' &&
             *p != ',' && i < 63) {
        char c = *p++;
        s->a[i++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
      }
      s->a[i] = 0;
      if (i == 0) return -1;
    }
    out->n_parts++;
  }
  *pp = p;
  return out->n_parts > 0 ? 0 : -1;
}

static void sel_parse(const char *src, sel_t *out) {
  out->n_comps = 0; out->ok = 0;
  if (!src) return;
  const char *p = src;
  sel_skip_ws(&p);
  while (*p && out->n_comps < SEL_MAX_COMPOUND) {
    /* stop at "," — we don't support selector lists; treat as
     * end-of-selector so callers can extend later if needed. */
    if (*p == ',') break;
    if (sel_parse_compound(&p, &out->comps[out->n_comps]) != 0) return;
    out->n_comps++;
    sel_skip_ws(&p);
  }
  out->ok = (out->n_comps > 0);
}

static int classlist_has_token(const char *list, const char *tok) {
  if (!list || !*list || !tok || !*tok) return 0;
  int tlen = 0; while (tok[tlen]) tlen++;
  const char *p = list;
  while (*p) {
    while (*p == ' ' || *p == '\t') p++;
    const char *s = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    int len = (int)(p - s);
    if (len == tlen) {
      int eq = 1;
      for (int i = 0; i < tlen; i++) if (s[i] != tok[i]) { eq = 0; break; }
      if (eq) return 1;
    }
  }
  return 0;
}

static int str_starts(const char *s, const char *pre) {
  while (*pre) { if (*s++ != *pre++) return 0; } return 1;
}
static int str_ends(const char *s, const char *suf) {
  int ls = 0, lp = 0;
  while (s[ls]) ls++;
  while (suf[lp]) lp++;
  if (lp > ls) return 0;
  for (int i = 0; i < lp; i++) if (s[ls - lp + i] != suf[i]) return 0;
  return 1;
}
static int str_contains(const char *s, const char *needle) {
  if (!*needle) return 1;
  for (; *s; s++) if (str_starts(s, needle)) return 1;
  return 0;
}

static int match_simple(dom_node_t *n, const sim_t *s) {
  if (!n || n->type != DOM_NODE_ELEMENT) return 0;
  switch (s->kind) {
    case SIM_TAG: {
      if (!n->tag_name) return 0;
      const char *a = s->a; const char *b = n->tag_name;
      while (*a && *b && *a == *b) { a++; b++; }
      return *a == 0 && *b == 0;
    }
    case SIM_ID: {
      const char *v = dom_get_attr(n, "id");
      if (!v) return 0;
      const char *a = s->a;
      while (*a && *v && *a == *v) { a++; v++; }
      return *a == 0 && *v == 0;
    }
    case SIM_CLASS: {
      const char *cls = dom_get_attr(n, "class");
      return classlist_has_token(cls, s->a);
    }
    case SIM_ATTR_HAS: {
      return dom_get_attr(n, s->a) != NULL;
    }
    case SIM_ATTR_EQ: {
      const char *v = dom_get_attr(n, s->a);
      if (!v) return 0;
      const char *b = s->b;
      while (*b && *v && *b == *v) { b++; v++; }
      return *b == 0 && *v == 0;
    }
    case SIM_ATTR_PREFIX: {
      const char *v = dom_get_attr(n, s->a);
      return v ? str_starts(v, s->b) : 0;
    }
    case SIM_ATTR_SUFFIX: {
      const char *v = dom_get_attr(n, s->a);
      return v ? str_ends(v, s->b) : 0;
    }
    case SIM_ATTR_CONTAINS: {
      const char *v = dom_get_attr(n, s->a);
      return v ? str_contains(v, s->b) : 0;
    }
  }
  return 0;
}

static int match_compound(dom_node_t *n, const compound_t *c) {
  for (int i = 0; i < c->n_parts; i++)
    if (!match_simple(n, &c->parts[i])) return 0;
  return 1;
}

/* Does `n` match the full selector? The last compound must match n
 * itself; each previous compound (in order) must match SOME ancestor
 * such that the ancestor chain runs in selector order top→down. */
static int match_full(dom_node_t *n, const sel_t *sel) {
  if (!sel->ok || sel->n_comps == 0) return 0;
  int last = sel->n_comps - 1;
  if (!match_compound(n, &sel->comps[last])) return 0;
  /* Walk up; for each remaining compound try to find an ancestor. */
  dom_node_t *anc = n->parent;
  for (int i = last - 1; i >= 0; i--) {
    while (anc && !match_compound(anc, &sel->comps[i])) anc = anc->parent;
    if (!anc) return 0;
    anc = anc->parent;
  }
  return 1;
}

static void qsa_collect(JSContext *ctx, dom_node_t *root, const sel_t *sel,
                        JSValue arr, uint32_t *i) {
  if (!root) return;
  if (root->type == DOM_NODE_ELEMENT && match_full(root, sel))
    JS_SetPropertyUint32(ctx, arr, (*i)++, wrap_element(ctx, root));
  for (dom_node_t *c = root->first_child; c; c = c->next_sibling)
    qsa_collect(ctx, c, sel, arr, i);
}

static dom_node_t *qsa_first(dom_node_t *root, const sel_t *sel) {
  if (!root) return NULL;
  if (root->type == DOM_NODE_ELEMENT && match_full(root, sel)) return root;
  for (dom_node_t *c = root->first_child; c; c = c->next_sibling) {
    dom_node_t *r = qsa_first(c, sel);
    if (r) return r;
  }
  return NULL;
}

static JSValue qsa_generic(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int find_all) {
  (void)argc;
  dom_node_t *root = unwrap_element(this_val);
  if (!root) return find_all ? JS_NewArray(ctx) : JS_NULL;
  const char *sel_src = JS_ToCString(ctx, argv[0]);
  if (!sel_src) return JS_EXCEPTION;
  sel_t sel;
  sel_parse(sel_src, &sel);
  JS_FreeCString(ctx, sel_src);
  if (!sel.ok) return find_all ? JS_NewArray(ctx) : JS_NULL;

  /* Search descendants only — exclude `this` so callers like
   * node.querySelectorAll('a') don't match the node itself. */
  if (find_all) {
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (dom_node_t *c = root->first_child; c; c = c->next_sibling)
      qsa_collect(ctx, c, &sel, arr, &i);
    return arr;
  } else {
    for (dom_node_t *c = root->first_child; c; c = c->next_sibling) {
      dom_node_t *r = qsa_first(c, &sel);
      if (r) return wrap_element(ctx, r);
    }
    return JS_NULL;
  }
}

static JSValue dom_el_querySelectorAll(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  return qsa_generic(ctx, this_val, argc, argv, 1);
}

static JSValue dom_el_querySelector(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  return qsa_generic(ctx, this_val, argc, argv, 0);
}

/* ====================================================================
 * R6/B3: classList
 * ================================================================== */

static dom_node_t *classlist_node(JSValueConst this_val) {
  /* The classList object stashes the underlying element pointer on
   * its own opaque field via an unused property; instead we reuse the
   * Element class id so JS_GetOpaque returns the element directly. */
  return (dom_node_t *)JS_GetOpaque(this_val, dom_element_class_id);
}

static void classlist_write(dom_node_t *n, const char *list) {
  /* Collapse leading/trailing spaces and write back. */
  while (*list == ' ') list++;
  char buf[1024]; int w = 0;
  bool prev_ws = false;
  while (*list && w < (int)sizeof(buf) - 1) {
    char c = *list++;
    if (c == ' ' || c == '\t') {
      if (!prev_ws) { buf[w++] = ' '; prev_ws = true; }
    } else { buf[w++] = c; prev_ws = false; }
  }
  while (w > 0 && buf[w-1] == ' ') w--;
  buf[w] = 0;
  dom_set_attr(n, "class", buf);
  dom_mark_dirty();
}

static JSValue cl_add(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv) {
  dom_node_t *n = classlist_node(this_val);
  if (!n) return JS_UNDEFINED;
  for (int i = 0; i < argc; i++) {
    const char *tok = JS_ToCString(ctx, argv[i]);
    if (!tok) continue;
    const char *cur = dom_get_attr(n, "class");
    if (!classlist_has_token(cur, tok)) {
      char buf[1024];
      if (cur && *cur)
        snprintf(buf, sizeof(buf), "%s %s", cur, tok);
      else
        snprintf(buf, sizeof(buf), "%s", tok);
      classlist_write(n, buf);
    }
    JS_FreeCString(ctx, tok);
  }
  return JS_UNDEFINED;
}

/* Build a new class string with `tok` removed (all occurrences). */
static void classlist_remove_one(dom_node_t *n, const char *tok) {
  const char *cur = dom_get_attr(n, "class");
  if (!cur || !*cur) return;
  int tlen = 0; while (tok[tlen]) tlen++;
  char out[1024]; int w = 0;
  const char *p = cur;
  while (*p) {
    while (*p == ' ') p++;
    const char *s = p;
    while (*p && *p != ' ') p++;
    int len = (int)(p - s);
    int match = (len == tlen);
    if (match) for (int i = 0; i < tlen; i++) if (s[i] != tok[i]) { match = 0; break; }
    if (!match && len > 0) {
      if (w > 0 && w < (int)sizeof(out)-1) out[w++] = ' ';
      for (int i = 0; i < len && w < (int)sizeof(out)-1; i++) out[w++] = s[i];
    }
  }
  out[w] = 0;
  dom_set_attr(n, "class", out);
  dom_mark_dirty();
}

static JSValue cl_remove(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
  dom_node_t *n = classlist_node(this_val);
  if (!n) return JS_UNDEFINED;
  for (int i = 0; i < argc; i++) {
    const char *tok = JS_ToCString(ctx, argv[i]);
    if (!tok) continue;
    classlist_remove_one(n, tok);
    JS_FreeCString(ctx, tok);
  }
  return JS_UNDEFINED;
}

static JSValue cl_contains(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
  (void)argc;
  dom_node_t *n = classlist_node(this_val);
  if (!n) return JS_FALSE;
  const char *tok = JS_ToCString(ctx, argv[0]);
  if (!tok) return JS_FALSE;
  int has = classlist_has_token(dom_get_attr(n, "class"), tok);
  JS_FreeCString(ctx, tok);
  return JS_NewBool(ctx, has);
}

static JSValue cl_toggle(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
  dom_node_t *n = classlist_node(this_val);
  if (!n) return JS_FALSE;
  const char *tok = JS_ToCString(ctx, argv[0]);
  if (!tok) return JS_FALSE;
  int has = classlist_has_token(dom_get_attr(n, "class"), tok);
  int force_given = (argc >= 2);
  int force = force_given ? JS_ToBool(ctx, argv[1]) : 0;
  int want = force_given ? force : !has;
  if (want && !has) {
    const char *cur = dom_get_attr(n, "class");
    char buf[1024];
    if (cur && *cur) snprintf(buf, sizeof(buf), "%s %s", cur, tok);
    else             snprintf(buf, sizeof(buf), "%s", tok);
    classlist_write(n, buf);
  } else if (!want && has) {
    classlist_remove_one(n, tok);
  }
  JS_FreeCString(ctx, tok);
  return JS_NewBool(ctx, want);
}

static JSValue cl_replace(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
  (void)argc;
  dom_node_t *n = classlist_node(this_val);
  if (!n) return JS_FALSE;
  const char *a = JS_ToCString(ctx, argv[0]);
  const char *b = JS_ToCString(ctx, argv[1]);
  if (!a || !b) {
    if (a) JS_FreeCString(ctx, a);
    if (b) JS_FreeCString(ctx, b);
    return JS_FALSE;
  }
  int has = classlist_has_token(dom_get_attr(n, "class"), a);
  if (has) {
    classlist_remove_one(n, a);
    const char *cur = dom_get_attr(n, "class");
    char buf[1024];
    if (cur && *cur) snprintf(buf, sizeof(buf), "%s %s", cur, b);
    else             snprintf(buf, sizeof(buf), "%s", b);
    classlist_write(n, buf);
  }
  JS_FreeCString(ctx, a);
  JS_FreeCString(ctx, b);
  return JS_NewBool(ctx, has);
}

/* getter for element.classList. We build a fresh wrapper each time,
 * stashing the element pointer on it via the Element class id so the
 * helper methods can recover it without a separate class. */
static JSValue dom_el_get_classList(JSContext *ctx, JSValueConst this_val) {
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_NULL;
  JSValue obj = JS_NewObjectClass(ctx, dom_element_class_id);
  JS_SetOpaque(obj, n);
  JS_SetPropertyStr(ctx, obj, "add",
      JS_NewCFunction(ctx, cl_add,      "add",      1));
  JS_SetPropertyStr(ctx, obj, "remove",
      JS_NewCFunction(ctx, cl_remove,   "remove",   1));
  JS_SetPropertyStr(ctx, obj, "contains",
      JS_NewCFunction(ctx, cl_contains, "contains", 1));
  JS_SetPropertyStr(ctx, obj, "toggle",
      JS_NewCFunction(ctx, cl_toggle,   "toggle",   1));
  JS_SetPropertyStr(ctx, obj, "replace",
      JS_NewCFunction(ctx, cl_replace,  "replace",  2));
  return obj;
}

/* ====================================================================
 * R6/B3: dataset
 *
 * Build a snapshot object of every `data-*` attribute, exposed as
 * `dataset.foo` for an attribute called `data-foo`. Writes via
 * `el.dataset.foo = "x"` propagate back to the element attribute via
 * a per-key setter (rare in practice, but the site uses
 * `ul.dataset.loaded = '1'`).
 * ================================================================== */

/* dataset proxy: a fresh object built on each access, with one entry
 * per `data-*` attribute. Writes go through a (somewhat hacky) closure
 * — to keep things simple we just provide a setter via Proxy-less
 * Object.defineProperty stamping the value back onto the underlying
 * element on every write. */

/* For writes, we stash the element node on the dataset object itself
 * (via Element class id opaque), then convert key→data-key on
 * defineProperty('set'). To stay small we install a generic proxy:
 * the dataset object IS the element wrapper plus we expose camelCase
 * properties whose getter reads attribute and setter writes it. */

typedef struct { char a[64]; } ds_key_ctx_t;  /* unused — we keep keys
                                                 in the JS property name
                                                 directly */

/* Convert "fooBar" → "data-foo-bar" into out (lowercased). */
static void ds_camel_to_attr(const char *in, char *out, size_t cap) {
  size_t w = 0;
  const char *p0 = "data-";
  for (int i = 0; p0[i] && w < cap-1; i++) out[w++] = p0[i];
  for (; *in && w < cap-1; in++) {
    char c = *in;
    if (c >= 'A' && c <= 'Z') {
      if (w < cap-1) out[w++] = '-';
      if (w < cap-1) out[w++] = (char)(c + 32);
    } else out[w++] = c;
  }
  out[w] = 0;
}

/* Convert "data-foo-bar" → "fooBar". Returns 1 if input is data-*. */
static int ds_attr_to_camel(const char *in, char *out, size_t cap) {
  if (in[0] != 'd' || in[1] != 'a' || in[2] != 't' || in[3] != 'a' ||
      in[4] != '-')
    return 0;
  const char *p = in + 5;
  size_t w = 0;
  int up = 0;
  while (*p && w < cap-1) {
    char c = *p++;
    if (c == '-') { up = 1; continue; }
    if (up) { c = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; up = 0; }
    out[w++] = c;
  }
  out[w] = 0;
  return 1;
}

/* The dataset object is created via a JS Proxy emulated by getters
 * and setters per key. Since we don't know the keys up-front for
 * writes that introduce new ones, we fall back to "snapshot of
 * existing data-* attributes" with stamped setters; new writes are
 * also caught by adding a JS Proxy with get/set handlers. Implement
 * via Proxy. */

static JSValue ds_proxy_get(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
  (void)argc; (void)this_val;
  /* argv: target, key, receiver */
  dom_node_t *n = unwrap_element(argv[0]);
  if (!n) return JS_UNDEFINED;
  if (JS_IsSymbol(argv[1])) return JS_UNDEFINED;
  const char *key = JS_ToCString(ctx, argv[1]);
  if (!key) return JS_UNDEFINED;
  char attr[80];
  ds_camel_to_attr(key, attr, sizeof(attr));
  JS_FreeCString(ctx, key);
  const char *v = dom_get_attr(n, attr);
  return v ? JS_NewString(ctx, v) : JS_UNDEFINED;
}

static JSValue ds_proxy_set(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
  (void)argc; (void)this_val;
  dom_node_t *n = unwrap_element(argv[0]);
  if (!n) return JS_TRUE;
  if (JS_IsSymbol(argv[1])) return JS_TRUE;
  const char *key = JS_ToCString(ctx, argv[1]);
  if (!key) return JS_TRUE;
  char attr[80];
  ds_camel_to_attr(key, attr, sizeof(attr));
  JS_FreeCString(ctx, key);
  const char *val = JS_ToCString(ctx, argv[2]);
  dom_set_attr(n, attr, val ? val : "");
  dom_mark_dirty();
  if (val) JS_FreeCString(ctx, val);
  return JS_TRUE;
}

static JSValue ds_proxy_has(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
  (void)ctx; (void)this_val; (void)argc;
  dom_node_t *n = unwrap_element(argv[0]);
  if (!n) return JS_FALSE;
  if (JS_IsSymbol(argv[1])) return JS_FALSE;
  const char *key = JS_ToCString(ctx, argv[1]);
  if (!key) return JS_FALSE;
  char attr[80];
  ds_camel_to_attr(key, attr, sizeof(attr));
  JS_FreeCString(ctx, key);
  return JS_NewBool(ctx, dom_get_attr(n, attr) != NULL);
}

static JSValue dom_el_get_dataset(JSContext *ctx, JSValueConst this_val) {
  dom_node_t *n = unwrap_element(this_val);
  if (!n) return JS_NULL;
  /* Build a target object holding the element pointer and pre-populate
   * with snapshot keys (useful for `for (const k of Object.keys(ds))`). */
  JSValue target = JS_NewObjectClass(ctx, dom_element_class_id);
  JS_SetOpaque(target, n);
  for (dom_attr_t *a = n->attrs; a; a = a->next) {
    char camel[80];
    if (ds_attr_to_camel(a->name, camel, sizeof(camel)))
      JS_SetPropertyStr(ctx, target, camel,
                        JS_NewString(ctx, a->value ? a->value : ""));
  }

  /* Wrap with a Proxy so writes/reads of unknown keys also flow
   * through to the underlying element. */
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ProxyCtor = JS_GetPropertyStr(ctx, global, "Proxy");
  JS_FreeValue(ctx, global);
  if (!JS_IsConstructor(ctx, ProxyCtor)) {
    JS_FreeValue(ctx, ProxyCtor);
    return target;
  }
  JSValue handler = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, handler, "get",
      JS_NewCFunction(ctx, ds_proxy_get, "get", 3));
  JS_SetPropertyStr(ctx, handler, "set",
      JS_NewCFunction(ctx, ds_proxy_set, "set", 4));
  JS_SetPropertyStr(ctx, handler, "has",
      JS_NewCFunction(ctx, ds_proxy_has, "has", 2));
  JSValue args[2] = { target, handler };
  JSValue proxy = JS_CallConstructor(ctx, ProxyCtor, 2, args);
  JS_FreeValue(ctx, target);
  JS_FreeValue(ctx, handler);
  JS_FreeValue(ctx, ProxyCtor);
  if (JS_IsException(proxy)) {
    qjs_dump_exception(ctx);
    return JS_NULL;
  }
  return proxy;
}

/* ====================================================================
 * R6/B3: getBoundingClientRect + offsetWidth/Height
 *
 * We don't have a real layout engine to produce true rects, so we
 * return a benign placeholder. Pages use this primarily for two
 * patterns:
 *   - element.getBoundingClientRect().top  (for in-page scroll math)
 *   - element.offsetWidth                  (for forced reflow tricks)
 * Both still "work" with a zero-rect: scroll math becomes scrollY-80,
 * which our scrollTo just clamps to >=0.
 * ================================================================== */

static JSValue dom_el_getBoundingClientRect(JSContext *ctx,
                                            JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
  (void)this_val; (void)argc; (void)argv;
  JSValue r = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, r, "x",      JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, r, "y",      JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, r, "top",    JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, r, "left",   JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, r, "right",  JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, r, "bottom", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, r, "width",  JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, r, "height", JS_NewInt32(ctx, 0));
  return r;
}

static JSValue dom_el_get_offsetWidth(JSContext *ctx, JSValueConst this_val) {
  (void)ctx; (void)this_val;
  return JS_NewInt32(ctx, 0);
}
static JSValue dom_el_get_offsetHeight(JSContext *ctx, JSValueConst this_val) {
  (void)ctx; (void)this_val;
  return JS_NewInt32(ctx, 0);
}
static JSValue dom_el_get_clientWidth(JSContext *ctx, JSValueConst this_val) {
  (void)ctx; (void)this_val;
  return JS_NewInt32(ctx, 0);
}
static JSValue dom_el_get_clientHeight(JSContext *ctx, JSValueConst this_val) {
  (void)ctx; (void)this_val;
  return JS_NewInt32(ctx, 0);
}

/* ====================================================================
 * R6/B3: style stub
 *
 * Pages often do `el.style.transform = 'foo'`. We don't actually
 * support arbitrary CSS, but JS shouldn't crash when poking style —
 * return a plain object that accepts any property assignment.
 * ================================================================== */

static JSValue dom_el_get_style(JSContext *ctx, JSValueConst this_val) {
  (void)this_val;
  /* A bare object — assignments to .transform/.opacity/etc. are
   * accepted but have no visual effect. */
  return JS_NewObject(ctx);
}

static const JSCFunctionListEntry dom_element_proto[] = {
  JS_CGETSET_DEF("tagName",      dom_el_get_tagName,     NULL),
  JS_CGETSET_DEF("id",           dom_el_get_id,          NULL),
  JS_CGETSET_DEF("className",    dom_el_get_className,   NULL),
  JS_CGETSET_DEF("textContent",  dom_el_get_textContent, dom_el_set_textContent),
  JS_CGETSET_DEF("innerHTML",    NULL,                   dom_el_set_innerHTML),
  JS_CGETSET_DEF("value",        dom_el_get_value,       dom_el_set_value),
  JS_CGETSET_DEF("children",     dom_el_get_children,    NULL),
  JS_CGETSET_DEF("parentNode",   dom_el_get_parentNode,  NULL),
  JS_CGETSET_DEF("classList",    dom_el_get_classList,   NULL),
  JS_CGETSET_DEF("dataset",      dom_el_get_dataset,     NULL),
  JS_CGETSET_DEF("style",        dom_el_get_style,       NULL),
  JS_CGETSET_DEF("offsetWidth",  dom_el_get_offsetWidth,  NULL),
  JS_CGETSET_DEF("offsetHeight", dom_el_get_offsetHeight, NULL),
  JS_CGETSET_DEF("clientWidth",  dom_el_get_clientWidth,  NULL),
  JS_CGETSET_DEF("clientHeight", dom_el_get_clientHeight, NULL),
  JS_CFUNC_DEF("getAttribute",            1, dom_el_getAttribute),
  JS_CFUNC_DEF("hasAttribute",            1, dom_el_hasAttribute),
  JS_CFUNC_DEF("getElementsByTagName",    1, dom_el_getElementsByTagName),
  JS_CFUNC_DEF("querySelector",           1, dom_el_querySelector),
  JS_CFUNC_DEF("querySelectorAll",        1, dom_el_querySelectorAll),
  JS_CFUNC_DEF("getBoundingClientRect",   0, dom_el_getBoundingClientRect),
  JS_CFUNC_DEF("setAttribute",            2, dom_el_setAttribute),
  JS_CFUNC_DEF("removeAttribute",         1, dom_el_removeAttribute),
  JS_CFUNC_DEF("appendChild",             1, dom_el_appendChild),
  JS_CFUNC_DEF("removeChild",             1, dom_el_removeChild),
  JS_CFUNC_DEF("addEventListener",        2, dom_el_addEventListener),
  JS_CFUNC_DEF("removeEventListener",     2, dom_el_removeEventListener),
};

/* ------------------------------------------------------------------ */
/* document object methods                                             */
/* ------------------------------------------------------------------ */

/* The doc tree pointer is stashed on the document object's opaque so
 * methods can recover it without a global. We reuse the Element class
 * id even for the document wrapper because it lets every method that
 * operates on a node (including `document` for tag/id search) recover
 * a dom_node_t* uniformly. */

static JSValue doc_getElementById(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  (void)argc;
  dom_node_t *root = unwrap_element(this_val);
  if (!root) return JS_NULL;
  const char *id = JS_ToCString(ctx, argv[0]);
  if (!id) return JS_EXCEPTION;
  dom_node_t *r = dom_get_element_by_id(root, id);
  JS_FreeCString(ctx, id);
  return r ? wrap_element(ctx, r) : JS_NULL;
}

static JSValue doc_getElementsByTagName(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  (void)argc;
  dom_node_t *root = unwrap_element(this_val);
  JSValue arr = JS_NewArray(ctx);
  if (!root) return arr;
  const char *tag = JS_ToCString(ctx, argv[0]);
  if (!tag) { JS_FreeValue(ctx, arr); return JS_EXCEPTION; }
  uint32_t i = 0;
  getByTag_collect(ctx, root, tag, arr, &i);
  JS_FreeCString(ctx, tag);
  return arr;
}

/* document.createElement: returns a free-standing element wrapper.
 * Caller must appendChild() to attach. If the script drops the
 * reference without attaching, the C node leaks until page teardown
 * — acceptable for now (page lifetime is short). */
static JSValue doc_createElement(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  (void)this_val; (void)argc;
  const char *tag = JS_ToCString(ctx, argv[0]);
  if (!tag) return JS_EXCEPTION;
  dom_node_t *n = dom_create_element(tag);
  JS_FreeCString(ctx, tag);
  if (!n) return JS_ThrowOutOfMemory(ctx);
  return wrap_element(ctx, n);
}

/* document.createTextNode mirrors createElement but for TEXT nodes;
 * less commonly used by scripts than the .textContent setter but
 * convenient for surgical insertions. */
static JSValue doc_createTextNode(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  (void)this_val; (void)argc;
  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) return JS_EXCEPTION;
  dom_node_t *n = dom_create_text(s);
  JS_FreeCString(ctx, s);
  if (!n) return JS_ThrowOutOfMemory(ctx);
  return wrap_element(ctx, n);
}

/* Legacy minimal doc_querySelector removed in R6/B3 — see the
 * selector engine above; document now binds dom_el_querySelector
 * directly. */

/* ------------------------------------------------------------------ */
/* installation                                                        */
/* ------------------------------------------------------------------ */

void qjs_install_dom(JSContext *ctx, dom_node_t *doc) {
  JSRuntime *rt = JS_GetRuntime(ctx);

  /* JS_NewClassID hands out a globally-unique id (process-wide) and
   * caches into the static. JS_NewClass installs the class def in the
   * given runtime's class table — and class tables don't survive
   * JS_FreeRuntime, so we must reinstall it on every fresh runtime
   * (i.e. every navigation), otherwise the next JS_NewObjectClass on
   * this id trips `class_id < class_count` in quickjs.c:2581. */
  if (dom_element_class_id == 0) {
    JS_NewClassID(rt, &dom_element_class_id);
  }
  if (!JS_IsRegisteredClass(rt, dom_element_class_id)) {
    JS_NewClass(rt, dom_element_class_id, &dom_element_class);
  }

  /* Element.prototype */
  JSValue proto = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, proto, dom_element_proto,
                             sizeof(dom_element_proto) /
                                 sizeof(dom_element_proto[0]));
  JS_SetClassProto(ctx, dom_element_class_id, proto);

  /* `document` is itself an Element wrapper pointing at the DOCUMENT
   * node — that makes the search methods reusable. The document gets
   * extra methods bolted on directly. */
  JSValue document = JS_NewObjectClass(ctx, dom_element_class_id);
  JS_SetOpaque(document, doc);

  JS_SetPropertyStr(ctx, document, "getElementById",
      JS_NewCFunction(ctx, doc_getElementById, "getElementById", 1));
  JS_SetPropertyStr(ctx, document, "getElementsByTagName",
      JS_NewCFunction(ctx, doc_getElementsByTagName,
                      "getElementsByTagName", 1));
  /* R6/B3: document.querySelector/All use the selector engine; the
   * Element prototype's matching helpers work on the document since
   * we wrap it with the same class id. */
  JS_SetPropertyStr(ctx, document, "querySelector",
      JS_NewCFunction(ctx, dom_el_querySelector,    "querySelector",    1));
  JS_SetPropertyStr(ctx, document, "querySelectorAll",
      JS_NewCFunction(ctx, dom_el_querySelectorAll, "querySelectorAll", 1));
  JS_SetPropertyStr(ctx, document, "createElement",
      JS_NewCFunction(ctx, doc_createElement, "createElement", 1));
  JS_SetPropertyStr(ctx, document, "createTextNode",
      JS_NewCFunction(ctx, doc_createTextNode, "createTextNode", 1));

  /* documentElement / body / head — populated from the tree. */
  dom_node_t *html = dom_get_first_element_by_tag(doc, "html");
  dom_node_t *body = dom_get_first_element_by_tag(doc, "body");
  dom_node_t *head = dom_get_first_element_by_tag(doc, "head");
  JS_SetPropertyStr(ctx, document, "documentElement",
                    html ? wrap_element(ctx, html) : JS_NULL);
  JS_SetPropertyStr(ctx, document, "body",
                    body ? wrap_element(ctx, body) : JS_NULL);
  JS_SetPropertyStr(ctx, document, "head",
                    head ? wrap_element(ctx, head) : JS_NULL);

  /* document.title — extracted at install time. The legacy `<title>`
   * read in dom_get_first_element_by_tag returns the element; we
   * concatenate its TEXT children. */
  const char *title = "";
  char title_buf[256] = {0};
  dom_node_t *tn = dom_get_first_element_by_tag(doc, "title");
  if (tn) {
    int w = 0;
    bool prev_ws = true;
    text_collect(tn, title_buf, &w, sizeof(title_buf), &prev_ws);
    while (w > 0 && title_buf[w - 1] == ' ') w--;
    title_buf[w] = 0;
    title = title_buf;
  }
  JS_SetPropertyStr(ctx, document, "title", JS_NewString(ctx, title));

  /* attach to globalThis */
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "document", document);
  JS_FreeValue(ctx, global);
}
