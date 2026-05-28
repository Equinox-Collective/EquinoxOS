/* sdk/include/dom.h — minimal HTML DOM tree for EquinoxOS browser apps.
 *
 * Replaces the flat HTML → lines[] pipeline in app/htmlview.c with a
 * proper tree intermediate. Used by:
 *   - htmlview.c (renderer walks the tree)
 *   - any QuickJS app (DOM bindings expose this tree to JS)
 *
 * Parser is intentionally lenient — no full HTML5 spec compliance.
 * It handles the common cases real pages and our own res/index.html
 * use: element open/close, self-closing tags, void elements, comments,
 * attributes (bare, ='..', ="..", =unquoted), <script>/<style> raw
 * text (until </script>/</style>), DOCTYPE skip, CDATA-as-text.
 *
 * Memory model: every node and every string is malloc()'d. Free the
 * whole tree with dom_free() on the root.
 *
 * Tree shape: dom_parse() returns a DOCUMENT node whose children are
 * the top-level elements (typically <!doctype> is skipped and <html>
 * is the single child, but the parser tolerates "tag soup" and just
 * appends siblings at the top level).
 */

#ifndef _EQ_DOM_H
#define _EQ_DOM_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  DOM_NODE_DOCUMENT = 0,
  DOM_NODE_ELEMENT  = 1,
  DOM_NODE_TEXT     = 2,
  DOM_NODE_COMMENT  = 3,
} dom_node_type_t;

typedef struct dom_attr {
  char            *name;     /* lowercased */
  char            *value;    /* may be empty string but never NULL */
  struct dom_attr *next;
} dom_attr_t;

typedef struct dom_node {
  dom_node_type_t   type;
  char             *tag_name;       /* lowercased; NULL for non-ELEMENT */
  char             *text;           /* TEXT/COMMENT payload; NULL otherwise */
  dom_attr_t       *attrs;          /* singly-linked list, insertion order */

  struct dom_node  *parent;
  struct dom_node  *first_child;
  struct dom_node  *last_child;
  struct dom_node  *next_sibling;
  struct dom_node  *prev_sibling;
} dom_node_t;

/* Parse an HTML byte buffer into a DOM tree. Returns a DOCUMENT root.
 * The caller owns the returned tree and must free it with dom_free().
 * Returns NULL on malloc failure. */
dom_node_t *dom_parse(const char *html, uint32_t size);

/* Recursively free a subtree. Safe on NULL. */
void dom_free(dom_node_t *node);

/* Return the value of the named attribute (case-insensitive on name),
 * or NULL if the node has no such attribute (or isn't an element).
 * Pointer is owned by the node — do not free. */
const char *dom_get_attr(const dom_node_t *node, const char *name);

/* Depth-first search for the first element node with id="id". */
dom_node_t *dom_get_element_by_id(dom_node_t *root, const char *id);

/* Depth-first search for the first element node with the given tag
 * (case-insensitive). */
dom_node_t *dom_get_first_element_by_tag(dom_node_t *root, const char *tag);

/* Recursive debug dump to stdout. Useful for smoke tests. */
void dom_print(const dom_node_t *node, int indent);

#endif
