/* sdk/include/dom_js.h — phase J4.
 *
 * Installs a `document` global on a QuickJS context that wraps a
 * sdk/lib_dom tree. Each DOM element becomes a JS object of an
 * "Element" class. The C-side tree owns the storage; JS wrappers
 * hold a borrowed pointer and never free.
 *
 * Read-only on J4 — write APIs (setAttribute, appendChild,
 * textContent setter, …) come in J6 along with re-render.
 *
 * Usage:
 *
 *   #include "quickjs.h"
 *   #include "dom.h"
 *   #include "dom_js.h"
 *
 *   dom_node_t *doc = dom_parse(html, size);
 *   qjs_install_dom(ctx, doc);     // installs `document` global
 *   // run scripts that read from `document`
 *   dom_free(doc);                 // safe — JS holds no ownership
 */

#ifndef DOM_JS_H
#define DOM_JS_H

#include "quickjs.h"
#include "dom.h"

/* Installs the `document` global object and its Element class on the
 * given QuickJS context. The document is backed by `doc`, which must
 * be a dom_node_t* tree returned by dom_parse(). The tree must
 * outlive every JS evaluation against `ctx`. */
void qjs_install_dom(JSContext *ctx, dom_node_t *doc);

/* Dispatch a synthetic event of type `name` on `target` to every
 * Element-level listener registered via element.addEventListener().
 * Returns the number of handlers invoked. */
int  qjs_dom_dispatch_event(JSContext *ctx, dom_node_t *target,
                            const char *name);

/* Free any JSValues held by the per-element listener pool. MUST be
 * called before JS_FreeContext on the same ctx. */
void qjs_dom_teardown(JSContext *ctx);

/* R5/N5: dispatch a key event ('keydown' / 'keyup') with extra
 * `key` (string) and `keyCode` (number) fields populated on the
 * synthetic event. Returns 1 if any handler called
 * event.preventDefault(). */
int  qjs_dom_dispatch_key_event(JSContext *ctx, dom_node_t *target,
                                const char *name,
                                const char *keystr, int keycode);

/* DOM has been mutated by JS since the last consume? Returns 1 and
 * clears the flag if so. Set by every mutation binding (setAttribute,
 * textContent, appendChild, innerHTML, …). */
int  qjs_dom_consume_dirty(void);

#endif /* DOM_JS_H */
