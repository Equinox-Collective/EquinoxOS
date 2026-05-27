/* ---------------------------------------------------------------------------
 * app/urlget.c — phase 5 sanity app for the new HTTP/HTTPS client library.
 *
 * Feeds an arbitrary URL into eq_http_get() and dumps the parsed response
 * to the serial console. Roughly the EquinoxOS equivalent of `curl -i`.
 *
 * Compared to phase 4c's httpsget.elf this app:
 *
 *   * Takes a full URL — scheme/host/port/path/query handled by the
 *     parser in <url.h>. Both http:// and https:// work.
 *   * Follows 3xx redirects (up to 5 by default). Try it on a host that
 *     bounces http -> https — e.g. wikipedia.org — to see the chain.
 *   * Prints headers parsed by the library, not the raw response.
 *
 * Usage:
 *
 *     run bin/urlget.elf <http://example.com/|http://example.com/>                  # plain HTTP
 *     run bin/urlget.elf <https://example.com/|https://example.com/>                 # HTTPS
 *     run bin/urlget.elf <https://example.com/|https://example.com/> 8.47.69.0       # bypass SLIRP DNS
 *     run bin/urlget.elf <http://wikipedia.org/|http://wikipedia.org/>                # follows redirects to <https://en.wikipedia.org/wiki/Main_Page|https://en.wikipedia.org/wiki/Main_Page>
 *
 * The optional 2nd arg is a dotted-quad IP that bypasses DNS for the very
 * first request only (same workaround as in httpsget.elf, useful while
 * the SLIRP DNS proxy on Windows returns flaky pool members).
 * ------------------------------------------------------------------------ */

#include <equos.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <url.h>
#include <http_client.h>

#include "../third_party/ca_bundle/ca_bundle.h"

static void dump_lines(const char *prefix, const char *blob, size_t len) {
  if (!blob || len == 0) return;
  size_t line_start = 0;
  for (size_t i = 0; i < len; i++) {
    if (blob[i] == '\n') {
      printf("%s", prefix);
      for (size_t j = line_start; j < i; j++) {
        if (blob[j] != '\r') putchar(blob[j]);
      }
      putchar('\n');
      line_start = i + 1;
    }
  }
  if (line_start < len) {
    printf("%s", prefix);
    for (size_t j = line_start; j < len; j++) {
      if (blob[j] != '\r') putchar(blob[j]);
    }
    putchar('\n');
  }
}

int main(int argc, char **argv) {
  const char *url = (argc >= 2) ? argv[1] : "http://example.com/";
  const char *ip_override_str = (argc >= 3) ? argv[2] : NULL;

  eq_http_options_t opts;
  memset(&opts, 0, sizeof opts);
  opts.trust_anchors      = TAs_MOZ;
  opts.trust_anchors_num  = TAs_MOZ_NUM;
  opts.follow_redirects   = 5;
  opts.recv_timeout_ms    = 20000;
  opts.body_limit_bytes   = 1u * 1024u * 1024u;
  opts.verbose            = 1;

  if (ip_override_str) {
    uint32_t ip_be = net_dns_resolve(ip_override_str);
    if (ip_be == 0) {
      printf("[urlget] bad IP override: %s\n", ip_override_str);
      return 1;
    }
    opts.ip_override_be = ip_be;
  }

  printf("[urlget] GET %s\n", url);

  eq_http_response_t resp;
  memset(&resp, 0, sizeof resp);
  int rc = eq_http_get(url, &opts, &resp);

  printf("[urlget] rc=%d status=%d redirects=%d tls_last_err=%d\n",
         rc, resp.status_code, resp.redirects_followed, resp.tls_last_err);
  if (resp.final_url) printf("[urlget] final URL: %s\n", resp.final_url);
  if (resp.status_line) printf("[urlget] %s\n", resp.status_line);

  if (resp.headers_len) {
    printf("[urlget] -- headers (%u B) --\n", (unsigned)resp.headers_len);
    dump_lines("  | ", resp.headers, resp.headers_len);
  }

  printf("[urlget] -- body (%u B) --\n", (unsigned)resp.body_len);
  if (resp.body && resp.body_len) {
    /* Body may be binary; just stream the bytes. */
    for (size_t i = 0; i < resp.body_len; i++) putchar(resp.body[i]);
    if (resp.body[resp.body_len - 1] != '\n') putchar('\n');
  }

  printf("[urlget] DONE\n");
  eq_http_response_free(&resp);
  return rc == EQ_HTTP_OK ? 0 : 1;
}
