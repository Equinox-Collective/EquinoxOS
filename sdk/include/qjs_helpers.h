/* sdk/include/qjs_helpers.h — host helpers for embedding QuickJS in
 * an EquinoxOS userspace app.
 *
 * The vendored quickjs.c is host-agnostic — it doesn't know how to
 * print, how to fetch URLs, how to read files, etc. Each app that
 * embeds the engine needs to install its own globals. These helpers
 * are the common pieces every JS-using app wants:
 *
 *   qjs_install_console(ctx)
 *       Installs a 'console' object on globalThis with .log / .warn /
 *       .error / .info / .debug methods. Each writes a space-separated
 *       stringification of its arguments to stdout (via printf), followed
 *       by a newline. Identical behaviour for all 5 methods — EquinoxOS
 *       has no log-level routing yet.
 *
 *   qjs_dump_exception(ctx)
 *       Pretty-prints the current pending exception (call after any
 *       JSValue API that returned a value where JS_IsException() is
 *       true). Frees the exception value. Prints "exception: <msg>"
 *       and, when available, the stack trace under "  stack: <...>".
 */

#ifndef _EQ_QJS_HELPERS_H
#define _EQ_QJS_HELPERS_H

#include "quickjs.h"

void qjs_install_console(JSContext *ctx);
void qjs_dump_exception(JSContext *ctx);

#endif
