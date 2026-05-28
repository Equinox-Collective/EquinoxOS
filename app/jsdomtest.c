/* app/jsdomtest.c — phase J4 smoke test.
 *
 * Loads res/index.html, parses to DOM, installs `document` on a
 * QuickJS context, runs a small script that exercises the bindings.
 *
 * Expected output (against the current res/index.html):
 *
 *   jsdomtest: starting
 *   document.title = "EquinoxOS Local Web"
 *   document.body.children.length = 7
 *   document.body.children[0].tagName = "H1"
 *   document.body.children[0].textContent = "EquinoxOS Local Web"
 *   h2s.length = 2
 *   h2s[0].textContent = "Supported in this viewer"
 *   h2s[1].textContent = "Next target"
 *   document.getElementsByTagName('li').length = 5
 *   first li text starts with "Headings"
 *   querySelector('h1').tagName = "H1"
 *   jsdomtest: done
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "equos.h"
#include "quickjs.h"
#include "qjs_helpers.h"
#include "dom.h"
#include "dom_js.h"

static char *read_whole_file(const char *path, uint32_t *out_size) {
  uint32_t size = 0;
  void *buf = (void *)_syscall(SYS_READ_FILE,
                               (uint64_t)path, (uint64_t)&size,
                               0, 0, 0);
  if (!buf) return NULL;
  *out_size = size;
  return (char *)buf;
}

static void eval_dump(JSContext *ctx, const char *src, const char *tag) {
  JSValue r = JS_Eval(ctx, src, strlen(src), tag, JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) {
    qjs_dump_exception(ctx);
  }
  JS_FreeValue(ctx, r);
}

int main(int argc, char **argv) {
  const char *path = (argc >= 2) ? argv[1] : "res/index.html";
  printf("jsdomtest: starting\n");

  uint32_t size = 0;
  char *html = read_whole_file(path, &size);
  if (!html) {
    printf("jsdomtest: ERR: can't read %s\n", path);
    return 1;
  }
  printf("jsdomtest: read %u bytes from %s\n", (unsigned)size, path);

  dom_node_t *doc = dom_parse(html, size);
  if (!doc) {
    printf("jsdomtest: ERR: dom_parse failed\n");
    return 1;
  }

  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);

  qjs_install_console(ctx);
  qjs_install_dom(ctx, doc);

  const char *script =
    "console.log('document.title = ' + JSON.stringify(document.title));\n"
    "console.log('document.body.children.length = ' + document.body.children.length);\n"
    "const c0 = document.body.children[0];\n"
    "console.log('document.body.children[0].tagName = ' + JSON.stringify(c0.tagName));\n"
    "console.log('document.body.children[0].textContent = ' + JSON.stringify(c0.textContent));\n"
    "const h2s = document.getElementsByTagName('h2');\n"
    "console.log('h2s.length = ' + h2s.length);\n"
    "for (let i = 0; i < h2s.length; i++) {\n"
    "  console.log('  h2s[' + i + '].textContent = ' + JSON.stringify(h2s[i].textContent));\n"
    "}\n"
    "const lis = document.getElementsByTagName('li');\n"
    "console.log('lis.length = ' + lis.length);\n"
    "if (lis.length > 0) {\n"
    "  console.log('  lis[0].textContent = ' + JSON.stringify(lis[0].textContent));\n"
    "}\n"
    "const qh1 = document.querySelector('h1');\n"
    "console.log('querySelector(\"h1\").tagName = ' + JSON.stringify(qh1 ? qh1.tagName : null));\n"
    "const qcode = document.querySelector('code');\n"
    "console.log('querySelector(\"code\").textContent = ' + JSON.stringify(qcode ? qcode.textContent : null));\n";

  eval_dump(ctx, script, "<jsdomtest>");

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  dom_free(doc);
  printf("jsdomtest: done\n");
  return 0;
}
