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

#endif /* DOM_JS_H */
