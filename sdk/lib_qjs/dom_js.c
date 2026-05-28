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

/* Public dispatcher. Builds a tiny synthetic Event object and passes
 * it as the single argument; supports type/target/preventDefault/
 * defaultPrevented. Bubbling and capture phases are not modelled. */
int qjs_dom_dispatch_event(JSContext *ctx, dom_node_t *target,
                           const char *name) {
  if (!ctx || !target || !name) return 0;
  int called = 0;
  for (el_listener_t *l = g_el_listeners; l; l = l->next) {
    if (l->target == target && strcmp(l->event, name) == 0) {
      JSValue ev = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, ev, "type",   JS_NewString(ctx, name));
      JS_SetPropertyStr(ctx, ev, "target", wrap_element(ctx, target));
      JS_SetPropertyStr(ctx, ev, "defaultPrevented", JS_FALSE);
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
      JS_FreeValue(ctx, ev);
      called++;
    }
  }
  return called;
}

void qjs_dom_teardown(JSContext *ctx) {
  el_listeners_reset(ctx);
  g_dom_dirty = 0;
}

/* --------------------------------------------------------------------
 * Element.prototype entries
 * ------------------------------------------------------------------ */

static const JSCFunctionListEntry dom_element_proto[] = {
  JS_CGETSET_DEF("tagName",      dom_el_get_tagName,     NULL),
  JS_CGETSET_DEF("id",           dom_el_get_id,          NULL),
  JS_CGETSET_DEF("className",    dom_el_get_className,   NULL),
  JS_CGETSET_DEF("textContent",  dom_el_get_textContent, dom_el_set_textContent),
  JS_CGETSET_DEF("innerHTML",    NULL,                   dom_el_set_innerHTML),
  JS_CGETSET_DEF("value",        dom_el_get_value,       dom_el_set_value),
  JS_CGETSET_DEF("children",     dom_el_get_children,    NULL),
  JS_CGETSET_DEF("parentNode",   dom_el_get_parentNode,  NULL),
  JS_CFUNC_DEF("getAttribute",            1, dom_el_getAttribute),
  JS_CFUNC_DEF("hasAttribute",            1, dom_el_hasAttribute),
  JS_CFUNC_DEF("getElementsByTagName",    1, dom_el_getElementsByTagName),
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

/* Minimal querySelector: only `#id` and bare `tag` selectors (good
 * enough for early scripts; full selector engine waits). */
static JSValue doc_querySelector(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  (void)argc;
  dom_node_t *root = unwrap_element(this_val);
  if (!root) return JS_NULL;
  const char *sel = JS_ToCString(ctx, argv[0]);
  if (!sel) return JS_EXCEPTION;
  dom_node_t *r = NULL;
  if (sel[0] == '#') {
    r = dom_get_element_by_id(root, sel + 1);
  } else {
    r = dom_get_first_element_by_tag(root, sel);
  }
  JS_FreeCString(ctx, sel);
  return r ? wrap_element(ctx, r) : JS_NULL;
}

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
  JS_SetPropertyStr(ctx, document, "querySelector",
      JS_NewCFunction(ctx, doc_querySelector, "querySelector", 1));
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
