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

void qjs_run_page_scripts(dom_node_t *doc,
                          const struct br_x509_trust_anchor *tas,
                          size_t tas_num);

#endif
