/* app/jstest.c — QuickJS smoke test (phases J1 + J2).
 *
 * J1: boot a runtime + context, evaluate `1+2` and string concat —
 *     proves the engine links and bytecode executes.
 *
 * J2: install `console.log` and verify the built-in `Math`, `Date`,
 *     and `JSON` globals work — QuickJS includes them by default,
 *     we just need to confirm they don't blow up under our toolchain.
 *
 * Expected output (J0+J1+J2):
 *   jstest: starting
 *   jstest: J1.a result = 3
 *   jstest: J1.b result = hello world
 *   jstest: J2 — running script via console.log + Math/Date/JSON
 *   hello from console.log 42 true
 *   Math.PI = 3.141592653589793
 *   Math.sqrt(2) = 1.4142135623730951
 *   Math.atan2(1,1) = 0.7853981633974483
 *   JSON: {"name":"equos","year":2026,"phases":7}
 *   parsed.name = equos
 *   typeof Date.now() = number
 *   jstest: done
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "quickjs.h"
#include "qjs_helpers.h"

/* Run one piece of JS, stringify and print the *return value*.
 * Used for the J1 smoke tests only — J2 scripts print via console.log. */
static int eval_and_print(JSContext *ctx, const char *src, const char *tag) {
  JSValue v = JS_Eval(ctx, src, strlen(src), tag, JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(v)) {
    printf("jstest: ERR: eval threw on '%s'\n", tag);
    qjs_dump_exception(ctx);
    JS_FreeValue(ctx, v);
    return -1;
  }
  const char *str = JS_ToCString(ctx, v);
  if (!str) {
    printf("jstest: ERR: ToCString failed for '%s'\n", tag);
    JS_FreeValue(ctx, v);
    return -1;
  }
  printf("jstest: %s result = %s\n", tag, str);
  JS_FreeCString(ctx, str);
  JS_FreeValue(ctx, v);
  return 0;
}

/* Run JS for its side effects (console.log etc). Return value is
 * discarded except for exception checking. */
static int eval_for_effect(JSContext *ctx, const char *src, const char *tag) {
  JSValue v = JS_Eval(ctx, src, strlen(src), tag, JS_EVAL_TYPE_GLOBAL);
  int rc = 0;
  if (JS_IsException(v)) {
    printf("jstest: ERR: eval threw on '%s'\n", tag);
    qjs_dump_exception(ctx);
    rc = -1;
  }
  JS_FreeValue(ctx, v);
  return rc;
}

/* The J2 script lives here as a C string so we don't need a file
 * loader in the host. Each line tests a different built-in. */
static const char j2_script[] =
  "console.log('hello from console.log', 42, true);\n"
  "console.log('Math.PI =', Math.PI);\n"
  "console.log('Math.sqrt(2) =', Math.sqrt(2));\n"
  "console.log('Math.atan2(1,1) =', Math.atan2(1, 1));\n"
  "var obj = { name: 'equos', year: 2026, phases: 7 };\n"
  "var s = JSON.stringify(obj);\n"
  "console.log('JSON:', s);\n"
  "var parsed = JSON.parse(s);\n"
  "console.log('parsed.name =', parsed.name);\n"
  "console.log('typeof Date.now() =', typeof Date.now());\n";

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("jstest: starting\n");

  JSRuntime *rt = JS_NewRuntime();
  if (!rt) { printf("jstest: ERR: JS_NewRuntime failed\n"); return 1; }

  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) {
    printf("jstest: ERR: JS_NewContext failed\n");
    JS_FreeRuntime(rt); return 1;
  }

  /* --- J1 — bytecode + tagged ints + string allocator -------------- */
  if (eval_and_print(ctx, "1 + 2",             "J1.a") != 0) goto fail;
  if (eval_and_print(ctx, "'hello ' + 'world'", "J1.b") != 0) goto fail;

  /* --- J2 — console.log + Math/Date/JSON --------------------------- */
  qjs_install_console(ctx);
  printf("jstest: J2 — running script via console.log + Math/Date/JSON\n");
  if (eval_for_effect(ctx, j2_script, "<J2>") != 0) goto fail;

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  printf("jstest: done\n");
  return 0;

fail:
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return 1;
}
