/* ============================================================================
 * NexxoN OS - HTML DOM + CSS box model  (v1.0)
 * ----------------------------------------------------------------------------
 * In-memory representation of a parsed HTML page.  The tree is a forest
 * of dom_node_t structs that live in a single statically-allocated arena
 * (no malloc).  Each node carries:
 *
 *   * type        - ELEMENT or TEXT
 *   * tag         - element name ("div", "h1", "p", ...) — empty for text
 *   * text        - raw character data for TEXT nodes
 *   * attributes  - up to 4 (key, value) pairs (href, src, class, id)
 *   * computed    - box-model results filled by dom_layout()
 *
 * The companion CSS layer is intentionally tiny: an internal table of
 * (tag, prop, value) "stylesheet" rules drives default display values
 * (block / inline), margins, padding, and font-weight.  A user-supplied
 * <style> block parsed by css_apply_stylesheet() can override the
 * defaults on a per-tag basis.
 *
 * dom_layout() runs a single pass:
 *   1. Compute used widths starting from a viewport width.
 *   2. Recursively assign children positions (inline runs left to right,
 *      wrapping at viewport width; block-level children stacked
 *      vertically with their margin / padding applied).
 *   3. Cache (x, y, w, h) in node->box for the renderer.
 * ============================================================================ */
#ifndef NEXXON_DOM_H
#define NEXXON_DOM_H

#include "types.h"

#define DOM_TAG_MAX        16
#define DOM_TEXT_MAX       128
#define DOM_ATTR_MAX       4
#define DOM_ATTR_KEY_MAX   16
#define DOM_ATTR_VAL_MAX   80
#define DOM_NODE_MAX       512

typedef enum {
    DOM_NODE_ELEMENT = 1,
    DOM_NODE_TEXT    = 2,
} dom_node_kind_t;

typedef enum {
    DOM_DISPLAY_INLINE = 0,
    DOM_DISPLAY_BLOCK  = 1,
    DOM_DISPLAY_NONE   = 2,
} dom_display_t;

typedef struct {
    int x, y;
    int width, height;
} dom_box_t;

typedef struct {
    char key[DOM_ATTR_KEY_MAX];
    char value[DOM_ATTR_VAL_MAX];
} dom_attr_t;

typedef struct {
    dom_display_t display;
    int  font_size;            /* px, default 16 */
    int  font_weight;          /* 400 = normal, 700 = bold */
    int  margin_top;
    int  margin_bottom;
    int  padding_left;
    int  padding_right;
    int  padding_top;
    int  padding_bottom;
    int  color_argb;
    int  bg_argb;              /* 0 = transparent */
    bool underline;
} dom_style_t;

struct dom_node;
typedef struct dom_node {
    bool             in_use;
    dom_node_kind_t  kind;
    char             tag[DOM_TAG_MAX];
    char             text[DOM_TEXT_MAX];
    int              n_attrs;
    dom_attr_t       attrs[DOM_ATTR_MAX];

    int              parent;       /* index in arena, -1 for root */
    int              first_child;
    int              next_sibling;

    dom_style_t      style;
    dom_box_t        box;
} dom_node_t;

typedef struct {
    dom_node_t  nodes[DOM_NODE_MAX];
    int         root;
    int         used;
} dom_doc_t;

void dom_init        (dom_doc_t *d);
int  dom_parse_html  (dom_doc_t *d, const char *html, uint32_t len);
void dom_apply_default_css(dom_doc_t *d);

/* Single layout pass.  `viewport_w` is the available width in pixels. */
void dom_layout      (dom_doc_t *d, int viewport_w);
/* Total document height after layout (so the scrollbar can size itself). */
int  dom_height      (const dom_doc_t *d);

/* Walk every visible node in painting order (back-to-front). */
typedef void (*dom_visit_cb_t)(const dom_node_t *n, void *user);
void dom_visit       (const dom_doc_t *d, dom_visit_cb_t cb, void *user);

const char *dom_attr (const dom_node_t *n, const char *key);

#endif /* NEXXON_DOM_H */
