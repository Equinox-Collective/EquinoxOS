/* app/jspagetest.c — phase J6a smoke test.
 *
 * Same wiring browser.elf uses: read HTML → parse to DOM →
 * qjs_run_page_scripts → free. No renderer, no GUI; pure serial
 * output. Lets us validate the on-load script pipeline without
 * needing network or bearssl.
 *
 * Reads `res/jstest.html` by default; argv[1] overrides.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "equos.h"
#include "dom.h"
#include "qjs_page.h"

int main(int argc, char **argv) {
  const char *path = (argc >= 2) ? argv[1] : "res/jstest.html";
  printf("jspagetest: starting on %s\n", path);

  uint32_t size = 0;
  char *html = (char *)_syscall(SYS_READ_FILE, (uint64_t)path,
                                (uint64_t)&size, 0, 0, 0);
  if (!html) {
    printf("jspagetest: ERR: cannot read %s\n", path);
    return 1;
  }
  printf("jspagetest: read %u bytes\n", (unsigned)size);

  dom_node_t *doc = dom_parse(html, size);
  if (!doc) { printf("jspagetest: ERR: dom_parse failed\n"); return 1; }

  /* No TAs — fetch from JS can still hit local files; http(s) calls
   * will fail with EQ_HTTP_ERR_NO_ANCHORS, which is what we want here
   * to keep the test offline. */
  qjs_run_page_scripts(doc, NULL, 0);

  dom_free(doc);
  printf("jspagetest: done\n");
  return 0;
}
