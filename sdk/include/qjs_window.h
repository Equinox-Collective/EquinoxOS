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

#include <stdint.h>
#include "quickjs.h"

void qjs_install_window(JSContext *ctx, const char *url);
void qjs_install_storage(JSContext *ctx);
void qjs_install_timers(JSContext *ctx);
void qjs_install_events(JSContext *ctx);

void qjs_drain_timers(JSContext *ctx);

/* R5/N5: per-frame timer tick. Advances the virtual clock to
 * `now_ms` (ms since boot from the host) and fires every timer with
 * due <= now_ms. Intervals are rescheduled; one-shots are unlinked.
 * Unlike qjs_drain_timers this does NOT free the remaining timer
 * list — long-running setIntervals survive across frames. */
void qjs_window_tick_timers(JSContext *ctx, uint64_t now_ms);
void qjs_fire_DOMContentLoaded(JSContext *ctx);
void qjs_fire_load(JSContext *ctx);
void qjs_fire_loaded_events(JSContext *ctx);  /* DOMContentLoaded then load, no timers in between */

/* Releases every JSValue still held in module-static state (event
 * listener callbacks, leftover interval timers, storage entries).
 * MUST be called before JS_FreeContext/JS_FreeRuntime — QuickJS
 * asserts on a non-empty GC list at runtime teardown. */
void qjs_window_teardown(JSContext *ctx);

/* R5/N1: pending navigation requested via JS. Kinds:
 *   1 = ASSIGN   navigate to `url_out` and push history
 *   2 = REPLACE  navigate to `url_out` without pushing history
 *   3 = RELOAD   reload the current page (url_out is empty)
 *   4 = HISTORY  history.go(delta_out)
 * Returns 1 on a hit (and clears state), 0 if nothing pending. */
int qjs_window_take_nav(int *kind, char *url_out, size_t url_len, int *delta_out);

/* The host pokes this after every navigation so `history.length`
 * matches the back-stack depth. */
void qjs_window_set_history_length(int n);

#endif
