/* sdk/lib_qjs/qjs_helpers.c — host bindings for embedded QuickJS.
 * See sdk/include/qjs_helpers.h for the API contract.
 */

#include <stdio.h>
#include <string.h>

#include "quickjs.h"
#include "qjs_helpers.h"

/* ---------------------------------------------------------------------------
 * console.{log,warn,error,info,debug}
 *
 * All 5 methods use the same C implementation — EquinoxOS doesn't yet
 * route stderr separately from stdout and there's no log-level routing,
 * so the only thing that matters is that the methods exist and print
 * their arguments space-separated.
 * ------------------------------------------------------------------------ */

static JSValue js_console_print(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
  (void)this_val;
  for (int i = 0; i < argc; i++) {
    if (i > 0) putchar(' ');
    const char *s = JS_ToCString(ctx, argv[i]);
    if (s) {
      /* fputs/printf would walk the string anyway — just write via printf
       * for portability with our SDK stdio (no buffered streams beyond
       * stdout). %s is fine even for very long strings.  */
      printf("%s", s);
      JS_FreeCString(ctx, s);
    } else {
      printf("<?>");
    }
  }
  putchar('\n');
  return JS_UNDEFINED;
}

void qjs_install_console(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue console = JS_NewObject(ctx);

  /* All 5 method names point at the same C function — for our purposes
   * they're all "write to stdout". Real differentiation can come later. */
  static const char *const methods[] = {
    "log", "warn", "error", "info", "debug",
  };
  for (unsigned i = 0; i < sizeof(methods)/sizeof(methods[0]); i++) {
    JSValue fn = JS_NewCFunction(ctx, js_console_print, methods[i], 1);
    JS_SetPropertyStr(ctx, console, methods[i], fn);
  }

  JS_SetPropertyStr(ctx, global, "console", console);
  JS_FreeValue(ctx, global);
}

/* ---------------------------------------------------------------------------
 * Exception dumper
 *
 * QuickJS exceptions are normal Error objects with a .stack property.
 * Print both .toString() and .stack when available. Returns to the
 * caller with the context's exception slot cleared (JS_GetException
 * does that as a side effect).
 * ------------------------------------------------------------------------ */

void qjs_dump_exception(JSContext *ctx) {
  JSValue exc = JS_GetException(ctx);

  const char *msg = JS_ToCString(ctx, exc);
  if (msg) {
    printf("exception: %s\n", msg);
    JS_FreeCString(ctx, msg);
  } else {
    printf("exception: <unprintable>\n");
  }

  /* If the exception has a .stack, print it too. Non-Error throws
   * (e.g. `throw 42`) won't have one — that's fine, we just skip. */
  if (JS_IsError(exc)) {
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsUndefined(stack)) {
      const char *s = JS_ToCString(ctx, stack);
      if (s) {
        printf("  stack: %s\n", s);
        JS_FreeCString(ctx, s);
      }
    }
    JS_FreeValue(ctx, stack);
  }

  JS_FreeValue(ctx, exc);
}

/* ---------------------------------------------------------------------------
 * Microtask pump
 *
 * Promise then-chains run as queued jobs in QuickJS. After a top-level
 * eval that involves promises, jobs sit on the runtime's queue until
 * something drains them. We loop calling JS_ExecutePendingJob until
 * the queue is empty (return value 0) or a job throws (<0).
 * ------------------------------------------------------------------------ */

int qjs_run_microtasks(JSContext *ctx) {
  JSRuntime *rt = JS_GetRuntime(ctx);
  for (;;) {
    JSContext *job_ctx = NULL;
    int r = JS_ExecutePendingJob(rt, &job_ctx);
    if (r == 0) return 0;        /* idle: no jobs left */
    if (r < 0) {                 /* a job threw — dump + bail */
      if (job_ctx) qjs_dump_exception(job_ctx);
      return -1;
    }
    /* r > 0: we ran a job, loop and try the next. */
  }
}
