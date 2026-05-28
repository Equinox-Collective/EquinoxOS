/* sdk/lib_qjs/qjs_page.c — run-scripts-on-load pass (phase J6a). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "equos.h"
#include "quickjs.h"
#include "dom.h"
#include "qjs_helpers.h"
#include "qjs_fetch.h"
#include "dom_js.h"
#include "qjs_page.h"

/* Concatenate every TEXT-node child of `n` into a freshly-malloc'd
 * NUL-terminated buffer. Returns NULL on OOM (or empty body — caller
 * treats NULL == "nothing to run"). */
static char *collect_script_text(dom_node_t *n) {
  size_t total = 1; /* NUL */
  for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
    if (c->type == DOM_NODE_TEXT && c->text) total += strlen(c->text);
  }
  if (total == 1) return NULL;
  char *buf = (char *)malloc(total);
  if (!buf) return NULL;
  size_t w = 0;
  for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
    if (c->type == DOM_NODE_TEXT && c->text) {
      size_t l = strlen(c->text);
      memcpy(buf + w, c->text, l);
      w += l;
    }
  }
  buf[w] = 0;
  return buf;
}

/* Walks the DOM and runs every <script> element in document order.
 * Inline scripts are evaluated directly; <script src="..."> is loaded
 * via a synchronous helper that calls the globally-installed fetch()
 * indirectly through a tiny shim eval (avoids us duplicating the
 * scheme dispatch). */
static int script_index;

static void run_inline(JSContext *ctx, const char *src, const char *tag) {
  JSValue r = JS_Eval(ctx, src, strlen(src), tag, JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) qjs_dump_exception(ctx);
  JS_FreeValue(ctx, r);
  /* Drain microtasks per-script so order-of-execution between sibling
   * scripts matches what the browser would do. */
  qjs_run_microtasks(ctx);
}

static void exec_scripts(JSContext *ctx, dom_node_t *n) {
  if (!n) return;
  if (n->type == DOM_NODE_ELEMENT && n->tag_name &&
      strcmp(n->tag_name, "script") == 0) {
    const char *type = dom_get_attr(n, "type");
    /* Treat unset / empty / "text/javascript" / "application/javascript"
     * / "module" as JS. Anything else (e.g. "application/json",
     * "text/plain") is skipped. */
    bool is_js = true;
    if (type && *type) {
      if (strcmp(type, "text/javascript") != 0 &&
          strcmp(type, "application/javascript") != 0 &&
          strcmp(type, "module") != 0) {
        is_js = false;
      }
    }
    if (is_js) {
      const char *src_attr = dom_get_attr(n, "src");
      script_index++;
      char tag[40];
      snprintf(tag, sizeof(tag), "<script:%d>", script_index);

      if (src_attr && *src_attr) {
        /* Use the page's own fetch binding to load the script body,
         * then JS_Eval the resolved text. Doing it through globalThis
         * keeps a single code path for scheme dispatch and lets the
         * user override fetch if they want. The `await` syntax inside
         * a module-shaped wrapper is the easiest way to block on it
         * synchronously from C, *but* we've already drained microtasks
         * by the time we exit run_inline below — so a plain
         * Promise+microtask spin works too. */
        char shim[256];
        snprintf(shim, sizeof(shim),
                 "globalThis.__scriptText = null;"
                 "fetch(%c%s%c).then(r => r.text())"
                 ".then(t => { globalThis.__scriptText = t; })"
                 ".catch(e => { console.error('script %s: ' + (e&&e.message)); });",
                 '"', src_attr, '"', src_attr);
        run_inline(ctx, shim, tag);
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue v = JS_GetPropertyStr(ctx, g, "__scriptText");
        if (JS_IsString(v)) {
          const char *body = JS_ToCString(ctx, v);
          if (body) {
            run_inline(ctx, body, tag);
            JS_FreeCString(ctx, body);
          }
        }
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, g);
      } else {
        char *body = collect_script_text(n);
        if (body) {
          run_inline(ctx, body, tag);
          free(body);
        }
      }
    }
    /* don't descend into script children */
    return;
  }
  for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
    exec_scripts(ctx, c);
  }
}

void qjs_run_page_scripts(dom_node_t *doc,
                          const struct br_x509_trust_anchor *tas,
                          size_t tas_num) {
  if (!doc) return;
  script_index = 0;

  JSRuntime *rt = JS_NewRuntime();
  if (!rt) return;
  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) { JS_FreeRuntime(rt); return; }

  qjs_install_console(ctx);
  qjs_install_dom(ctx, doc);
  qjs_install_fetch(ctx, tas, tas_num);

  exec_scripts(ctx, doc);
  qjs_run_microtasks(ctx);

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}
