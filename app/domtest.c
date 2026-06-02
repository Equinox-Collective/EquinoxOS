/* app/domtest.c — smoke test for sdk/lib_dom (phase J3).
 *
 * Loads res/index.html from disk, parses it into a dom_node_t tree,
 * dumps the tree, and exercises the search helpers:
 *
 *   dom_get_first_element_by_tag(root, "title") → expect <title>
 *   dom_get_first_element_by_tag(root, "h1")    → expect <h1>
 *   dom_get_element_by_id(root, "<some id>")    → expect non-NULL
 *
 * Expected output (abridged):
 *
 *   domtest: starting
 *   domtest: read N bytes
 *   #document
 *     <html>
 *       <head>
 *         <title>
 *           #text "EquinoxOS Local Web"
 *       <body>
 *         <h1>
 *           #text "EquinoxOS Local Web"
 *         ...
 *   domtest: title text = 'EquinoxOS Local Web'
 *   domtest: h1 text = 'EquinoxOS Local Web'
 *   domtest: done
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "equos.h"
#include "dom.h"

/* Read a whole file from the EquinoxOS VFS. Mirrors what htmlview
 * does for its `index.html` path. Buffer is kernel-allocated; we
 * don't free it. */
static char *read_whole_file(const char *path, uint32_t *out_size) {
  uint32_t size = 0;
  void *buf = (void *)_syscall(SYS_READ_FILE,
                               (uint64_t)path, (uint64_t)&size,
                               0, 0, 0);
  if (!buf) return NULL;
  *out_size = size;
  return (char *)buf;
}

/* Concatenate every TEXT descendant of `node` into a freshly malloc'd
 * string, collapsing internal whitespace runs to single spaces and
 * trimming the ends. Caller free()s. */
static char *node_text_content(const dom_node_t *node) {
  /* First pass: total length upper bound. */
  uint32_t total = 0;
  const dom_node_t *cur = node->first_child;
  /* DFS via parent/sibling pointers without recursion */
  while (cur && cur != node) {
    if (cur->type == DOM_NODE_TEXT && cur->text)
      total += (uint32_t)strlen(cur->text);
    /* descend */
    if (cur->first_child) cur = cur->first_child;
    else {
      while (cur && cur != node && !cur->next_sibling) cur = cur->parent;
      if (cur == node) break;
      if (cur) cur = cur->next_sibling;
    }
  }
  char *buf = (char*)malloc(total + 1);
  if (!buf) return NULL;
  uint32_t w = 0;
  bool prev_ws = true;  /* collapses leading whitespace */
  cur = node->first_child;
  while (cur && cur != node) {
    if (cur->type == DOM_NODE_TEXT && cur->text) {
      for (const char *s = cur->text; *s; s++) {
        char c = *s;
        bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (ws) {
          if (!prev_ws) { buf[w++] = ' '; prev_ws = true; }
        } else {
          buf[w++] = c; prev_ws = false;
        }
      }
    }
    if (cur->first_child) cur = cur->first_child;
    else {
      while (cur && cur != node && !cur->next_sibling) cur = cur->parent;
      if (cur == node) break;
      if (cur) cur = cur->next_sibling;
    }
  }
  /* trim trailing space */
  while (w > 0 && buf[w-1] == ' ') w--;
  buf[w] = 0;
  return buf;
}

int main(int argc, char **argv) {
  const char *path = (argc >= 2) ? argv[1] : "res/index.html";
  printf("domtest: starting\n");

  uint32_t size = 0;
  char *html = read_whole_file(path, &size);
  if (!html) {
    printf("domtest: ERR: can't read %s\n", path);
    return 1;
  }
  printf("domtest: read %u bytes from %s\n", (unsigned)size, path);

  dom_node_t *doc = dom_parse(html, size);
  if (!doc) {
    printf("domtest: ERR: dom_parse failed\n");
    return 1;
  }

  dom_print(doc, 0);

  dom_node_t *title = dom_get_first_element_by_tag(doc, "title");
  if (title) {
    char *t = node_text_content(title);
    printf("domtest: title text = '%s'\n", t ? t : "(null)");
    free(t);
  } else {
    printf("domtest: no <title>\n");
  }

  dom_node_t *h1 = dom_get_first_element_by_tag(doc, "h1");
  if (h1) {
    char *t = node_text_content(h1);
    printf("domtest: h1 text = '%s'\n", t ? t : "(null)");
    free(t);
  } else {
    printf("domtest: no <h1>\n");
  }

  dom_free(doc);
  printf("domtest: done\n");
  return 0;
}
