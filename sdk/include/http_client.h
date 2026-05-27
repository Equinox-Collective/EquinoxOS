#ifndef _EQUOS_HTTP_CLIENT_H
#define _EQUOS_HTTP_CLIENT_H
/* ---------------------------------------------------------------------------
 * EquinoxOS HTTP/HTTPS client library (phase 5).
 *
 * One entry point — eq_http_get() — that takes a URL string and returns a
 * fully-parsed response. The library handles:
 *
 *   * URL parsing (sdk/include/url.h)
 *   * DNS resolution (via SYS_NET_DNS_RESOLVE, with optional IPv4 override)
 *   * Plain TCP for http://  (sockets API from phase 1)
 *   * BearSSL handshake for https://, including:
 *       - X.509 chain verification against caller-supplied trust anchors
 *       - SNI / SAN hostname check (hostname check ON, no NULL)
 *       - RTC-based set_time so expiry checks actually work (phase 4b)
 *       - 32-byte engine seed via SYS_GETRANDOM (phase 2)
 *   * HTTP/1.0 GET with Host:, User-Agent:, Accept:, Connection: close
 *   * Header / body split and status-code parsing
 *   * Redirect following (3xx + Location:), up to a caller-set limit;
 *     relative Location: values are resolved against the requesting URL
 *
 * Phase 6's browser.elf will live on top of this — *all* network I/O
 * happens here, the renderer just consumes the resulting body.
 *
 * The library is NOT reentrant: a single static BearSSL client context
 * lives in this translation unit and is reused for every fetch. Each call
 * to eq_http_get() reinitialises it. That's intentional — TLS context is
 * ~30 KiB and we'd rather not pay for that twice per process.
 *
 * Memory: response strings (status_line, headers, body, final_url) are
 * malloc()'d. The caller MUST free them via eq_http_response_free().
 *
 * Typical use:
 *
 *     #include <http_client.h>
 *     #include "../third_party/ca_bundle/ca_bundle.h"
 *
 *     eq_http_options_t opts = {0};
 *     opts.trust_anchors     = TAs_MOZ;
 *     opts.trust_anchors_num = TAs_MOZ_NUM;
 *     opts.follow_redirects  = 5;
 *
 *     eq_http_response_t r = {0};
 *     int rc = eq_http_get("https://example.com/", &opts, &r);
 *     if (rc == EQ_HTTP_OK) {
 *         printf("status=%d body=%u bytes\n", r.status_code,
 *                (unsigned)r.body_len);
 *     }
 *     eq_http_response_free(&r);
 * ------------------------------------------------------------------------ */

#include <stdint.h>
#include <stddef.h>
#include <bearssl.h>

#define EQ_HTTP_MAX_REDIRECTS_DEFAULT      5
#define EQ_HTTP_DEFAULT_RECV_TIMEOUT_MS    20000u
#define EQ_HTTP_DEFAULT_BODY_LIMIT_BYTES   (1u * 1024u * 1024u)  /* 1 MiB */

typedef enum {
  EQ_HTTP_OK            =  0,
  EQ_HTTP_ERR_BAD_URL   = -1,
  EQ_HTTP_ERR_DNS       = -2,
  EQ_HTTP_ERR_CONNECT   = -3,
  EQ_HTTP_ERR_TLS       = -4,
  EQ_HTTP_ERR_SEND      = -5,
  EQ_HTTP_ERR_RECV      = -6,
  EQ_HTTP_ERR_PARSE     = -7,
  EQ_HTTP_ERR_REDIRECT  = -8,
  EQ_HTTP_ERR_OOM       = -9,
  EQ_HTTP_ERR_TOO_BIG   = -10,
  EQ_HTTP_ERR_NO_ANCHORS = -11,
} eq_http_err_t;

typedef struct {
  /* Hard cap on redirect chain length. 0 → use default (5).
   * Negative → never follow. */
  int      follow_redirects;

  /* Trust anchors for HTTPS. MUST be supplied (and non-empty) if any
   * URL in the chain — including post-redirect — is https://. Pass
   * TAs_MOZ / TAs_MOZ_NUM from third_party/ca_bundle/ca_bundle.h for
   * the full Mozilla CCADB set. */
  const br_x509_trust_anchor *trust_anchors;
  size_t                      trust_anchors_num;

  /* Per-fetch recv timeout. 0 → use default (20 s). */
  uint32_t recv_timeout_ms;

  /* Hard cap on body size in bytes. 0 → use default (1 MiB). Set very
   * high (e.g. 256 MiB) to effectively disable. */
  uint32_t body_limit_bytes;

  /* Optional dotted-quad override: if non-zero, bypass DNS for the
   * *first* request and dial this IPv4 (in network byte order). SNI
   * and Host: still use the URL's hostname. Useful to test against a
   * specific edge while our SLIRP DNS returns a flaky pool member.
   * Redirects re-DNS as usual. */
  uint32_t ip_override_be;

  /* If non-zero, dump per-step diagnostics on the serial console. */
  int verbose;

  /* Optional callback fired as body bytes arrive. If NULL, the body is
   * buffered into `out->body` only. The chunk pointer is valid only
   * for the duration of the call; copy if you need to keep it. */
  void (*on_body_chunk)(const unsigned char *data, size_t len, void *ctx);
  void  *on_body_chunk_ctx;
} eq_http_options_t;

typedef struct {
  int      status_code;          /* e.g. 200, 0 on error */
  char    *status_line;          /* "HTTP/1.x NNN reason"  (NUL-term) */
  char    *headers;              /* raw header block, no status line, NUL-term */
  size_t   headers_len;
  char    *body;                 /* response body, NUL-padded for printf-safety */
  size_t   body_len;
  char    *final_url;            /* URL we actually GETted (post-redirect) */
  int      redirects_followed;
  int      err;                  /* eq_http_err_t snapshot */
  int      tls_last_err;         /* BR_ERR_xxx, 0 on plain HTTP or TLS success */
} eq_http_response_t;

/* Perform a GET against the given URL. Returns EQ_HTTP_OK or a negative
 * eq_http_err_t. Even on error, response fields may be partially filled
 * (e.g. status_code on a 5xx) — always pair with eq_http_response_free(). */
int eq_http_get(const char *url, const eq_http_options_t *opts,
                eq_http_response_t *out);

/* Free all malloc()'d fields and zero the struct. Safe to call on a
 * zero-initialised struct or after a partial failure. */
void eq_http_response_free(eq_http_response_t *r);

/* Case-insensitive header lookup. Returns a pointer into a static
 * scratch buffer that is overwritten by each call (NOT thread-safe,
 * fine for single-threaded apps). Returns NULL if header is absent. */
const char *eq_http_response_header(const eq_http_response_t *r,
                                    const char *name);

#endif /* _EQUOS_HTTP_CLIENT_H */
