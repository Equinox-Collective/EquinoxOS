/* sdk/include/qjs_page.h — phase J6a.
 *
 * One-shot "run every <script> on this page" pass for the browser/
 * htmlview pipeline. Sits between dom_parse and the renderer:
 *
 *     dom_node_t *doc = dom_parse(html, size);
 *     qjs_run_page_scripts(doc, TAs_MOZ, TAs_MOZ_NUM);   // <- here
 *     render(doc);
 *     dom_free(doc);
 *
 * Spins up a fresh QuickJS runtime per page (cheap; ~ms), installs
 * console + document (bound to `doc`) + fetch, walks the tree for
 * <script> elements, evaluates inline contents in document order,
 * handles <script src="..."> via the same fetch binding (synchronous
 * under the hood), drains microtasks, and tears the runtime down.
 *
 * Mutations to the DOM from inside scripts are not yet wired (J6b);
 * for now the script side is read-only — useful for telemetry,
 * console.log, and reading document state.
 *
 * Trust anchors: same convention as qjs_install_fetch — pass
 * TAs_MOZ / TAs_MOZ_NUM for HTTPS, or NULL/0 for file/http only.
 */

#ifndef _EQ_QJS_PAGE_H
#define _EQ_QJS_PAGE_H

#include <stddef.h>
#include "dom.h"

struct br_x509_trust_anchor;

/* `page_url` is plumbed through to window.location; pass NULL for
 * about:blank. */
void qjs_run_page_scripts(dom_node_t *doc,
                          const char *page_url,
                          const struct br_x509_trust_anchor *tas,
                          size_t tas_num);

/* -----------------------------------------------------------------
 * Persistent page session (phase R4 / F0).
 *
 * `qjs_run_page_scripts` is one-shot — it tears the runtime down
 * before returning, so JS handlers registered via addEventListener
 * cannot fire after page load. For interactive content (buttons,
 * inputs, form submits) the renderer needs to keep the JS context
 * alive across frames and call back into it whenever a widget event
 * fires.
 *
 * Lifecycle:
 *   qjs_page_t *p = qjs_page_create(doc, url, tas, num);  // runs initial scripts
 *   ...per frame...
 *     if (widget_clicked)
 *       qjs_page_dispatch_event(p, node, "click");        // fires JS handlers
 *     if (qjs_page_consume_dirty(p))
 *       rebuild_lines();                                  // re-render after JS mutations
 *   qjs_page_free(p);                                     // before next nav
 * ---------------------------------------------------------------- */

typedef struct qjs_page qjs_page_t;

qjs_page_t *qjs_page_create(dom_node_t *doc,
                            const char *page_url,
                            const struct br_x509_trust_anchor *tas,
                            size_t tas_num);

/* Dispatch a synthetic event of type `name` on `target`. Walks the
 * per-page Element listener pool and calls each matching handler.
 * Returns 1 if any default action was prevented (event.preventDefault
 * called), else 0. */
int qjs_page_dispatch_event(qjs_page_t *p,
                            dom_node_t *target,
                            const char *name);

/* Has any JS callback mutated the DOM since the last consume?
 * Returns 1 and clears the flag, or 0. */
int qjs_page_consume_dirty(qjs_page_t *p);

/* R5/N0: did JS write to `location.href` (or any of the aliased
 * setters) since the last check? On a hit, copy the requested URL
 * into `out` (truncated to `out_len-1`) and update the page's
 * remembered URL so the next call doesn't re-fire. Returns 1 if a
 * navigation was requested, else 0. */
int qjs_page_pending_nav(qjs_page_t *p, char *out, size_t out_len);

/* R5/N1: richer navigation poll. Reports both explicit verbs
 * (location.assign/replace/reload, history.back/forward/go) and the
 * implicit href-overwrite case. `kind_out`:
 *   1 ASSIGN   url_out is the target, push history.
 *   2 REPLACE  url_out is the target, don't push history.
 *   3 RELOAD   reload the current URL (url_out is empty).
 *   4 HISTORY  history.go(delta_out).
 * Returns 1 if a navigation was requested (and clears it), else 0. */
int qjs_page_take_nav(qjs_page_t *p, int *kind_out,
                      char *url_out, size_t url_len, int *delta_out);

void qjs_page_free(qjs_page_t *p);

#endif
