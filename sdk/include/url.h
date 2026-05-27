#ifndef _EQUOS_URL_H
#define _EQUOS_URL_H
/* ---------------------------------------------------------------------------
 * EquinoxOS URL parser (phase 5).
 *
 * Small header-only RFC-3986-flavoured parser for the subset of URLs the
 * browser stack actually needs:
 *
 *     scheme "://" host [ ":" port ] [ path [ "?" query ] ] [ "#" frag ]
 *
 * Only http:// and https:// are accepted. Anything else returns
 * EQ_URL_ERR_BAD_SCHEME. Fragments (#…) are silently stripped because they
 * are client-side and never travel on the wire. Userinfo (user[:pwd]@…) is
 * rejected on purpose — we do not want to grow auth handling here.
 *
 * Two entry points:
 *
 *   eq_url_parse(url_str, &url)
 *       Parse an absolute URL into its parts.
 *
 *   eq_url_resolve(&base_url, ref_str, out_buf, out_sz)
 *       Resolve a possibly-relative reference against a base URL into a new
 *       absolute URL string. Handles three relative forms:
 *           "http(s)://..."   absolute   (verbatim copy)
 *           "//host/..."      protocol-relative (steal scheme from base)
 *           "/path"           root-relative
 *           "rel/path"        path-relative to base's directory
 *
 *       This is exactly what's needed to follow Location: redirects.
 *
 * Why header-only? It's pure parsing — no syscalls, no I/O — so inlining
 * costs almost nothing and avoids adding another .c file to the SDK link.
 * Same convention as <bearssl_io.h>.
 * ------------------------------------------------------------------------ */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

#define EQ_URL_MAX_HOST 256
#define EQ_URL_MAX_PATH 2048

typedef struct {
  char scheme[8];                /* "http" or "https" (lowercase, NUL-term) */
  char host[EQ_URL_MAX_HOST];    /* hostname / dotted-quad, NUL-term        */
  uint16_t port;                 /* 80 / 443 if omitted                     */
  char path[EQ_URL_MAX_PATH];    /* always starts with '/', no fragment     */
  int is_https;                  /* convenience: scheme == "https"          */
} eq_url_t;

typedef enum {
  EQ_URL_OK = 0,
  EQ_URL_ERR_BAD_SCHEME = -1,
  EQ_URL_ERR_NO_HOST = -2,
  EQ_URL_ERR_HOST_TOO_LONG = -3,
  EQ_URL_ERR_PATH_TOO_LONG = -4,
  EQ_URL_ERR_BAD_PORT = -5,
  EQ_URL_ERR_HAS_USERINFO = -6,
} eq_url_err_t;

static inline int eq_url_parse(const char *url, eq_url_t *out) {
  if (!url || !out) return EQ_URL_ERR_BAD_SCHEME;
  memset(out, 0, sizeof *out);

  const char *p = url;

  /* --- scheme ---------------------------------------------------------- */
  if (strncasecmp(p, "https://", 8) == 0) {
    strcpy(out->scheme, "https");
    out->is_https = 1;
    out->port = 443;
    p += 8;
  } else if (strncasecmp(p, "http://", 7) == 0) {
    strcpy(out->scheme, "http");
    out->is_https = 0;
    out->port = 80;
    p += 7;
  } else {
    return EQ_URL_ERR_BAD_SCHEME;
  }

  /* --- authority ------------------------------------------------------- */
  /* Reject userinfo: '@' inside the authority part is not allowed here.
   * Find end of authority first (until '/', '?', '#', or end). */
  const char *auth_end = p;
  while (*auth_end && *auth_end != '/' && *auth_end != '?' &&
         *auth_end != '#')
    auth_end++;
  for (const char *q = p; q < auth_end; q++) {
    if (*q == '@') return EQ_URL_ERR_HAS_USERINFO;
  }

  /* host (until ':' or end of authority) */
  const char *host_end = p;
  while (host_end < auth_end && *host_end != ':') host_end++;
  size_t host_len = (size_t)(host_end - p);
  if (host_len == 0) return EQ_URL_ERR_NO_HOST;
  if (host_len >= EQ_URL_MAX_HOST) return EQ_URL_ERR_HOST_TOO_LONG;
  memcpy(out->host, p, host_len);
  out->host[host_len] = '\0';
  p = host_end;

  /* optional port */
  if (p < auth_end && *p == ':') {
    p++;
    const char *port_start = p;
    while (p < auth_end && *p >= '0' && *p <= '9') p++;
    if (p == port_start) return EQ_URL_ERR_BAD_PORT;
    /* port_start..p are all decimal digits per the loop above. atoi
     * (not strtol — the SDK has no strtol) is fine since we already
     * bounded the range to known digits. Reject 0 and >65535. */
    int port = atoi(port_start);
    if (port <= 0 || port > 65535) return EQ_URL_ERR_BAD_PORT;
    out->port = (uint16_t)port;
  }

  /* --- path + query (strip fragment) ----------------------------------- */
  const char *path_start = auth_end;
  const char *path_end = path_start;
  while (*path_end && *path_end != '#') path_end++;
  size_t path_len = (size_t)(path_end - path_start);

  if (path_len == 0) {
    /* No path at all: use "/" */
    out->path[0] = '/';
    out->path[1] = '\0';
  } else if (*path_start == '?') {
    /* Query without explicit path: prepend "/" */
    if (path_len + 2 > EQ_URL_MAX_PATH) return EQ_URL_ERR_PATH_TOO_LONG;
    out->path[0] = '/';
    memcpy(out->path + 1, path_start, path_len);
    out->path[path_len + 1] = '\0';
  } else {
    if (path_len + 1 > EQ_URL_MAX_PATH) return EQ_URL_ERR_PATH_TOO_LONG;
    memcpy(out->path, path_start, path_len);
    out->path[path_len] = '\0';
  }

  return EQ_URL_OK;
}

/* Resolve a possibly-relative reference against a base URL. Writes a fresh
 * absolute URL string into `out`. Returns 0 on success, -1 on overflow /
 * malformed reference.
 *
 * Path-relative refs are resolved against the base's *directory* (the part
 * up to and including the last '/'). Query strings on the base are dropped.
 * This is the minimum that makes Location: chasing useful, not a full
 * RFC-3986 dot-segment resolver. */
static inline int eq_url_resolve(const eq_url_t *base, const char *ref,
                                 char *out, size_t outsz) {
  if (!base || !ref || !out || outsz == 0) return -1;

  /* Absolute? */
  if (strncasecmp(ref, "http://", 7) == 0 ||
      strncasecmp(ref, "https://", 8) == 0) {
    size_t rlen = strlen(ref);
    if (rlen + 1 > outsz) return -1;
    memcpy(out, ref, rlen + 1);
    return 0;
  }

  /* Protocol-relative: "//host/path" — steal scheme from base. */
  if (ref[0] == '/' && ref[1] == '/') {
    int n = snprintf(out, outsz, "%s:%s", base->scheme, ref);
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
  }

  /* Root-relative: "/path" */
  if (ref[0] == '/') {
    int n;
    if ((base->is_https && base->port == 443) ||
        (!base->is_https && base->port == 80)) {
      n = snprintf(out, outsz, "%s://%s%s",
                   base->scheme, base->host, ref);
    } else {
      n = snprintf(out, outsz, "%s://%s:%u%s",
                   base->scheme, base->host, (unsigned)base->port, ref);
    }
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
  }

  /* Path-relative: keep base path up to and including the last '/'. */
  const char *q = strchr(base->path, '?');
  size_t base_path_len = q ? (size_t)(q - base->path) : strlen(base->path);
  const char *last_slash = NULL;
  for (size_t i = 0; i < base_path_len; i++) {
    if (base->path[i] == '/') last_slash = base->path + i;
  }
  size_t dir_len = last_slash ? (size_t)(last_slash - base->path) + 1 : 1;

  int n;
  if ((base->is_https && base->port == 443) ||
      (!base->is_https && base->port == 80)) {
    n = snprintf(out, outsz, "%s://%s%.*s%s",
                 base->scheme, base->host,
                 (int)dir_len, base->path, ref);
  } else {
    n = snprintf(out, outsz, "%s://%s:%u%.*s%s",
                 base->scheme, base->host, (unsigned)base->port,
                 (int)dir_len, base->path, ref);
  }
  return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}

#endif /* _EQUOS_URL_H */
