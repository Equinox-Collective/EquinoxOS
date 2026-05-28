/* app/jsfetchtest.c — phase J5 smoke test.
 *
 * Exercises:
 *   - fetch('res/index.html')  → Promise<Response>
 *   - response.ok / .status / .url / .text()
 *   - .then() chain (verifies microtask pump)
 *   - Promise.all over two parallel fetches
 *
 * No network needed — both URLs are local files. Network fetch is
 * exercised by browser.elf in J6.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "equos.h"
#include "quickjs.h"
#include "qjs_helpers.h"
#include "qjs_fetch.h"

static void eval_dump(JSContext *ctx, const char *src, const char *tag) {
  JSValue r = JS_Eval(ctx, src, strlen(src), tag, JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) qjs_dump_exception(ctx);
  JS_FreeValue(ctx, r);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("jsfetchtest: starting\n");

  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);

  qjs_install_console(ctx);
  /* No trust anchors needed — we only touch local files here. */
  qjs_install_fetch(ctx, NULL, 0);

  const char *script =
    /* Use globalThis so C can read the counter back — `let` would
     * scope the binding to the eval and stay invisible from outside. */
    "globalThis.done = 0;\n"
    "fetch('res/index.html').then(r => {\n"
    "  console.log('A status =', r.status);\n"
    "  console.log('A ok =', r.ok);\n"
    "  console.log('A url =', r.url);\n"
    "  return r.text();\n"
    "}).then(t => {\n"
    "  console.log('A text length =', t.length);\n"
    "  console.log('A starts with <!doctype =',\n"
    "              t.substring(0, 9).toLowerCase() === '<!doctype');\n"
    "  globalThis.done++;\n"
    "}).catch(e => { console.error('A failed:', e && e.message); });\n"
    "\n"
    "Promise.all([\n"
    "  fetch('res/index.html').then(r => r.text()),\n"
    "  fetch('res/index.html').then(r => r.text()),\n"
    "]).then(([a, b]) => {\n"
    "  console.log('B both equal =', a === b);\n"
    "  console.log('B length =', a.length);\n"
    "  globalThis.done++;\n"
    "}).catch(e => { console.error('B failed:', e && e.message); });\n"
    "\n"
    "fetch('res/does_not_exist.html').then(r => {\n"
    "  console.error('C unexpectedly resolved, status=' + r.status);\n"
    "}).catch(e => {\n"
    "  console.log('C correctly rejected:', e && e.message);\n"
    "  globalThis.done++;\n"
    "});\n";

  eval_dump(ctx, script, "<jsfetchtest>");
  if (qjs_run_microtasks(ctx) < 0) {
    printf("jsfetchtest: microtask drain reported an exception\n");
  }

  /* Read `done` to confirm all three test arms ran their tail then(). */
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue v = JS_GetPropertyStr(ctx, global, "done");
  int32_t done = 0;
  JS_ToInt32(ctx, &done, v);
  JS_FreeValue(ctx, v);
  JS_FreeValue(ctx, global);
  printf("jsfetchtest: done counter = %d (expected 3)\n", (int)done);

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  printf("jsfetchtest: done\n");
  return 0;
}
