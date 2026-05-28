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
#include "qjs_window.h"
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
                          const char *page_url,
                          const struct br_x509_trust_anchor *tas,
                          size_t tas_num) {
  if (!doc) return;
  script_index = 0;

  JSRuntime *rt = JS_NewRuntime();
  if (!rt) return;
  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) { JS_FreeRuntime(rt); return; }

  /* Order matters: console first (so install logs work); document
   * before window so the window pass can attach `document.location`;
   * fetch before scripts so they can call it; storage/timers/events
   * before scripts so listeners and setTimeout calls land in our
   * pools. */
  qjs_install_console(ctx);
  qjs_install_dom(ctx, doc);
  qjs_install_window(ctx, page_url);
  qjs_install_fetch(ctx, tas, tas_num);
  qjs_install_storage(ctx);
  qjs_install_timers(ctx);
  qjs_install_events(ctx);

  exec_scripts(ctx, doc);
  qjs_run_microtasks(ctx);

  /* Spec-ish load order: DOMContentLoaded → drain → timers → load.
   * Most pages don't care about the precise interleaving; we match
   * the common "DCL first, work, then load" expectation. */
  qjs_fire_DOMContentLoaded(ctx);
  qjs_drain_timers(ctx);
  qjs_fire_load(ctx);

  /* Drop any JSValues we parked in module-static state (event
   * listeners, leftover intervals) before the runtime is freed —
   * QuickJS asserts on a non-empty GC list at teardown. */
  qjs_window_teardown(ctx);
  qjs_dom_teardown(ctx);

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

/* ====================================================================
 * Persistent page session (phase R4 / F0)
 *
 * Lives across frames so widget events (button clicks, input change,
 * eventually form submit) can fire JS callbacks registered during the
 * initial script run. Created once per page load; freed before the
 * next load_page().
 * ================================================================== */

struct qjs_page {
  JSRuntime *rt;
  JSContext *ctx;
  /* R5/N0: last-known URL (== current_url at create-time). Compared
   * against globalThis.location.href after each event dispatch to
   * detect `location.href = "..."` JS-driven navigation. */
  char       url[512];
};

qjs_page_t *qjs_page_create(dom_node_t *doc,
                            const char *page_url,
                            const struct br_x509_trust_anchor *tas,
                            size_t tas_num) {
  if (!doc) return NULL;
  qjs_page_t *p = (qjs_page_t *)calloc(1, sizeof(*p));
  if (!p) return NULL;

  p->rt = JS_NewRuntime();
  if (!p->rt) { free(p); return NULL; }
  p->ctx = JS_NewContext(p->rt);
  if (!p->ctx) { JS_FreeRuntime(p->rt); free(p); return NULL; }

  script_index = 0;
  qjs_install_console(p->ctx);
  qjs_install_dom    (p->ctx, doc);
  qjs_install_window (p->ctx, page_url);
  if (page_url) {
    strncpy(p->url, page_url, sizeof(p->url) - 1);
    p->url[sizeof(p->url) - 1] = 0;
  }
  qjs_install_fetch  (p->ctx, tas, tas_num);
  qjs_install_storage(p->ctx);
  qjs_install_timers (p->ctx);
  qjs_install_events (p->ctx);

  exec_scripts(p->ctx, doc);
  qjs_run_microtasks(p->ctx);
  qjs_fire_DOMContentLoaded(p->ctx);
  qjs_drain_timers(p->ctx);
  qjs_fire_load(p->ctx);

  /* Initial scripts may have set the dirty flag; clear so the very
   * first frame doesn't unconditionally rebuild lines. */
  qjs_dom_consume_dirty();
  return p;
}

int qjs_page_dispatch_event(qjs_page_t *p, dom_node_t *target,
                            const char *name) {
  if (!p || !p->ctx) return 0;
  int n = qjs_dom_dispatch_event(p->ctx, target, name);
  /* Handlers commonly schedule microtasks (Promise.then etc); drain
   * so the user sees the result by the next paint. */
  qjs_run_microtasks(p->ctx);
  return n;
}

int qjs_page_take_nav(qjs_page_t *p, int *kind_out,
                      char *url_out, size_t url_len, int *delta_out) {
  if (!p || !p->ctx) return 0;

  /* 1. Explicit verbs (location.assign/replace/reload, history.*). */
  int    kind  = 0;
  int    delta = 0;
  char   url[512] = {0};
  if (qjs_window_take_nav(&kind, url, sizeof(url), &delta)) {
    if (kind_out)  *kind_out  = kind;
    if (delta_out) *delta_out = delta;
    if (url_out && url_len > 0) {
      strncpy(url_out, url, url_len - 1);
      url_out[url_len - 1] = 0;
    }
    /* Keep p->url in sync for the href-polling fallback so the next
     * call doesn't re-fire on the same change. We update it
     * optimistically for ASSIGN/REPLACE; RELOAD/HISTORY leave it
     * alone (the host resolves the target URL itself). */
    if ((kind == 1 || kind == 2) && url[0]) {
      strncpy(p->url, url, sizeof(p->url) - 1);
      p->url[sizeof(p->url) - 1] = 0;
    }
    return 1;
  }

  /* 2. Fallback: did anyone overwrite `location.href` directly? */
  JSValue global = JS_GetGlobalObject(p->ctx);
  JSValue loc    = JS_GetPropertyStr(p->ctx, global, "location");
  int navigated = 0;
  if (JS_IsObject(loc)) {
    JSValue href = JS_GetPropertyStr(p->ctx, loc, "href");
    const char *s = JS_ToCString(p->ctx, href);
    if (s && strcmp(s, p->url) != 0) {
      if (url_out && url_len > 0) {
        strncpy(url_out, s, url_len - 1);
        url_out[url_len - 1] = 0;
      }
      strncpy(p->url, s, sizeof(p->url) - 1);
      p->url[sizeof(p->url) - 1] = 0;
      if (kind_out)  *kind_out  = 1; /* ASSIGN */
      if (delta_out) *delta_out = 0;
      navigated = 1;
    }
    if (s) JS_FreeCString(p->ctx, s);
    JS_FreeValue(p->ctx, href);
  }
  JS_FreeValue(p->ctx, loc);
  JS_FreeValue(p->ctx, global);
  return navigated;
}

/* Back-compat wrapper around qjs_page_take_nav for callers that only
 * care about ASSIGN-style navigations. RELOAD and HISTORY are
 * collapsed to a no-op (caller will see them on the next take_nav). */
int qjs_page_pending_nav(qjs_page_t *p, char *out, size_t out_len) {
  int kind = 0, delta = 0;
  if (!qjs_page_take_nav(p, &kind, out, out_len, &delta)) return 0;
  return (kind == 1 || kind == 2) ? 1 : 0;
}

int qjs_page_consume_dirty(qjs_page_t *p) {
  (void)p;
  return qjs_dom_consume_dirty();
}

void qjs_page_free(qjs_page_t *p) {
  if (!p) return;
  if (p->ctx) {
    qjs_window_teardown(p->ctx);
    qjs_dom_teardown   (p->ctx);
    JS_FreeContext(p->ctx);
  }
  if (p->rt) JS_FreeRuntime(p->rt);
  free(p);
}
