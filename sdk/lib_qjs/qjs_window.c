/* sdk/lib_qjs/qjs_window.c — phase J7 web globals.
 *
 * window + location, localStorage, timers, addEventListener for the
 * DOMContentLoaded/load duo. Strictly per-page (no persistence
 * across qjs_run_page_scripts() invocations); state is held in
 * module-static globals scoped to the lifetime of the single context
 * we know we own at a time. Multi-context safety is intentionally
 * skipped here — every page boots a fresh runtime + context anyway.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "equos.h"
#include "quickjs.h"
#include "qjs_helpers.h"
#include "qjs_window.h"

/* ====================================================================
 * 1. window + location
 * ================================================================== */

/* Parse `url` into scheme / host / pathname components. Missing
 * components come back as "". The returned strings live in `out`'s
 * fixed buffers — callers don't free them. */
typedef struct {
  char scheme[16];   /* "http", "https", "" for relative */
  char host[128];    /* "example.com[:port]" */
  char pathname[256];/* "/foo/bar?q=1#frag" or "" */
  char href[512];    /* original (truncated) */
} parsed_url_t;

static void parse_url(const char *url, parsed_url_t *out) {
  memset(out, 0, sizeof(*out));
  if (!url) { strcpy(out->href, "about:blank"); return; }
  strncpy(out->href, url, sizeof(out->href) - 1);

  const char *p = strstr(url, "://");
  const char *rest;
  if (p) {
    size_t sl = (size_t)(p - url);
    if (sl >= sizeof(out->scheme)) sl = sizeof(out->scheme) - 1;
    memcpy(out->scheme, url, sl); out->scheme[sl] = 0;
    rest = p + 3;
    const char *slash = strchr(rest, '/');
    if (slash) {
      size_t hl = (size_t)(slash - rest);
      if (hl >= sizeof(out->host)) hl = sizeof(out->host) - 1;
      memcpy(out->host, rest, hl); out->host[hl] = 0;
      strncpy(out->pathname, slash, sizeof(out->pathname) - 1);
    } else {
      strncpy(out->host, rest, sizeof(out->host) - 1);
      strcpy(out->pathname, "/");
    }
  } else {
    /* relative / file path */
    strncpy(out->pathname, url, sizeof(out->pathname) - 1);
  }
}

static JSValue loc_toString(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
  (void)argc; (void)argv;
  JSValue href = JS_GetPropertyStr(ctx, this_val, "href");
  return href; /* already a string */
}

/* ----- R5/N1: explicit nav requests (separate from href-polling) ----- */
typedef enum {
  NAV_NONE = 0,
  NAV_ASSIGN,   /* navigate, push history */
  NAV_REPLACE,  /* navigate, don't push */
  NAV_RELOAD,   /* reload current URL */
  NAV_HISTORY,  /* history.go(delta) */
} nav_kind_t;

static struct {
  nav_kind_t kind;
  char       url[512];
  int        delta;
} g_nav;

static void nav_clear(void) {
  g_nav.kind  = NAV_NONE;
  g_nav.url[0] = 0;
  g_nav.delta  = 0;
}

static void nav_set_url(nav_kind_t k, const char *url) {
  g_nav.kind = k;
  g_nav.delta = 0;
  if (url) {
    strncpy(g_nav.url, url, sizeof(g_nav.url) - 1);
    g_nav.url[sizeof(g_nav.url) - 1] = 0;
  } else {
    g_nav.url[0] = 0;
  }
}

int qjs_window_take_nav(int *kind, char *url_out, size_t url_len, int *delta_out) {
  if (g_nav.kind == NAV_NONE) return 0;
  if (kind) *kind = (int)g_nav.kind;
  if (url_out && url_len > 0) {
    strncpy(url_out, g_nav.url, url_len - 1);
    url_out[url_len - 1] = 0;
  }
  if (delta_out) *delta_out = g_nav.delta;
  nav_clear();
  return 1;
}

static JSValue js_loc_assign(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) return JS_UNDEFINED;
  const char *s = JS_ToCString(ctx, argv[0]);
  if (s) { nav_set_url(NAV_ASSIGN, s); JS_FreeCString(ctx, s); }
  return JS_UNDEFINED;
}
static JSValue js_loc_replace(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) return JS_UNDEFINED;
  const char *s = JS_ToCString(ctx, argv[0]);
  if (s) { nav_set_url(NAV_REPLACE, s); JS_FreeCString(ctx, s); }
  return JS_UNDEFINED;
}
static JSValue js_loc_reload(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
  (void)ctx; (void)this_val; (void)argc; (void)argv;
  nav_set_url(NAV_RELOAD, NULL);
  return JS_UNDEFINED;
}

static JSValue js_hist_go(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
  (void)this_val;
  int32_t delta = 0;
  if (argc >= 1) JS_ToInt32(ctx, &delta, argv[0]);
  g_nav.kind  = NAV_HISTORY;
  g_nav.delta = (int)delta;
  g_nav.url[0] = 0;
  return JS_UNDEFINED;
}
static JSValue js_hist_back(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
  (void)ctx; (void)this_val; (void)argc; (void)argv;
  g_nav.kind  = NAV_HISTORY;
  g_nav.delta = -1;
  g_nav.url[0] = 0;
  return JS_UNDEFINED;
}
static JSValue js_hist_forward(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
  (void)ctx; (void)this_val; (void)argc; (void)argv;
  g_nav.kind  = NAV_HISTORY;
  g_nav.delta = 1;
  g_nav.url[0] = 0;
  return JS_UNDEFINED;
}

/* history.length is filled in by the C side at install/refresh time —
 * we just expose a getter that reads a module-static int. The host
 * pokes it via qjs_window_set_history_length() after every nav. */
static int g_hist_len;
void qjs_window_set_history_length(int n) { g_hist_len = n; }
static JSValue js_hist_get_length(JSContext *ctx, JSValueConst this_val) {
  (void)this_val;
  return JS_NewInt32(ctx, g_hist_len);
}

void qjs_install_window(JSContext *ctx, const char *url) {
  parsed_url_t pu;
  parse_url(url, &pu);
  nav_clear();

  /* window === globalThis is the easiest thing humans expect; bind
   * the global object onto itself under both names. */
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));

  /* Build the location object. */
  JSValue loc = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, loc, "href",     JS_NewString(ctx, pu.href));
  JS_SetPropertyStr(ctx, loc, "protocol",
                    JS_NewString(ctx, pu.scheme[0] ? pu.scheme : ""));
  JS_SetPropertyStr(ctx, loc, "host",     JS_NewString(ctx, pu.host));
  JS_SetPropertyStr(ctx, loc, "hostname", JS_NewString(ctx, pu.host));
  JS_SetPropertyStr(ctx, loc, "pathname", JS_NewString(ctx, pu.pathname));
  JS_SetPropertyStr(ctx, loc, "toString",
      JS_NewCFunction(ctx, loc_toString, "toString", 0));
  /* R5/N1 explicit nav verbs. */
  JS_SetPropertyStr(ctx, loc, "assign",
      JS_NewCFunction(ctx, js_loc_assign,  "assign",  1));
  JS_SetPropertyStr(ctx, loc, "replace",
      JS_NewCFunction(ctx, js_loc_replace, "replace", 1));
  JS_SetPropertyStr(ctx, loc, "reload",
      JS_NewCFunction(ctx, js_loc_reload,  "reload",  0));

  JS_SetPropertyStr(ctx, global, "location", JS_DupValue(ctx, loc));

  /* R5/N1: window.history. We only model a back-stack, so .forward
   * is best-effort (works if the host tracked a high-water mark);
   * .length reads the host's current stack depth. */
  JSValue hist = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, hist, "back",
      JS_NewCFunction(ctx, js_hist_back,    "back",    0));
  JS_SetPropertyStr(ctx, hist, "forward",
      JS_NewCFunction(ctx, js_hist_forward, "forward", 0));
  JS_SetPropertyStr(ctx, hist, "go",
      JS_NewCFunction(ctx, js_hist_go,      "go",      1));
  /* length as a getter so JS sees the live count. */
  JSAtom len_atom = JS_NewAtom(ctx, "length");
  JSValue getter = JS_NewCFunction2(ctx, (JSCFunction *)js_hist_get_length,
                                    "get length", 0, JS_CFUNC_getter, 0);
  JS_DefinePropertyGetSet(ctx, hist, len_atom, getter, JS_UNDEFINED,
                          JS_PROP_CONFIGURABLE);
  JS_FreeAtom(ctx, len_atom);
  JS_SetPropertyStr(ctx, global, "history", hist);

  /* document.location too. */
  JSValue doc = JS_GetPropertyStr(ctx, global, "document");
  if (JS_IsObject(doc)) JS_SetPropertyStr(ctx, doc, "location", loc);
  else                  JS_FreeValue(ctx, loc);
  JS_FreeValue(ctx, doc);
  JS_FreeValue(ctx, global);
}

/* ====================================================================
 * 2. localStorage  (in-memory; per-page lifetime)
 * ================================================================== */

typedef struct ls_entry { char *key; char *val; struct ls_entry *next; } ls_entry_t;
static ls_entry_t *g_ls;

static void ls_reset(void) {
  ls_entry_t *e = g_ls;
  while (e) { ls_entry_t *n = e->next; free(e->key); free(e->val); free(e); e = n; }
  g_ls = NULL;
}

static char *dupstr(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char *o = (char *)malloc(n); if (o) memcpy(o, s, n);
  return o;
}

static JSValue ls_getItem(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
  (void)this_val; (void)argc;
  const char *k = JS_ToCString(ctx, argv[0]);
  if (!k) return JS_NULL;
  JSValue ret = JS_NULL;
  for (ls_entry_t *e = g_ls; e; e = e->next) {
    if (strcmp(e->key, k) == 0) { ret = JS_NewString(ctx, e->val); break; }
  }
  JS_FreeCString(ctx, k);
  return ret;
}

static JSValue ls_setItem(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc < 2) return JS_UNDEFINED;
  const char *k = JS_ToCString(ctx, argv[0]);
  const char *v = JS_ToCString(ctx, argv[1]);
  if (!k || !v) { if (k) JS_FreeCString(ctx, k); if (v) JS_FreeCString(ctx, v); return JS_UNDEFINED; }
  for (ls_entry_t *e = g_ls; e; e = e->next) {
    if (strcmp(e->key, k) == 0) {
      free(e->val); e->val = dupstr(v);
      JS_FreeCString(ctx, k); JS_FreeCString(ctx, v); return JS_UNDEFINED;
    }
  }
  ls_entry_t *e = (ls_entry_t *)malloc(sizeof(*e));
  if (e) { e->key = dupstr(k); e->val = dupstr(v); e->next = g_ls; g_ls = e; }
  JS_FreeCString(ctx, k); JS_FreeCString(ctx, v);
  return JS_UNDEFINED;
}

static JSValue ls_removeItem(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
  (void)this_val; (void)argc;
  const char *k = JS_ToCString(ctx, argv[0]);
  if (!k) return JS_UNDEFINED;
  ls_entry_t *prev = NULL;
  for (ls_entry_t *e = g_ls; e; prev = e, e = e->next) {
    if (strcmp(e->key, k) == 0) {
      if (prev) prev->next = e->next; else g_ls = e->next;
      free(e->key); free(e->val); free(e); break;
    }
  }
  JS_FreeCString(ctx, k);
  return JS_UNDEFINED;
}

static JSValue ls_clear(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
  (void)ctx; (void)this_val; (void)argc; (void)argv;
  ls_reset();
  return JS_UNDEFINED;
}

static JSValue ls_key(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv) {
  (void)this_val; (void)argc;
  int32_t i = 0;
  if (JS_ToInt32(ctx, &i, argv[0])) return JS_NULL;
  ls_entry_t *e = g_ls;
  while (e && i > 0) { e = e->next; i--; }
  return e ? JS_NewString(ctx, e->key) : JS_NULL;
}

static JSValue ls_get_length(JSContext *ctx, JSValueConst this_val) {
  (void)this_val;
  int n = 0;
  for (ls_entry_t *e = g_ls; e; e = e->next) n++;
  return JS_NewInt32(ctx, n);
}

static const JSCFunctionListEntry ls_proto[] = {
  JS_CFUNC_DEF("getItem",    1, ls_getItem),
  JS_CFUNC_DEF("setItem",    2, ls_setItem),
  JS_CFUNC_DEF("removeItem", 1, ls_removeItem),
  JS_CFUNC_DEF("clear",      0, ls_clear),
  JS_CFUNC_DEF("key",        1, ls_key),
  JS_CGETSET_DEF("length",   ls_get_length, NULL),
};

void qjs_install_storage(JSContext *ctx) {
  ls_reset();
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, obj, ls_proto,
                             sizeof(ls_proto) / sizeof(ls_proto[0]));
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "localStorage", obj);
  JS_FreeValue(ctx, global);
}

/* ====================================================================
 * 3. timers  (setTimeout / setInterval; relative-ordering only)
 * ================================================================== */

typedef struct timer {
  int      id;
  uint64_t due;          /* virtual time, not wall-clock */
  uint64_t interval;     /* 0 → one-shot */
  JSValue  fn;
  struct timer *next;
} timer_t;

static timer_t *g_timers;
static int      g_next_timer_id = 1;
static uint64_t g_vclock;          /* monotonic virtual ms */

static void timers_reset(JSContext *ctx) {
  timer_t *t = g_timers;
  while (t) { timer_t *n = t->next; JS_FreeValue(ctx, t->fn); free(t); t = n; }
  g_timers = NULL;
  g_next_timer_id = 1;
  g_vclock = 0;
}

static JSValue set_timer_common(JSContext *ctx, int argc, JSValueConst *argv,
                                bool repeat) {
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
  int32_t delay = 0;
  if (argc >= 2) JS_ToInt32(ctx, &delay, argv[1]);
  if (delay < 0) delay = 0;
  timer_t *t = (timer_t *)malloc(sizeof(*t));
  if (!t) return JS_NewInt32(ctx, 0);
  t->id       = g_next_timer_id++;
  t->due      = g_vclock + (uint64_t)delay;
  t->interval = repeat ? (uint64_t)delay : 0;
  t->fn       = JS_DupValue(ctx, argv[0]);
  t->next     = g_timers;
  g_timers    = t;
  return JS_NewInt32(ctx, t->id);
}

static JSValue js_setTimeout(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
  (void)this_val;
  return set_timer_common(ctx, argc, argv, false);
}
static JSValue js_setInterval(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
  (void)this_val;
  return set_timer_common(ctx, argc, argv, true);
}
static JSValue js_clearTimer(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
  (void)this_val; (void)argc;
  int32_t id = 0;
  JS_ToInt32(ctx, &id, argv[0]);
  timer_t *prev = NULL;
  for (timer_t *t = g_timers; t; prev = t, t = t->next) {
    if (t->id == id) {
      if (prev) prev->next = t->next; else g_timers = t->next;
      JS_FreeValue(ctx, t->fn); free(t); break;
    }
  }
  return JS_UNDEFINED;
}

void qjs_install_timers(JSContext *ctx) {
  timers_reset(ctx);
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "setTimeout",
      JS_NewCFunction(ctx, js_setTimeout, "setTimeout", 2));
  JS_SetPropertyStr(ctx, global, "setInterval",
      JS_NewCFunction(ctx, js_setInterval, "setInterval", 2));
  JS_SetPropertyStr(ctx, global, "clearTimeout",
      JS_NewCFunction(ctx, js_clearTimer, "clearTimeout", 1));
  JS_SetPropertyStr(ctx, global, "clearInterval",
      JS_NewCFunction(ctx, js_clearTimer, "clearInterval", 1));
  JS_FreeValue(ctx, global);
}

void qjs_drain_timers(JSContext *ctx) {
  /* Cap to keep a runaway setInterval from hanging the load.
   * 1024 callbacks per page is plenty for sane payloads. */
  for (int iter = 0; iter < 1024 && g_timers; iter++) {
    /* Pick the timer with the smallest due. Stable on ties (head wins
     * because we walk forward and only swap on strictly less). */
    timer_t *min = g_timers;
    for (timer_t *t = g_timers->next; t; t = t->next) {
      if (t->due < min->due) min = t;
    }
    g_vclock = min->due;

    JSValue ret = JS_Call(ctx, min->fn, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(ret)) qjs_dump_exception(ctx);
    JS_FreeValue(ctx, ret);
    qjs_run_microtasks(ctx);

    if (min->interval) {
      min->due = g_vclock + min->interval;
      /* and leave it in the list — will fire again */
    } else {
      /* unlink min */
      timer_t *prev = NULL;
      for (timer_t *t = g_timers; t; prev = t, t = t->next) {
        if (t == min) {
          if (prev) prev->next = t->next; else g_timers = t->next;
          break;
        }
      }
      JS_FreeValue(ctx, min->fn);
      free(min);
    }
  }
  /* Any leftover intervals: drop them silently. */
  timers_reset(ctx);
}

/* R5/N5: tick variant. Run all timers due by `now_ms`, reschedule
 * intervals, leave the remaining list intact. Used by the browser
 * once per frame so setInterval can drive animations and live UI. */
void qjs_window_tick_timers(JSContext *ctx, uint64_t now_ms) {
  /* Move virtual clock forward (but never backward — host clock can
   * stall briefly across long script runs). */
  if (now_ms > g_vclock) g_vclock = now_ms;

  /* Per-frame fairness cap — never run more than 64 callbacks per
   * tick so a runaway setInterval can't starve the render loop. */
  for (int iter = 0; iter < 64; iter++) {
    timer_t *min = NULL;
    for (timer_t *t = g_timers; t; t = t->next) {
      if (t->due > g_vclock) continue;
      if (!min || t->due < min->due) min = t;
    }
    if (!min) break;

    JSValue ret = JS_Call(ctx, min->fn, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(ret)) qjs_dump_exception(ctx);
    JS_FreeValue(ctx, ret);
    qjs_run_microtasks(ctx);

    if (min->interval) {
      min->due += min->interval;
      /* If we're way behind (paused tab equivalent), don't fire the
       * same interval more than once per tick — push due past now. */
      if (min->due <= g_vclock) min->due = g_vclock + min->interval;
    } else {
      timer_t *prev = NULL;
      for (timer_t *t = g_timers; t; prev = t, t = t->next) {
        if (t == min) {
          if (prev) prev->next = t->next; else g_timers = t->next;
          break;
        }
      }
      JS_FreeValue(ctx, min->fn);
      free(min);
    }
  }
}

/* ====================================================================
 * 4. addEventListener for DOMContentLoaded / load
 * ================================================================== */

typedef struct evlistener {
  char *event;
  JSValue fn;
  struct evlistener *next;
} evlistener_t;

static evlistener_t *g_listeners;

static void ev_reset(JSContext *ctx) {
  evlistener_t *e = g_listeners;
  while (e) {
    evlistener_t *n = e->next;
    JS_FreeValue(ctx, e->fn);
    free(e->event); free(e); e = n;
  }
  g_listeners = NULL;
}

static JSValue js_addEventListener(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc < 2) return JS_UNDEFINED;
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) return JS_UNDEFINED;
  if (!JS_IsFunction(ctx, argv[1])) {
    JS_FreeCString(ctx, name); return JS_UNDEFINED;
  }
  evlistener_t *l = (evlistener_t *)malloc(sizeof(*l));
  if (l) {
    l->event = dupstr(name);
    l->fn    = JS_DupValue(ctx, argv[1]);
    l->next  = g_listeners;
    g_listeners = l;
  }
  JS_FreeCString(ctx, name);
  return JS_UNDEFINED;
}

static JSValue js_removeEventListener(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc < 2) return JS_UNDEFINED;
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) return JS_UNDEFINED;
  evlistener_t *prev = NULL;
  for (evlistener_t *l = g_listeners; l; prev = l, l = l->next) {
    if (strcmp(l->event, name) == 0 &&
        JS_VALUE_GET_PTR(l->fn) == JS_VALUE_GET_PTR(argv[1])) {
      if (prev) prev->next = l->next; else g_listeners = l->next;
      JS_FreeValue(ctx, l->fn);
      free(l->event); free(l); break;
    }
  }
  JS_FreeCString(ctx, name);
  return JS_UNDEFINED;
}

void qjs_install_events(JSContext *ctx) {
  ev_reset(ctx);
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "addEventListener",
      JS_NewCFunction(ctx, js_addEventListener, "addEventListener", 2));
  JS_SetPropertyStr(ctx, global, "removeEventListener",
      JS_NewCFunction(ctx, js_removeEventListener, "removeEventListener", 2));
  /* document.addEventListener is just an alias — same listener pool;
   * the spec distinction (document vs window) isn't useful at our scale. */
  JSValue doc = JS_GetPropertyStr(ctx, global, "document");
  if (JS_IsObject(doc)) {
    JS_SetPropertyStr(ctx, doc, "addEventListener",
        JS_NewCFunction(ctx, js_addEventListener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, doc, "removeEventListener",
        JS_NewCFunction(ctx, js_removeEventListener, "removeEventListener", 2));
  }
  JS_FreeValue(ctx, doc);
  JS_FreeValue(ctx, global);
}

static void fire_one(JSContext *ctx, const char *name) {
  for (evlistener_t *l = g_listeners; l; l = l->next) {
    if (strcmp(l->event, name) == 0) {
      JSValue ret = JS_Call(ctx, l->fn, JS_UNDEFINED, 0, NULL);
      if (JS_IsException(ret)) qjs_dump_exception(ctx);
      JS_FreeValue(ctx, ret);
    }
  }
}

void qjs_fire_DOMContentLoaded(JSContext *ctx) {
  fire_one(ctx, "DOMContentLoaded");
  qjs_run_microtasks(ctx);
}
void qjs_fire_load(JSContext *ctx) {
  fire_one(ctx, "load");
  qjs_run_microtasks(ctx);
}
void qjs_fire_loaded_events(JSContext *ctx) {
  /* Legacy single-shot: still useful for callers that don't care
   * about ordering with timers. */
  qjs_fire_DOMContentLoaded(ctx);
  qjs_fire_load(ctx);
}

/* ====================================================================
 * R6/B3: window.scrollY + window.scrollTo + IntersectionObserver stub
 *
 * scrollY is a live getter pulled from a value the host sets each
 * frame (`qjs_window_set_scroll_y`). scrollTo writes a pending
 * request that the host polls via `qjs_window_take_scroll`.
 *
 * IntersectionObserver: real impl needs layout. We don't have layout.
 * The site (and most reveal-on-scroll setups) has an explicit
 * fallback path keyed on `'IntersectionObserver' in window`, so the
 * cleanest thing is to *not* install it — the fallback then runs
 * unconditionally and shows every revealable section straight away.
 * ================================================================== */

static int g_scroll_y     = 0;
static int g_pending_scroll = -1;   /* -1 = none, else target Y in px */

void qjs_window_set_scroll_y(int y) { g_scroll_y = y; }
int  qjs_window_take_scroll(int *out_y) {
  if (g_pending_scroll < 0) return 0;
  if (out_y) *out_y = g_pending_scroll;
  g_pending_scroll = -1;
  return 1;
}

static JSValue js_win_get_scrollY(JSContext *ctx, JSValueConst this_val) {
  (void)this_val;
  return JS_NewInt32(ctx, g_scroll_y);
}

static JSValue js_win_get_scrollX(JSContext *ctx, JSValueConst this_val) {
  (void)ctx; (void)this_val;
  return JS_NewInt32(ctx, 0);
}

static JSValue js_win_scrollTo(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
  (void)this_val;
  int y = 0;
  if (argc >= 1 && JS_IsObject(argv[0]) && !JS_IsNull(argv[0])) {
    JSValue top = JS_GetPropertyStr(ctx, argv[0], "top");
    int32_t v = 0;
    if (!JS_IsUndefined(top)) JS_ToInt32(ctx, &v, top);
    JS_FreeValue(ctx, top);
    y = v;
  } else if (argc >= 2) {
    int32_t v = 0;
    JS_ToInt32(ctx, &v, argv[1]);
    y = v;
  } else if (argc >= 1) {
    int32_t v = 0;
    JS_ToInt32(ctx, &v, argv[0]);
    y = v;
  }
  if (y < 0) y = 0;
  g_pending_scroll = y;
  return JS_UNDEFINED;
}

static JSValue js_win_scrollBy(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
  (void)this_val;
  int dy = 0;
  if (argc >= 1 && JS_IsObject(argv[0]) && !JS_IsNull(argv[0])) {
    JSValue t = JS_GetPropertyStr(ctx, argv[0], "top");
    int32_t v = 0;
    if (!JS_IsUndefined(t)) JS_ToInt32(ctx, &v, t);
    JS_FreeValue(ctx, t);
    dy = v;
  } else if (argc >= 2) {
    int32_t v = 0; JS_ToInt32(ctx, &v, argv[1]); dy = v;
  }
  int y = g_scroll_y + dy;
  if (y < 0) y = 0;
  g_pending_scroll = y;
  return JS_UNDEFINED;
}

/* getComputedStyle stub — returns an empty object so accesses like
 * .getPropertyValue('--foo') won't crash. */
static JSValue js_getPropertyValue(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)ctx; (void)this_val; (void)argc; (void)argv;
  return JS_NewString(ctx, "");
}
static JSValue js_getComputedStyle(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)this_val; (void)argc; (void)argv;
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "getPropertyValue",
      JS_NewCFunction(ctx, js_getPropertyValue, "getPropertyValue", 1));
  return o;
}

void qjs_install_scroll(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);

  JSAtom a;
  a = JS_NewAtom(ctx, "scrollY");
  JS_DefinePropertyGetSet(ctx, global, a,
      JS_NewCFunction2(ctx, (JSCFunction *)js_win_get_scrollY,
                       "get scrollY", 0, JS_CFUNC_getter, 0),
      JS_UNDEFINED, JS_PROP_CONFIGURABLE);
  JS_FreeAtom(ctx, a);

  a = JS_NewAtom(ctx, "pageYOffset");
  JS_DefinePropertyGetSet(ctx, global, a,
      JS_NewCFunction2(ctx, (JSCFunction *)js_win_get_scrollY,
                       "get pageYOffset", 0, JS_CFUNC_getter, 0),
      JS_UNDEFINED, JS_PROP_CONFIGURABLE);
  JS_FreeAtom(ctx, a);

  a = JS_NewAtom(ctx, "scrollX");
  JS_DefinePropertyGetSet(ctx, global, a,
      JS_NewCFunction2(ctx, (JSCFunction *)js_win_get_scrollX,
                       "get scrollX", 0, JS_CFUNC_getter, 0),
      JS_UNDEFINED, JS_PROP_CONFIGURABLE);
  JS_FreeAtom(ctx, a);

  JS_SetPropertyStr(ctx, global, "scrollTo",
      JS_NewCFunction(ctx, js_win_scrollTo, "scrollTo", 1));
  JS_SetPropertyStr(ctx, global, "scrollBy",
      JS_NewCFunction(ctx, js_win_scrollBy, "scrollBy", 1));
  JS_SetPropertyStr(ctx, global, "getComputedStyle",
      JS_NewCFunction(ctx, js_getComputedStyle, "getComputedStyle", 1));

  /* innerWidth/innerHeight — many pages branch on these. Defaults
   * track the EquinoxOS browser viewport. */
  JS_SetPropertyStr(ctx, global, "innerWidth",  JS_NewInt32(ctx, 640));
  JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, 420));
  JS_SetPropertyStr(ctx, global, "devicePixelRatio", JS_NewFloat64(ctx, 1.0));

  JS_FreeValue(ctx, global);
}

/* ====================================================================
 * R6/B1: navigator
 * ================================================================== */

void qjs_install_navigator(JSContext *ctx, const char *lang) {
  if (!lang || !*lang) lang = "en";
  /* Truncate to bare language tag; many call sites do
   * navigator.language.slice(0,2) anyway, but giving them a clean
   * "en"/"ru" avoids `en-US` surprises in switch-cases. */
  char tag[8]; int i;
  for (i = 0; i < 7 && lang[i] && lang[i] != ',' && lang[i] != ';'; i++)
    tag[i] = lang[i];
  tag[i] = '\0';

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue nav = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, nav, "language",  JS_NewString(ctx, tag));
  JS_SetPropertyStr(ctx, nav, "userAgent",
                    JS_NewString(ctx,
      "Mozilla/5.0 (EquinoxOS; x86_64) EQX/0.1 like Gecko"));
  JS_SetPropertyStr(ctx, nav, "appName",   JS_NewString(ctx, "Netscape"));
  JS_SetPropertyStr(ctx, nav, "appVersion",JS_NewString(ctx, "5.0 (EquinoxOS)"));
  JS_SetPropertyStr(ctx, nav, "platform",  JS_NewString(ctx, "EquinoxOS"));
  JS_SetPropertyStr(ctx, nav, "vendor",    JS_NewString(ctx, "Equinox Collective"));
  JS_SetPropertyStr(ctx, nav, "product",   JS_NewString(ctx, "Gecko"));
  JS_SetPropertyStr(ctx, nav, "onLine",    JS_TRUE);
  JS_SetPropertyStr(ctx, nav, "cookieEnabled", JS_FALSE);
  JS_SetPropertyStr(ctx, nav, "doNotTrack",    JS_NULL);

  /* navigator.languages = [tag] — 1-element array is enough for the
   * usual `(navigator.languages||[]).map(...)` idioms. */
  JSValue langs = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, langs, 0, JS_NewString(ctx, tag));
  JS_SetPropertyStr(ctx, nav, "languages", langs);

  JS_SetPropertyStr(ctx, global, "navigator", nav);
  JS_FreeValue(ctx, global);
}

/* ====================================================================
 * Teardown
 * ================================================================== */

void qjs_window_teardown(JSContext *ctx) {
  /* Order doesn't matter — each helper only touches its own pool.
   * What matters is that every JSValue we ever stored in C state
   * gets JS_FreeValue'd before the runtime is dropped. */
  ev_reset(ctx);
  timers_reset(ctx);
  ls_reset();   /* strings only, no JSValues, but keep the pool clean */
}
