/* sdk/lib_qjs/qjs_fetch.c — fetch() over QuickJS (phase J5).
 *
 * Synchronous-under-the-hood implementation with a Promise-shaped API.
 * Plumbs through:
 *   no scheme → SYS_READ_FILE
 *   http://   → eq_http_get  (HTTP/1.1, 5 redirects, 20 s timeout)
 *   https://  → eq_http_get  (TLS via BearSSL, same defaults)
 *
 * Response model: { ok, status, url, _bodyPtr, _bodyLen, text() }.
 * _bodyPtr is a raw malloc'd char* stored as opaque on the Response
 * object; .text() reads it and copies into a JS string. Memory is
 * freed when the Response is finalized or .text() is called once
 * (whichever first — a Response can only yield text once for now;
 * good enough for J5).
 *
 * Both fetch() and Response.text() resolve synchronously — the work
 * is already done when they return. The Promise wrapping exists so
 * call sites match the real web API and so future async I/O can drop
 * in without surface changes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "equos.h"
#include "quickjs.h"
#include "qjs_fetch.h"
#include "http_client.h"

/* ------------------------------------------------------------------ */

/* Per-context configuration (trust anchors + the Response class id),
 * stashed on the runtime's user opaque. Multiple contexts on one
 * runtime share the class id once registered. */

static JSClassID fetch_response_class_id;

typedef struct {
  char    *url;       /* malloc'd, NUL-term */
  int      status;    /* 0 if local file, 200 if file read ok */
  char    *body;      /* malloc'd payload, NUL-padded */
  size_t   body_len;
} fetch_response_t;

static void fetch_response_finalizer(JSRuntime *rt, JSValue val) {
  (void)rt;
  fetch_response_t *r = JS_GetOpaque(val, fetch_response_class_id);
  if (!r) return;
  free(r->url);
  free(r->body);
  free(r);
}

static JSClassDef fetch_response_class = {
  "Response",
  .finalizer = fetch_response_finalizer,
};

/* Trust anchors stashed at install time. Single global is fine for now
 * — we only ever embed one runtime in jsdomtest/jsfetchtest/browser. */
static const struct br_x509_trust_anchor *g_tas;
static size_t g_tas_num;

/* ------------------------------------------------------------------ */
/* Response getters + methods                                          */
/* ------------------------------------------------------------------ */

static JSValue resp_get_ok(JSContext *ctx, JSValueConst this_val) {
  fetch_response_t *r = JS_GetOpaque(this_val, fetch_response_class_id);
  if (!r) return JS_FALSE;
  return (r->status >= 200 && r->status < 300) ? JS_TRUE : JS_FALSE;
}

static JSValue resp_get_status(JSContext *ctx, JSValueConst this_val) {
  fetch_response_t *r = JS_GetOpaque(this_val, fetch_response_class_id);
  return JS_NewInt32(ctx, r ? r->status : 0);
}

static JSValue resp_get_url(JSContext *ctx, JSValueConst this_val) {
  fetch_response_t *r = JS_GetOpaque(this_val, fetch_response_class_id);
  return JS_NewString(ctx, (r && r->url) ? r->url : "");
}

/* Helper: wrap a value/exception in an already-settled Promise. */
static JSValue settle_promise(JSContext *ctx, JSValue value, bool is_reject) {
  JSValue resolving[2];
  JSValue promise = JS_NewPromiseCapability(ctx, resolving);
  if (JS_IsException(promise)) { JS_FreeValue(ctx, value); return promise; }
  JSValue ret = JS_Call(ctx, resolving[is_reject ? 1 : 0], JS_UNDEFINED,
                        1, (JSValueConst *)&value);
  JS_FreeValue(ctx, ret);
  JS_FreeValue(ctx, value);
  JS_FreeValue(ctx, resolving[0]);
  JS_FreeValue(ctx, resolving[1]);
  return promise;
}

static JSValue resp_text(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
  (void)argc; (void)argv;
  fetch_response_t *r = JS_GetOpaque(this_val, fetch_response_class_id);
  if (!r || !r->body) {
    return settle_promise(ctx, JS_NewString(ctx, ""), false);
  }
  /* QuickJS strings are UTF-8 and zero-tolerant when length is given;
   * NUL bytes mid-body would be unusual for text/html, but use the
   * explicit-length API to be safe. JS_NewStringLen exists for this. */
  JSValue s = JS_NewStringLen(ctx, r->body, r->body_len);
  return settle_promise(ctx, s, false);
}

static const JSCFunctionListEntry resp_proto[] = {
  JS_CGETSET_DEF("ok",     resp_get_ok,     NULL),
  JS_CGETSET_DEF("status", resp_get_status, NULL),
  JS_CGETSET_DEF("url",    resp_get_url,    NULL),
  JS_CFUNC_DEF("text",     0, resp_text),
};

/* ------------------------------------------------------------------ */
/* fetch() body                                                        */
/* ------------------------------------------------------------------ */

static JSValue build_response_object(JSContext *ctx, fetch_response_t *r) {
  JSValue obj = JS_NewObjectClass(ctx, fetch_response_class_id);
  if (JS_IsException(obj)) return obj;
  JS_SetOpaque(obj, r);
  return obj;
}

static JSValue make_error(JSContext *ctx, const char *msg) {
  JSValue err = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg));
  return err;
}

static char *dup_cstr(const char *s) {
  size_t n = strlen(s) + 1;
  char *out = (char *)malloc(n);
  if (!out) return NULL;
  memcpy(out, s, n);
  return out;
}

static JSValue do_fetch_file(JSContext *ctx, const char *url) {
  uint32_t size = 0;
  char *raw = (char *)_syscall(SYS_READ_FILE, (uint64_t)url, (uint64_t)&size,
                               0, 0, 0);
  if (!raw) {
    return settle_promise(ctx, make_error(ctx, "fetch: file not found"), true);
  }
  /* Copy so we can free with stdlib free() symmetrically with http body. */
  char *body = (char *)malloc(size + 1);
  if (!body) {
    return settle_promise(ctx, make_error(ctx, "fetch: oom"), true);
  }
  memcpy(body, raw, size);
  body[size] = 0;

  fetch_response_t *r = (fetch_response_t *)calloc(1, sizeof(*r));
  if (!r) { free(body); return settle_promise(ctx, make_error(ctx, "fetch: oom"), true); }
  r->url      = dup_cstr(url);
  r->status   = 200;
  r->body     = body;
  r->body_len = size;
  return settle_promise(ctx, build_response_object(ctx, r), false);
}

static JSValue do_fetch_http(JSContext *ctx, const char *url) {
  eq_http_options_t opts = {0};
  opts.trust_anchors     = g_tas;
  opts.trust_anchors_num = g_tas_num;
  opts.follow_redirects  = EQ_HTTP_MAX_REDIRECTS_DEFAULT;
  opts.recv_timeout_ms   = EQ_HTTP_DEFAULT_RECV_TIMEOUT_MS;
  opts.body_limit_bytes  = EQ_HTTP_DEFAULT_BODY_LIMIT_BYTES;

  eq_http_response_t hr = {0};
  int rc = eq_http_get(url, &opts, &hr);
  if (rc != EQ_HTTP_OK) {
    char msg[96];
    snprintf(msg, sizeof(msg), "fetch: http error %d", rc);
    eq_http_response_free(&hr);
    return settle_promise(ctx, make_error(ctx, msg), true);
  }

  fetch_response_t *r = (fetch_response_t *)calloc(1, sizeof(*r));
  if (!r) { eq_http_response_free(&hr); return settle_promise(ctx, make_error(ctx, "fetch: oom"), true); }
  /* Take ownership of hr.body / hr.final_url so the response struct
   * doesn't double-free. eq_http_response_free zeros taken fields. */
  r->status   = hr.status_code;
  r->body     = hr.body;       hr.body = NULL;
  r->body_len = hr.body_len;   hr.body_len = 0;
  r->url      = hr.final_url ? hr.final_url : dup_cstr(url);
                              hr.final_url = NULL;
  eq_http_response_free(&hr);
  return settle_promise(ctx, build_response_object(ctx, r), false);
}

static JSValue js_fetch(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return settle_promise(ctx, make_error(ctx, "fetch: missing url"), true);
  }
  const char *url = JS_ToCString(ctx, argv[0]);
  if (!url) {
    return settle_promise(ctx, make_error(ctx, "fetch: bad url"), true);
  }

  JSValue result;
  if (strstr(url, "://") != NULL) {
    result = do_fetch_http(ctx, url);
  } else {
    result = do_fetch_file(ctx, url);
  }
  JS_FreeCString(ctx, url);
  return result;
}

/* ------------------------------------------------------------------ */
/* installation                                                        */
/* ------------------------------------------------------------------ */

void qjs_install_fetch(JSContext *ctx,
                       const struct br_x509_trust_anchor *tas,
                       size_t tas_num) {
  JSRuntime *rt = JS_GetRuntime(ctx);
  g_tas     = tas;
  g_tas_num = tas_num;

  /* See dom_js.c for the why: JS_NewClass lives in the runtime's
   * class table, which is wiped by JS_FreeRuntime — so re-install it
   * on every fresh runtime even though the class_id itself is
   * process-static. */
  if (fetch_response_class_id == 0) {
    JS_NewClassID(rt, &fetch_response_class_id);
  }
  if (!JS_IsRegisteredClass(rt, fetch_response_class_id)) {
    JS_NewClass(rt, fetch_response_class_id, &fetch_response_class);
  }
  JSValue proto = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, proto, resp_proto,
                             sizeof(resp_proto) / sizeof(resp_proto[0]));
  JS_SetClassProto(ctx, fetch_response_class_id, proto);

  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "fetch",
      JS_NewCFunction(ctx, js_fetch, "fetch", 1));
  JS_FreeValue(ctx, global);
}
