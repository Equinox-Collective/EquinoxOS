/* ---------------------------------------------------------------------------
 * sdk/lib_http/http_client.c — implementation of <http_client.h>
 *
 * Phase 5 of the browser stack. See sdk/include/http_client.h for the API
 * contract; this file is "just" the engine that walks one URL fetch and,
 * optionally, chases Location: redirects.
 *
 * Layered on top of (in order):
 *
 *   * <sys/socket.h>       — phase 1 sockets (sys_socket / connect / send / recv)
 *   * <sys/wall_time.h>    — phase 4b CMOS RTC -> Unix seconds for X.509
 *   * <bearssl.h> + <bearssl_io.h>
 *                          — phase 3 TLS (full client + minimal X.509)
 *   * <url.h>              — phase 5 URL parser / resolver
 *
 * Lives in its own directory (`sdk/lib_http/`) and is built as a single
 * extra object, NOT folded into $(SDK_OBJS), because it (a) pulls BearSSL
 * headers and (b) wouldn't make sense to drag into apps that have no
 * network needs (e.g. Doom). Apps that want HTTP add `$(HTTP_CLIENT_OBJ)`
 * to their link line — see the urlget.elf / browser.elf rules in Makefile.
 *
 * Non-reentrant by design: a single static TLS client context lives at
 * file scope and is reset on every eq_http_get() call. That keeps the
 * BSS footprint at ~30 KiB shared between every fetch instead of paying
 * it twice on redirects.
 *
 * Body storage uses malloc()/realloc() so a 200 OK doesn't pay for the
 * 1 MiB cap up front. The cap exists to keep a malformed Content-Length
 * or a misbehaving server from eating the whole heap.
 * ------------------------------------------------------------------------ */

#include <equos.h>
#include <sys/socket.h>
#include <sys/wall_time.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>

#include <bearssl.h>
#include <bearssl_io.h>

#include <url.h>
#include <http_client.h>

/* --- TLS state (large; lives in .bss, shared across every fetch) -------- */
static br_ssl_client_context   g_sc;
static br_x509_minimal_context g_xc;
static unsigned char           g_iobuf[BR_SSL_BUFSIZE_BIDI];

/* Per-request scratch — 1 KiB is enough for our standard 5-header GET. */
static char g_req[1024];

#define HTTP_INITIAL_RESP_CAP 8192u
#define HTTP_RECV_CHUNK       4096u

/* Forward decls. */
static int  do_one_fetch(const eq_url_t *url, const char *full_url,
                         const eq_http_options_t *opts,
                         eq_http_response_t *out);
static int  parse_response(eq_http_response_t *out,
                           const unsigned char *buf, size_t len);
static int  append_body(eq_http_response_t *out,
                        const unsigned char *data, size_t len,
                        uint32_t body_limit);

/* ========================================================================
 * Public API
 * ====================================================================== */

void eq_http_response_free(eq_http_response_t *r) {
  if (!r) return;
  free(r->status_line);  r->status_line = NULL;
  free(r->headers);      r->headers     = NULL;
  free(r->body);         r->body        = NULL;
  free(r->final_url);    r->final_url   = NULL;
  r->headers_len = 0;
  r->body_len    = 0;
  r->status_code = 0;
  r->redirects_followed = 0;
  r->err = 0;
  r->tls_last_err = 0;
}

int eq_http_get(const char *url_str, const eq_http_options_t *opts,
                eq_http_response_t *out) {
  if (!url_str || !out) return EQ_HTTP_ERR_BAD_URL;
  memset(out, 0, sizeof *out);

  static const eq_http_options_t default_opts = {
      .follow_redirects = EQ_HTTP_MAX_REDIRECTS_DEFAULT,
      .recv_timeout_ms  = EQ_HTTP_DEFAULT_RECV_TIMEOUT_MS,
      .body_limit_bytes = EQ_HTTP_DEFAULT_BODY_LIMIT_BYTES,
  };
  if (!opts) opts = &default_opts;

  int max_redir = opts->follow_redirects;
  if (max_redir == 0) max_redir = EQ_HTTP_MAX_REDIRECTS_DEFAULT;
  if (max_redir < 0)  max_redir = 0;

  /* Working URL buffer — rewritten on each redirect. 4 KiB is generous
   * given EQ_URL_MAX_HOST (256) + EQ_URL_MAX_PATH (2048). */
  char cur_url[4096];
  if (strlen(url_str) + 1 > sizeof cur_url) return EQ_HTTP_ERR_BAD_URL;
  strcpy(cur_url, url_str);

  /* ip_override only applies to the very first request. After a
   * redirect the destination may differ so DNS is the only sane source. */
  int first_pass = 1;
  int redirects  = 0;

  for (;;) {
    eq_url_t url;
    int rc = eq_url_parse(cur_url, &url);
    if (rc != EQ_URL_OK) {
      out->err = EQ_HTTP_ERR_BAD_URL;
      return EQ_HTTP_ERR_BAD_URL;
    }

    /* Free anything left over from the previous hop (status, headers,
     * body) — final_url we'll rewrite below. */
    free(out->status_line); out->status_line = NULL;
    free(out->headers);     out->headers     = NULL;
    free(out->body);        out->body        = NULL;
    out->headers_len = 0;
    out->body_len    = 0;
    out->status_code = 0;
    out->tls_last_err = 0;

    /* Mask the ip_override after the first hop. */
    eq_http_options_t scratch = *opts;
    if (!first_pass) scratch.ip_override_be = 0;

    rc = do_one_fetch(&url, cur_url, &scratch, out);
    first_pass = 0;

    /* Remember the URL we actually requested (the last one before
     * any further redirect). */
    free(out->final_url);
    out->final_url = strdup(cur_url);

    if (rc != EQ_HTTP_OK) {
      out->err = rc;
      return rc;
    }

    /* Should we follow a redirect? */
    int sc = out->status_code;
    int is_redir = (sc == 301 || sc == 302 || sc == 303 ||
                    sc == 307 || sc == 308);
    if (!is_redir || redirects >= max_redir) {
      return EQ_HTTP_OK;
    }

    const char *loc = eq_http_response_header(out, "Location");
    if (!loc || !*loc) {
      /* 3xx without Location: — give the caller what we have. */
      return EQ_HTTP_OK;
    }

    char next_url[4096];
    if (eq_url_resolve(&url, loc, next_url, sizeof next_url) != 0) {
      out->err = EQ_HTTP_ERR_REDIRECT;
      return EQ_HTTP_ERR_REDIRECT;
    }

    if (opts->verbose) {
      printf("[http] %d -> %s\n", sc, next_url);
    }

    strcpy(cur_url, next_url);
    redirects++;
    out->redirects_followed = redirects;
  }
}

const char *eq_http_response_header(const eq_http_response_t *r,
                                    const char *name) {
  if (!r || !r->headers || !name) return NULL;
  static char value_buf[1024];
  size_t nlen = strlen(name);
  const char *p = r->headers;

  while (*p) {
    const char *eol = strstr(p, "\r\n");
    size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

    if (line_len > nlen + 1 &&
        strncasecmp(p, name, nlen) == 0 &&
        p[nlen] == ':') {
      const char *v = p + nlen + 1;
      while (*v == ' ' || *v == '\t') v++;
      size_t vlen = (size_t)((eol ? eol : p + line_len) - v);
      if (vlen >= sizeof value_buf) vlen = sizeof value_buf - 1;
      memcpy(value_buf, v, vlen);
      value_buf[vlen] = '\0';
      return value_buf;
    }

    if (!eol) break;
    p = eol + 2;
  }
  return NULL;
}

/* ========================================================================
 * Internal: single GET, no redirect chasing.
 * ====================================================================== */

static int do_one_fetch(const eq_url_t *url, const char *full_url,
                        const eq_http_options_t *opts,
                        eq_http_response_t *out) {
  (void)full_url;
  uint32_t recv_to = opts->recv_timeout_ms ? opts->recv_timeout_ms
                                            : EQ_HTTP_DEFAULT_RECV_TIMEOUT_MS;
  uint32_t body_limit = opts->body_limit_bytes
                            ? opts->body_limit_bytes
                            : EQ_HTTP_DEFAULT_BODY_LIMIT_BYTES;

  /* --- DNS / IP literal --------------------------------------------- */
  uint32_t ip_be;
  if (opts->ip_override_be) {
    ip_be = opts->ip_override_be;
    if (opts->verbose) {
      printf("[http] IP override %u.%u.%u.%u\n",
             (unsigned)((ip_be >> 24) & 0xFF),
             (unsigned)((ip_be >> 16) & 0xFF),
             (unsigned)((ip_be >>  8) & 0xFF),
             (unsigned)( ip_be        & 0xFF));
    }
  } else {
    ip_be = net_dns_resolve(url->host);
    if (ip_be == 0) {
      if (opts->verbose) printf("[http] DNS FAIL: %s\n", url->host);
      return EQ_HTTP_ERR_DNS;
    }
    if (opts->verbose) {
      printf("[http] DNS %s -> %u.%u.%u.%u\n", url->host,
             (unsigned)((ip_be >> 24) & 0xFF),
             (unsigned)((ip_be >> 16) & 0xFF),
             (unsigned)((ip_be >>  8) & 0xFF),
             (unsigned)( ip_be        & 0xFF));
    }
  }

  /* --- TLS prerequisites ------------------------------------------- */
  if (url->is_https) {
    if (!opts->trust_anchors || opts->trust_anchors_num == 0) {
      return EQ_HTTP_ERR_NO_ANCHORS;
    }
  }

  /* --- TCP --------------------------------------------------------- */
  int s = sys_socket();
  if (s < 0) return EQ_HTTP_ERR_CONNECT;

  sys_setsockopt(s, SOCK_LEVEL_SOCKET, SOCK_OPT_RCVTIMEO,
                 &recv_to, sizeof recv_to);

  if (sys_connect(s, ip_be, url->port) < 0) {
    sys_close_sock(s);
    return EQ_HTTP_ERR_CONNECT;
  }
  if (opts->verbose) {
    printf("[http] connect %s:%u ok\n", url->host, (unsigned)url->port);
  }

  /* --- TLS setup (only for https://) ------------------------------- */
  br_sslio_context ioc;
  eq_bearssl_sock_io_t io = { .fd = s };

  if (url->is_https) {
    br_ssl_client_init_full(&g_sc, &g_xc,
                            opts->trust_anchors, opts->trust_anchors_num);
    br_ssl_engine_set_buffer(&g_sc.eng, g_iobuf, sizeof g_iobuf, 1);

    uint64_t now_unix = 0;
    if (sys_get_wall_time(&now_unix) != 0) {
      sys_close_sock(s);
      return EQ_HTTP_ERR_TLS;
    }
    uint32_t br_days, br_secs;
    unix_to_bearssl_time(now_unix, &br_days, &br_secs);
    br_x509_minimal_set_time(&g_xc, br_days, br_secs);

    eq_bearssl_seed_engine(&g_sc.eng, 32);
    br_ssl_client_reset(&g_sc, url->host, 0);
    br_sslio_init(&ioc, &g_sc.eng,
                  eq_bearssl_sock_read,  &io,
                  eq_bearssl_sock_write, &io);
    if (opts->verbose) {
      printf("[http] TLS init done (SNI=%s, TAs=%u)\n",
             url->host, (unsigned)opts->trust_anchors_num);
    }
  }

  /* --- Build HTTP/1.0 request -------------------------------------- */
  int req_len = snprintf(g_req, sizeof g_req,
                         "GET %s HTTP/1.0\r\n"
                         "Host: %s\r\n"
                         "User-Agent: EquinoxOS/http_client (phase5)\r\n"
                         "Accept: */*\r\n"
                         "Connection: close\r\n"
                         "\r\n",
                         url->path, url->host);
  if (req_len <= 0 || (size_t)req_len >= sizeof g_req) {
    sys_close_sock(s);
    return EQ_HTTP_ERR_SEND;
  }

  /* --- Send -------------------------------------------------------- */
  if (url->is_https) {
    if (br_sslio_write_all(&ioc, g_req, (size_t)req_len) < 0 ||
        br_sslio_flush(&ioc) < 0) {
      out->tls_last_err = br_ssl_engine_last_error(&g_sc.eng);
      sys_close_sock(s);
      return EQ_HTTP_ERR_SEND;
    }
  } else {
    int sent = 0;
    while (sent < req_len) {
      int n = sys_send(s, (const uint8_t *)g_req + sent,
                       (uint32_t)(req_len - sent));
      if (n <= 0) {
        sys_close_sock(s);
        return EQ_HTTP_ERR_SEND;
      }
      sent += n;
    }
  }
  if (opts->verbose) {
    printf("[http] GET %s (%d bytes sent)\n", url->path, req_len);
  }

  /* --- Receive into growable buffer, then split ------------------- */
  size_t cap = HTTP_INITIAL_RESP_CAP;
  size_t len = 0;
  unsigned char *buf = (unsigned char *)malloc(cap);
  if (!buf) {
    sys_close_sock(s);
    return EQ_HTTP_ERR_OOM;
  }

  for (;;) {
    if (len + HTTP_RECV_CHUNK > cap) {
      size_t new_cap = cap * 2;
      if (new_cap > body_limit + 64 * 1024u) {
        new_cap = body_limit + 64 * 1024u;
      }
      if (new_cap <= cap) {
        free(buf);
        sys_close_sock(s);
        return EQ_HTTP_ERR_TOO_BIG;
      }
      unsigned char *nb = (unsigned char *)realloc(buf, new_cap);
      if (!nb) { free(buf); sys_close_sock(s); return EQ_HTTP_ERR_OOM; }
      buf = nb;
      cap = new_cap;
    }
    int n;
    if (url->is_https) {
      n = br_sslio_read(&ioc, buf + len, cap - len);
    } else {
      n = sys_recv(s, buf + len, (uint32_t)(cap - len));
    }
    if (n <= 0) break;
    len += (size_t)n;
  }

  if (url->is_https) {
    out->tls_last_err = br_ssl_engine_last_error(&g_sc.eng);
  }
  sys_close_sock(s);

  /* --- Parse status / headers / body ------------------------------ */
  int rc = parse_response(out, buf, len);
  if (rc == EQ_HTTP_OK && opts->on_body_chunk && out->body && out->body_len) {
    /* HTTP/1.0 connection-close: we have the whole body buffered. Fire
     * the callback once with the complete body so streaming consumers
     * still see something. Phase 5 doesn't do mid-fetch streaming. */
    opts->on_body_chunk((const unsigned char *)out->body,
                        out->body_len, opts->on_body_chunk_ctx);
  }
  free(buf);
  return rc;
}

/* --- Parse "HTTP/1.x NNN ...\r\n<headers>\r\n\r\n<body>" ---------------- */
static int parse_response(eq_http_response_t *out,
                          const unsigned char *buf, size_t len) {
  if (len == 0) return EQ_HTTP_ERR_RECV;

  /* Find header terminator. */
  size_t boundary = 0;
  int found = 0;
  for (size_t i = 0; i + 3 < len; i++) {
    if (buf[i] == '\r' && buf[i+1] == '\n' &&
        buf[i+2] == '\r' && buf[i+3] == '\n') {
      boundary = i;
      found = 1;
      break;
    }
  }
  if (!found) return EQ_HTTP_ERR_PARSE;

  size_t headers_block_len = boundary;          /* excludes the final CRLFCRLF */
  size_t body_off          = boundary + 4;
  size_t body_len          = len - body_off;

  /* Status line = up to the first \r\n inside the headers block. */
  size_t status_len = 0;
  for (size_t i = 0; i + 1 < headers_block_len; i++) {
    if (buf[i] == '\r' && buf[i+1] == '\n') { status_len = i; break; }
  }
  if (status_len == 0) return EQ_HTTP_ERR_PARSE;

  out->status_line = (char *)malloc(status_len + 1);
  if (!out->status_line) return EQ_HTTP_ERR_OOM;
  memcpy(out->status_line, buf, status_len);
  out->status_line[status_len] = '\0';

  /* "HTTP/1.x NNN [reason]" — pick out the 3-digit code. */
  const char *sp = strchr(out->status_line, ' ');
  if (!sp) return EQ_HTTP_ERR_PARSE;
  while (*sp == ' ') sp++;
  out->status_code = atoi(sp);

  /* Remaining headers (line by line) live after the status CRLF. */
  size_t hdrs_start = status_len + 2;
  size_t hdrs_size  = (headers_block_len > hdrs_start)
                         ? headers_block_len - hdrs_start : 0;
  out->headers = (char *)malloc(hdrs_size + 3);
  if (!out->headers) return EQ_HTTP_ERR_OOM;
  if (hdrs_size > 0) memcpy(out->headers, buf + hdrs_start, hdrs_size);
  /* Ensure trailing CRLF so eq_http_response_header()'s strstr is always
   * able to terminate the last line cleanly. */
  out->headers[hdrs_size]     = '\r';
  out->headers[hdrs_size + 1] = '\n';
  out->headers[hdrs_size + 2] = '\0';
  out->headers_len            = hdrs_size;

  /* Body. */
  out->body_len = body_len;
  out->body = (char *)malloc(body_len + 1);
  if (!out->body) return EQ_HTTP_ERR_OOM;
  if (body_len) memcpy(out->body, buf + body_off, body_len);
  out->body[body_len] = '\0';

  return EQ_HTTP_OK;
}

/* Currently unused — phase 5 buffers the whole response. Kept around for
 * when phase 7 introduces mid-stream chunking. */
static int append_body(eq_http_response_t *out,
                       const unsigned char *data, size_t len,
                       uint32_t body_limit) {
  (void)out; (void)data; (void)len; (void)body_limit;
  return EQ_HTTP_OK;
}
