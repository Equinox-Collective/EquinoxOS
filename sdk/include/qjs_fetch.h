/* sdk/include/qjs_fetch.h — phase J5.
 *
 * Installs a `fetch` global on a QuickJS context. The function shape
 * matches the web standard surface that simple scripts use; the actual
 * I/O is synchronous under the hood (it calls SYS_READ_FILE for local
 * paths and eq_http_get from <http_client.h> for http(s)://), but the
 * result is wrapped in a Promise so callers see the right control flow.
 *
 *   fetch(url)               → Promise<Response>
 *   Response.ok              → bool   (status in 200..299)
 *   Response.status          → number
 *   Response.url             → string (final URL after redirects)
 *   Response.text()          → Promise<string>
 *
 * The Promises are already settled by the time fetch returns — but the
 * .then() callbacks still need a microtask drain to fire, so call
 * qjs_run_microtasks() after every top-level eval.
 *
 * Trust anchors for HTTPS: the caller passes a pointer to the
 * br_x509_trust_anchor array and its count (typically TAs_MOZ /
 * TAs_MOZ_NUM from third_party/ca_bundle/ca_bundle.h). May be NULL for
 * file:// / http:// only use.
 */

#ifndef _EQ_QJS_FETCH_H
#define _EQ_QJS_FETCH_H

#include <stddef.h>
#include "quickjs.h"

struct br_x509_trust_anchor;

void qjs_install_fetch(JSContext *ctx,
                       const struct br_x509_trust_anchor *tas,
                       size_t tas_num);

#endif
