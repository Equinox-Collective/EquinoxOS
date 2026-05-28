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
#include "dom.h"
#include "dom_js.h"

/* ------------------------------------------------------------------ */

static JSClassID dom_element_class_id;

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

static const JSCFunctionListEntry dom_element_proto[] = {
  JS_CGETSET_DEF("tagName",      dom_el_get_tagName,     NULL),
  JS_CGETSET_DEF("id",           dom_el_get_id,          NULL),
  JS_CGETSET_DEF("className",    dom_el_get_className,   NULL),
  JS_CGETSET_DEF("textContent",  dom_el_get_textContent, NULL),
  JS_CGETSET_DEF("children",     dom_el_get_children,    NULL),
  JS_CGETSET_DEF("parentNode",   dom_el_get_parentNode,  NULL),
  JS_CFUNC_DEF("getAttribute",            1, dom_el_getAttribute),
  JS_CFUNC_DEF("hasAttribute",            1, dom_el_hasAttribute),
  JS_CFUNC_DEF("getElementsByTagName",    1, dom_el_getElementsByTagName),
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

  /* Register the Element class once per runtime. */
  if (dom_element_class_id == 0) {
    JS_NewClassID(rt, &dom_element_class_id);
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
