/* ============================================================================
 * NexxoN OS - DOM tree builder + CSS box-model layout engine
 * ----------------------------------------------------------------------------
 * Owns the parse + layout pipeline that the browser's redraw path runs
 * after each successful HTTP fetch.  The pipeline is:
 *
 *    dom_init -> dom_parse_html (tokeniser + tree builder)
 *             -> dom_apply_default_css (per-tag display, margins, fonts)
 *             -> dom_layout (recursive width assignment + line break)
 *             -> renderer walks the tree
 *
 * The parser is forgiving: missing closing tags get auto-closed when a
 * mismatched closing tag is encountered.  A stack of open elements
 * captures the current insertion point.  Self-closing tags (br, img, hr,
 * input) are emitted without pushing a new frame onto the stack.
 *
 * The CSS engine is intentionally a "default stylesheet only" affair:
 *   - block elements:   div, p, h1..h6, ul, ol, li, header, footer, body
 *   - inline elements:  span, a, b, i, strong, em, code
 *   - visual cues:      h1 = 28px / bold, h2 = 22px / bold, a = blue +
 *                       underline, code = monospace background, etc.
 *
 * Inline content is rendered as a single line and wrapped at the
 * viewport width using a naive word break.  Block elements stack
 * vertically and apply their margin/padding to their bounding box.
 * ============================================================================ */
#include "dom.h"
#include "string.h"
#include "debug.h"

#define DOM_FONT_CHAR_W   8     /* matches FONT_GLYPH_W from font.h        */
#define DOM_FONT_CHAR_H   8

/* ---------- Arena management --------------------------------------------- */
static int dom_alloc(dom_doc_t *d) {
    if (d->used >= DOM_NODE_MAX) return -1;
    int idx = d->used++;
    dom_node_t *n = &d->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->in_use      = true;
    n->parent      = -1;
    n->first_child = -1;
    n->next_sibling = -1;
    return idx;
}

void dom_init(dom_doc_t *d) {
    memset(d, 0, sizeof(*d));
    d->root = -1;
}

static void append_child(dom_doc_t *d, int parent, int child) {
    dom_node_t *p = &d->nodes[parent];
    d->nodes[child].parent = parent;
    if (p->first_child < 0) {
        p->first_child = child;
        return;
    }
    int cur = p->first_child;
    while (d->nodes[cur].next_sibling >= 0) cur = d->nodes[cur].next_sibling;
    d->nodes[cur].next_sibling = child;
}

/* ---------- HTML tokeniser ----------------------------------------------- */
static int ieq(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int is_void_tag(const char *tag) {
    static const char *voids[] = {
        "br", "img", "hr", "input", "meta", "link", "area", "base", "col",
        "embed", "source", "track", "wbr", NULL
    };
    for (int i = 0; voids[i]; i++) if (ieq(tag, voids[i])) return 1;
    return 0;
}

static void parse_attrs(const char *src, int len, dom_node_t *node) {
    int i = 0;
    /* Skip tag name. */
    while (i < len && src[i] != ' ' && src[i] != '\t') i++;
    while (i < len && node->n_attrs < DOM_ATTR_MAX) {
        while (i < len && (src[i] == ' ' || src[i] == '\t')) i++;
        if (i >= len || src[i] == '/' || src[i] == '>') break;
        dom_attr_t *a = &node->attrs[node->n_attrs++];
        int k = 0;
        while (i < len && src[i] != '=' && src[i] != ' ' && src[i] != '>' &&
               k < DOM_ATTR_KEY_MAX - 1) {
            char c = src[i++];
            if (c >= 'A' && c <= 'Z') c += 32;
            a->key[k++] = c;
        }
        a->key[k] = 0;
        a->value[0] = 0;
        if (i < len && src[i] == '=') {
            i++;
            char quote = 0;
            if (i < len && (src[i] == '"' || src[i] == '\'')) {
                quote = src[i++];
            }
            int v = 0;
            while (i < len && v < DOM_ATTR_VAL_MAX - 1) {
                if (quote && src[i] == quote) { i++; break; }
                if (!quote && (src[i] == ' ' || src[i] == '>')) break;
                a->value[v++] = src[i++];
            }
            a->value[v] = 0;
        }
    }
}

/* Strip HTTP response header (find \r\n\r\n).  Returns offset of body. */
static uint32_t skip_http_header(const char *src, uint32_t len) {
    if (len >= 5 && (src[0] == 'H' || src[0] == 'h') &&
        ieq("HTTP/", "HTTP/")) {
        for (uint32_t i = 0; i + 3 < len; i++) {
            if (src[i] == '\r' && src[i + 1] == '\n' &&
                src[i + 2] == '\r' && src[i + 3] == '\n') {
                return i + 4;
            }
        }
    }
    return 0;
}

int dom_parse_html(dom_doc_t *d, const char *html, uint32_t len) {
    /* Root <html> node. */
    int root = dom_alloc(d);
    if (root < 0) return -1;
    strcpy(d->nodes[root].tag, "html");
    d->nodes[root].kind = DOM_NODE_ELEMENT;
    d->root = root;
    /* Body node, the only block where parsing happens. */
    int body = dom_alloc(d);
    if (body < 0) return -1;
    strcpy(d->nodes[body].tag, "body");
    d->nodes[body].kind = DOM_NODE_ELEMENT;
    append_child(d, root, body);

    int stack[32];
    int sp = 0;
    stack[sp++] = body;

    uint32_t i = skip_http_header(html, len);
    char text_buf[DOM_TEXT_MAX];
    int  text_len = 0;
    bool ws = true;
    bool in_script = false;
    bool in_style  = false;

    while (i < len) {
        char c = html[i];
        if (c == '<') {
            /* Flush pending text. */
            if (text_len > 0 && !in_script && !in_style) {
                int tn = dom_alloc(d);
                if (tn < 0) break;
                d->nodes[tn].kind = DOM_NODE_TEXT;
                int cap = (text_len < DOM_TEXT_MAX - 1) ? text_len : DOM_TEXT_MAX - 1;
                memcpy(d->nodes[tn].text, text_buf, cap);
                d->nodes[tn].text[cap] = 0;
                append_child(d, stack[sp - 1], tn);
            }
            text_len = 0;
            ws = true;

            /* Find end of tag. */
            uint32_t end = i + 1;
            while (end < len && html[end] != '>') end++;
            if (end >= len) break;
            bool closing = (html[i + 1] == '/');
            const char *body_p = html + i + 1 + (closing ? 1 : 0);
            uint32_t blen = end - (i + 1) - (closing ? 1 : 0);
            if (blen == 0) { i = end + 1; continue; }
            /* Skip declarations <!doctype, <!--, ... */
            if (body_p[0] == '!') { i = end + 1; continue; }
            char tag[DOM_TAG_MAX];
            int tn = 0;
            while (tn < (int)blen && tn < DOM_TAG_MAX - 1 &&
                   body_p[tn] != ' ' && body_p[tn] != '/' &&
                   body_p[tn] != '\t' && body_p[tn] != '\r' &&
                   body_p[tn] != '\n') {
                char ch = body_p[tn++];
                if (ch >= 'A' && ch <= 'Z') ch += 32;
                tag[tn - 1] = ch;
            }
            tag[tn] = 0;

            if (in_script) {
                if (closing && ieq(tag, "script")) in_script = false;
                i = end + 1;
                continue;
            }
            if (in_style) {
                if (closing && ieq(tag, "style")) in_style = false;
                i = end + 1;
                continue;
            }
            if (closing) {
                /* Pop the stack until we find a matching open tag. */
                for (int s = sp - 1; s > 0; s--) {
                    if (ieq(d->nodes[stack[s]].tag, tag)) {
                        sp = s;
                        break;
                    }
                }
            } else {
                if (ieq(tag, "script")) { in_script = true; i = end + 1; continue; }
                if (ieq(tag, "style"))  { in_style  = true; i = end + 1; continue; }
                int node = dom_alloc(d);
                if (node < 0) break;
                d->nodes[node].kind = DOM_NODE_ELEMENT;
                strncpy(d->nodes[node].tag, tag, DOM_TAG_MAX - 1);
                parse_attrs(body_p, (int)blen, &d->nodes[node]);
                append_child(d, stack[sp - 1], node);
                if (!is_void_tag(tag) && sp < 32 && html[end - 1] != '/') {
                    stack[sp++] = node;
                }
            }
            i = end + 1;
            continue;
        }
        if (c == '&') {
            /* Tiny entity decode. */
            static const struct { const char *e; char r; } ents[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
                {"&quot;", '"'}, {"&apos;", '\''}, {"&nbsp;", ' '},
                {NULL, 0}
            };
            bool found = false;
            for (int k = 0; ents[k].e; k++) {
                uint32_t l = (uint32_t)strlen(ents[k].e);
                if (i + l <= len && strncmp(html + i, ents[k].e, l) == 0) {
                    if (text_len < DOM_TEXT_MAX - 1) text_buf[text_len++] = ents[k].r;
                    i += l;
                    found = true;
                    ws = false;
                    break;
                }
            }
            if (!found) {
                if (text_len < DOM_TEXT_MAX - 1) text_buf[text_len++] = c;
                i++;
                ws = false;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!ws && text_len < DOM_TEXT_MAX - 1) text_buf[text_len++] = ' ';
            ws = true;
            i++;
            continue;
        }
        if (text_len < DOM_TEXT_MAX - 1) text_buf[text_len++] = c;
        ws = false;
        i++;
    }
    /* Flush trailing text. */
    if (text_len > 0) {
        int tn = dom_alloc(d);
        if (tn >= 0) {
            d->nodes[tn].kind = DOM_NODE_TEXT;
            memcpy(d->nodes[tn].text, text_buf, text_len);
            d->nodes[tn].text[text_len] = 0;
            append_child(d, stack[sp - 1], tn);
        }
    }
    return d->used;
}

/* ---------- Default stylesheet ------------------------------------------ */
static const struct {
    const char *tag;
    dom_display_t disp;
    int font_size;
    int font_weight;
    int mt; int mb;
    int color;
    bool underline;
} default_rules[] = {
    { "html",   DOM_DISPLAY_BLOCK,  16, 400, 0, 0,  0xFF101018, false },
    { "body",   DOM_DISPLAY_BLOCK,  16, 400, 0, 0,  0xFF101018, false },
    { "div",    DOM_DISPLAY_BLOCK,  16, 400, 0, 4,  0xFF101018, false },
    { "p",      DOM_DISPLAY_BLOCK,  16, 400, 8, 8,  0xFF101018, false },
    { "h1",     DOM_DISPLAY_BLOCK,  28, 700, 12, 8, 0xFF001868, false },
    { "h2",     DOM_DISPLAY_BLOCK,  22, 700, 10, 6, 0xFF001868, false },
    { "h3",     DOM_DISPLAY_BLOCK,  18, 700, 8, 4,  0xFF202840, false },
    { "h4",     DOM_DISPLAY_BLOCK,  16, 700, 6, 4,  0xFF202840, false },
    { "h5",     DOM_DISPLAY_BLOCK,  14, 700, 4, 4,  0xFF202840, false },
    { "h6",     DOM_DISPLAY_BLOCK,  12, 700, 4, 4,  0xFF202840, false },
    { "ul",     DOM_DISPLAY_BLOCK,  16, 400, 6, 6,  0xFF101018, false },
    { "ol",     DOM_DISPLAY_BLOCK,  16, 400, 6, 6,  0xFF101018, false },
    { "li",     DOM_DISPLAY_BLOCK,  16, 400, 2, 2,  0xFF101018, false },
    { "header", DOM_DISPLAY_BLOCK,  16, 400, 0, 4,  0xFF101018, false },
    { "footer", DOM_DISPLAY_BLOCK,  14, 400, 4, 0,  0xFF707080, false },
    { "table",  DOM_DISPLAY_BLOCK,  16, 400, 4, 4,  0xFF101018, false },
    { "tr",     DOM_DISPLAY_BLOCK,  16, 400, 0, 0,  0xFF101018, false },
    { "hr",     DOM_DISPLAY_BLOCK,  16, 400, 6, 6,  0xFF808088, false },
    { "a",      DOM_DISPLAY_INLINE, 16, 400, 0, 0,  0xFF0040D0, true  },
    { "b",      DOM_DISPLAY_INLINE, 16, 700, 0, 0,  0xFF101018, false },
    { "strong", DOM_DISPLAY_INLINE, 16, 700, 0, 0,  0xFF101018, false },
    { "i",      DOM_DISPLAY_INLINE, 16, 400, 0, 0,  0xFF101018, false },
    { "em",     DOM_DISPLAY_INLINE, 16, 400, 0, 0,  0xFF101018, false },
    { "code",   DOM_DISPLAY_INLINE, 16, 400, 0, 0,  0xFF800040, false },
    { "span",   DOM_DISPLAY_INLINE, 16, 400, 0, 0,  0xFF101018, false },
    { "title",  DOM_DISPLAY_NONE,   16, 400, 0, 0,  0xFF101018, false },
    { "head",   DOM_DISPLAY_NONE,   16, 400, 0, 0,  0xFF101018, false },
    { NULL, DOM_DISPLAY_INLINE, 16, 400, 0, 0, 0xFF101018, false }
};

void dom_apply_default_css(dom_doc_t *d) {
    for (int i = 0; i < d->used; i++) {
        dom_node_t *n = &d->nodes[i];
        if (!n->in_use || n->kind != DOM_NODE_ELEMENT) {
            if (n->kind == DOM_NODE_TEXT) {
                n->style.display = DOM_DISPLAY_INLINE;
                n->style.font_size = 16;
                n->style.font_weight = 400;
                n->style.color_argb = 0xFF101018;
            }
            continue;
        }
        const char *tag = n->tag;
        for (int k = 0; default_rules[k].tag; k++) {
            if (ieq(tag, default_rules[k].tag)) {
                n->style.display      = default_rules[k].disp;
                n->style.font_size    = default_rules[k].font_size;
                n->style.font_weight  = default_rules[k].font_weight;
                n->style.margin_top   = default_rules[k].mt;
                n->style.margin_bottom = default_rules[k].mb;
                n->style.color_argb   = default_rules[k].color;
                n->style.underline    = default_rules[k].underline;
                break;
            }
        }
        /* Inherit colour if unset (text nodes). */
        if (n->style.color_argb == 0) n->style.color_argb = 0xFF101018;
    }
}

/* ---------- Layout ------------------------------------------------------- */
typedef struct {
    int x;
    int y;
    int line_h;
    int max_y;
} layout_state_t;

static int text_width(const char *s, int font_size) {
    int cw = (font_size * DOM_FONT_CHAR_W) / 16;
    int n = 0;
    while (s[n]) n++;
    return n * cw;
}

static int text_height(int font_size) {
    return (font_size * DOM_FONT_CHAR_H) / 16 + 2;
}

static void layout_inline_text(dom_node_t *n, layout_state_t *st, int max_x) {
    int cw = (n->style.font_size * DOM_FONT_CHAR_W) / 16;
    int ch = (n->style.font_size * DOM_FONT_CHAR_H) / 16;
    const char *s = n->text;
    if (ch + 2 > st->line_h) st->line_h = ch + 2;
    n->box.x = st->x;
    n->box.y = st->y;
    int word_start = 0, i = 0;
    while (s[i]) {
        /* Find the next break. */
        while (s[i] && s[i] != ' ') i++;
        int word_w = (i - word_start) * cw;
        if (st->x + word_w > max_x && st->x > 0) {
            st->y += st->line_h;
            st->x  = 0;
            st->line_h = ch + 2;
        }
        st->x += word_w;
        if (s[i] == ' ') { st->x += cw; i++; }
        word_start = i;
    }
    n->box.width  = st->x - n->box.x;
    n->box.height = ch + 2;
    if (st->y + st->line_h > st->max_y) st->max_y = st->y + st->line_h;
}

static void layout_node(dom_doc_t *d, int idx, int parent_w,
                        layout_state_t *st) {
    if (idx < 0) return;
    dom_node_t *n = &d->nodes[idx];
    if (!n->in_use) return;
    if (n->kind == DOM_NODE_TEXT) {
        layout_inline_text(n, st, parent_w);
        return;
    }
    if (n->style.display == DOM_DISPLAY_NONE) return;
    if (n->style.display == DOM_DISPLAY_BLOCK) {
        if (st->x != 0) { st->y += st->line_h; st->x = 0; st->line_h = 0; }
        st->y += n->style.margin_top;
        n->box.x = 0;
        n->box.y = st->y;
        n->box.width = parent_w;
        int saved_y = st->y;
        for (int c = n->first_child; c >= 0; c = d->nodes[c].next_sibling) {
            layout_node(d, c, parent_w, st);
        }
        if (st->x != 0) { st->y += st->line_h; st->x = 0; st->line_h = 0; }
        st->y += n->style.margin_bottom;
        n->box.height = st->y - saved_y;
        if (n->box.height < text_height(n->style.font_size))
            n->box.height = text_height(n->style.font_size);
        if (st->y > st->max_y) st->max_y = st->y;
    } else {
        /* Inline element: lay out children inline.  Inline element box
         * is approximated as the bounding box of all its children. */
        int saved_x = st->x;
        int saved_y = st->y;
        for (int c = n->first_child; c >= 0; c = d->nodes[c].next_sibling) {
            layout_node(d, c, parent_w, st);
        }
        n->box.x = saved_x;
        n->box.y = saved_y;
        n->box.width  = st->x - saved_x;
        n->box.height = st->line_h;
    }
}

void dom_layout(dom_doc_t *d, int viewport_w) {
    if (d->root < 0) return;
    layout_state_t st = { 0, 0, 0, 0 };
    layout_node(d, d->root, viewport_w, &st);
}

int dom_height(const dom_doc_t *d) {
    if (d->root < 0) return 0;
    return d->nodes[d->root].box.height;
}

static void visit(const dom_doc_t *d, int idx, dom_visit_cb_t cb, void *user) {
    if (idx < 0) return;
    const dom_node_t *n = &d->nodes[idx];
    if (!n->in_use) return;
    cb(n, user);
    for (int c = n->first_child; c >= 0; c = d->nodes[c].next_sibling)
        visit(d, c, cb, user);
}
void dom_visit(const dom_doc_t *d, dom_visit_cb_t cb, void *user) {
    visit(d, d->root, cb, user);
}

const char *dom_attr(const dom_node_t *n, const char *key) {
    if (!n) return NULL;
    for (int i = 0; i < n->n_attrs; i++) {
        if (ieq(n->attrs[i].key, key)) return n->attrs[i].value;
    }
    return NULL;
}
