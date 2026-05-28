/* sdk/include/qjs_window.h — phase J7.
 *
 * Bolts the remaining web platform niceties onto a JS context that
 * already has console + document + fetch installed:
 *
 *   qjs_install_window(ctx, url)
 *       window === globalThis. window.location / document.location is
 *       a small object with .href, .protocol, .host, .pathname,
 *       .toString(). `url` may be NULL ("about:blank").
 *
 *   qjs_install_storage(ctx)
 *       localStorage: getItem/setItem/removeItem/clear/length/key.
 *       In-memory only (no persistence across runs yet).
 *
 *   qjs_install_timers(ctx)
 *       setTimeout/clearTimeout/setInterval/clearInterval. Real
 *       delay is collapsed to relative ordering: callbacks fire in
 *       (due-time, then insertion-order) order during the drain.
 *
 *   qjs_install_events(ctx)
 *       window.addEventListener / removeEventListener — for J7 we
 *       only care about 'DOMContentLoaded' / 'load'. Listeners are
 *       fired by qjs_fire_loaded_events().
 *
 *   qjs_drain_timers(ctx)
 *       Runs all pending setTimeout/setInterval callbacks, draining
 *       microtasks between each. Bounded by an internal iteration
 *       cap so a runaway setInterval can't hang the load.
 *
 *   qjs_fire_loaded_events(ctx)
 *       Dispatches a DOMContentLoaded event, then a load event,
 *       calling every listener registered for them.
 *
 * These three drain helpers compose to match the standard load order:
 *   scripts → microtasks → DOMContentLoaded → microtasks → timers
 *   → load → microtasks.
 */

#ifndef _EQ_QJS_WINDOW_H
#define _EQ_QJS_WINDOW_H

#include "quickjs.h"

void qjs_install_window(JSContext *ctx, const char *url);
void qjs_install_storage(JSContext *ctx);
void qjs_install_timers(JSContext *ctx);
void qjs_install_events(JSContext *ctx);

void qjs_drain_timers(JSContext *ctx);
void qjs_fire_loaded_events(JSContext *ctx);

#endif
