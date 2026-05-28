/* app/jstest.c — phase J1 smoke test for the vendored QuickJS engine.
 *
 * Boots a JSRuntime + JSContext, evaluates a tiny expression, and
 * prints the result. The point of this app is link-level: it forces
 * libquickjs.a + the SDK shim into a real ELF and tells us whether
 * any symbols are missing on the freestanding cross.
 *
 * Expected output:
 *   jstest: starting
 *   jstest: result = 3
 *   jstest: result = "hello world"
 *   jstest: done
 *
 * If you see "ERR: JS_NewRuntime failed" the allocator is unhappy.
 * If you see "ERR: eval threw" the script raised an exception — we
 * print exception.toString().
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "quickjs.h"

/* Pull an exception out of the context and print it. Used after any
 * call that returns a JSValue with JS_IsException(v) == true. */
static void dump_exception(JSContext *ctx) {
  JSValue exc = JS_GetException(ctx);
  const char *msg = JS_ToCString(ctx, exc);
  if (msg) {
    printf("jstest: exception: %s\n", msg);
    JS_FreeCString(ctx, msg);
  } else {
    printf("jstest: exception (unprintable)\n");
  }
  JS_FreeValue(ctx, exc);
}

/* Run one piece of JS, print whatever it evaluated to. */
static int eval_and_print(JSContext *ctx, const char *src, const char *tag) {
  JSValue v = JS_Eval(ctx, src, strlen(src), tag, JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(v)) {
    printf("jstest: ERR: eval threw on '%s'\n", tag);
    dump_exception(ctx);
    JS_FreeValue(ctx, v);
    return -1;
  }

  /* Stringify whatever it returned — works for int, double, string. */
  const char *str = JS_ToCString(ctx, v);
  if (!str) {
    printf("jstest: ERR: ToCString failed for '%s'\n", tag);
    JS_FreeValue(ctx, v);
    return -1;
  }
  printf("jstest: result = %s\n", str);
  JS_FreeCString(ctx, str);
  JS_FreeValue(ctx, v);
  return 0;
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("jstest: starting\n");

  JSRuntime *rt = JS_NewRuntime();
  if (!rt) {
    printf("jstest: ERR: JS_NewRuntime failed\n");
    return 1;
  }

  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) {
    printf("jstest: ERR: JS_NewContext failed\n");
    JS_FreeRuntime(rt);
    return 1;
  }

  /* (a) integer arithmetic — exercises bytecode + tagged-pointer ints. */
  if (eval_and_print(ctx, "1 + 2", "<1+2>") != 0) {
    JS_FreeContext(ctx); JS_FreeRuntime(rt); return 1;
  }

  /* (b) string concat — exercises the string allocator + dtoa. */
  if (eval_and_print(ctx, "'hello ' + 'world'", "<concat>") != 0) {
    JS_FreeContext(ctx); JS_FreeRuntime(rt); return 1;
  }

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  printf("jstest: done\n");
  return 0;
}
