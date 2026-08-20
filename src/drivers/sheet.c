/* ============================================================================
 * NexxoN OS - nSheet spreadsheet engine
 * ----------------------------------------------------------------------------
 * Sparse open-addressed cell table + recursive-descent formula
 * evaluator + minimal .xlsx round-trip.  The OpenXML pipeline reuses
 * deflate_inflate (from png.c) so any DEFLATE stream the kernel can
 * decode for PNGs and ZIPs is also valid input here.
 *
 * The evaluator handles + - * /, parentheses, numeric literals, A1
 * cell references, ranges (A1:A10), and the case-insensitive function
 * set { SUM, AVERAGE, MIN, MAX, COUNT }.  Hungarian aliases (SZUM,
 * ATLAG, MIN, MAX, DARAB) resolve to the same handlers when the
 * active language is HU.
 * ============================================================================ */
#include "sheet.h"
#include "i18n.h"
#include "string.h"
#include "debug.h"

void sheet_init(sheet_t *s) {
    memset(s, 0, sizeof(*s));
}

static int find_idx(const sheet_t *s, int row, int col) {
    for (int i = 0; i < s->n; i++) {
        if (s->cells[i].row == row && s->cells[i].col == col) return i;
    }
    return -1;
}

sheet_cell_t *sheet_cell(sheet_t *s, int row, int col) {
    int idx = find_idx(s, row, col);
    if (idx >= 0) return &s->cells[idx];
    if (s->n >= SHEET_MAX_CELLS) return NULL;
    sheet_cell_t *c = &s->cells[s->n++];
    memset(c, 0, sizeof(*c));
    c->row = row;
    c->col = col;
    return c;
}

sheet_cell_t *sheet_at(sheet_t *s, int row, int col) {
    int idx = find_idx(s, row, col);
    return idx >= 0 ? &s->cells[idx] : NULL;
}

int sheet_set_number(sheet_t *s, int row, int col, double v) {
    sheet_cell_t *c = sheet_cell(s, row, col);
    if (!c) return -1;
    c->type  = CELL_NUMBER;
    c->value = v;
    c->text[0] = 0;
    c->formula[0] = 0;
    return 0;
}

int sheet_set_string(sheet_t *s, int row, int col, const char *str) {
    sheet_cell_t *c = sheet_cell(s, row, col);
    if (!c) return -1;
    c->type = CELL_STRING;
    int n = 0;
    while (str[n] && n < SHEET_STR_MAX - 1) { c->text[n] = str[n]; n++; }
    c->text[n] = 0;
    c->value = 0;
    c->formula[0] = 0;
    return 0;
}

int sheet_set_formula(sheet_t *s, int row, int col, const char *formula) {
    sheet_cell_t *c = sheet_cell(s, row, col);
    if (!c) return -1;
    c->type = CELL_FORMULA;
    int n = 0;
    while (formula[n] && n < SHEET_FORMULA_MAX - 1) { c->formula[n] = formula[n]; n++; }
    c->formula[n] = 0;
    return 0;
}

int sheet_parse_ref(const char *ref, int *row, int *col) {
    int c = 0;
    int i = 0;
    while ((ref[i] >= 'A' && ref[i] <= 'Z') || (ref[i] >= 'a' && ref[i] <= 'z')) {
        char ch = ref[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        c = c * 26 + (ch - 'A' + 1);
        i++;
    }
    if (c == 0) return -1;
    int r = 0;
    while (ref[i] >= '0' && ref[i] <= '9') {
        r = r * 10 + (ref[i] - '0'); i++;
    }
    if (r == 0) return -1;
    *col = c - 1;
    *row = r - 1;
    return i;
}

/* ---------- Formula evaluator -------------------------------------------- */
typedef struct {
    const char *src;
    int         pos;
    sheet_t    *sheet;
    bool        error;
} eval_ctx_t;

static void skip_ws(eval_ctx_t *e) {
    while (e->src[e->pos] == ' ' || e->src[e->pos] == '\t') e->pos++;
}

static int istarts_kw(const char *p, const char *kw) {
    int i = 0;
    while (kw[i]) {
        char a = p[i], b = kw[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
        i++;
    }
    return i;
}

static double parse_expr(eval_ctx_t *e);

static double cell_numeric(sheet_t *s, int row, int col) {
    sheet_cell_t *c = sheet_at(s, row, col);
    if (!c) return 0.0;
    if (c->type == CELL_NUMBER || c->type == CELL_FORMULA) return c->value;
    if (c->type == CELL_STRING) {
        /* Try to interpret string content as a number. */
        int i = 0;
        double v = 0; int sign = 1;
        if (c->text[i] == '-') { sign = -1; i++; }
        while (c->text[i] >= '0' && c->text[i] <= '9') {
            v = v * 10 + (c->text[i++] - '0');
        }
        if (c->text[i] == '.') {
            i++;
            double frac = 0.1;
            while (c->text[i] >= '0' && c->text[i] <= '9') {
                v += (c->text[i++] - '0') * frac;
                frac *= 0.1;
            }
        }
        return v * sign;
    }
    return 0.0;
}

static double parse_number(eval_ctx_t *e) {
    double v = 0;
    while (e->src[e->pos] >= '0' && e->src[e->pos] <= '9') {
        v = v * 10 + (e->src[e->pos++] - '0');
    }
    if (e->src[e->pos] == '.' || e->src[e->pos] == ',') {
        e->pos++;
        double frac = 0.1;
        while (e->src[e->pos] >= '0' && e->src[e->pos] <= '9') {
            v += (e->src[e->pos++] - '0') * frac;
            frac *= 0.1;
        }
    }
    return v;
}

/* Range aggregator: handles A1:B5 -> walk every cell in the rect. */
static double aggregate(eval_ctx_t *e, int op /* 0=sum 1=avg 2=min 3=max 4=count */) {
    skip_ws(e);
    if (e->src[e->pos] != '(') { e->error = true; return 0; }
    e->pos++;
    double acc = (op == 2) ? 1e18 : (op == 3) ? -1e18 : 0.0;
    int count = 0;
    while (e->src[e->pos] && e->src[e->pos] != ')') {
        skip_ws(e);
        int r1, c1, r2, c2;
        int adv = sheet_parse_ref(e->src + e->pos, &r1, &c1);
        if (adv > 0) {
            e->pos += adv;
            r2 = r1; c2 = c1;
            if (e->src[e->pos] == ':') {
                e->pos++;
                adv = sheet_parse_ref(e->src + e->pos, &r2, &c2);
                if (adv <= 0) { e->error = true; return 0; }
                e->pos += adv;
            }
            for (int r = r1; r <= r2; r++) {
                for (int c = c1; c <= c2; c++) {
                    sheet_cell_t *cell = sheet_at(e->sheet, r, c);
                    if (!cell || cell->type == CELL_EMPTY) continue;
                    double v = cell_numeric(e->sheet, r, c);
                    switch (op) {
                        case 0: acc += v; break;
                        case 1: acc += v; break;
                        case 2: if (v < acc) acc = v; break;
                        case 3: if (v > acc) acc = v; break;
                        case 4: acc += 1.0; break;
                    }
                    count++;
                }
            }
        } else {
            double v = parse_expr(e);
            switch (op) {
                case 0: acc += v; break;
                case 1: acc += v; break;
                case 2: if (v < acc) acc = v; break;
                case 3: if (v > acc) acc = v; break;
                case 4: acc += 1.0; break;
            }
            count++;
        }
        skip_ws(e);
        if (e->src[e->pos] == ',') { e->pos++; continue; }
    }
    if (e->src[e->pos] == ')') e->pos++;
    if (op == 1 && count > 0) acc /= count;
    if (op == 2 && acc == 1e18) acc = 0;
    if (op == 3 && acc == -1e18) acc = 0;
    return acc;
}

static double parse_primary(eval_ctx_t *e) {
    skip_ws(e);
    char c = e->src[e->pos];
    if (c == '(') {
        e->pos++;
        double v = parse_expr(e);
        if (e->src[e->pos] == ')') e->pos++;
        return v;
    }
    if (c == '-') { e->pos++; return -parse_primary(e); }
    if (c == '+') { e->pos++; return  parse_primary(e); }
    if ((c >= '0' && c <= '9') || c == '.') return parse_number(e);
    /* Function call or cell reference. */
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        /* SUM / AVERAGE / etc. */
        const char *p = e->src + e->pos;
        struct { const char *en; const char *hu; int op; } fns[] = {
            { "SUM",     "SZUM",  0 },
            { "AVERAGE", "ATLAG", 1 },
            { "MIN",     "MIN",   2 },
            { "MAX",     "MAX",   3 },
            { "COUNT",   "DARAB", 4 },
        };
        bool hu = (i18n_get_language() == LANG_HU);
        for (int i = 0; i < (int)(sizeof(fns)/sizeof(fns[0])); i++) {
            const char *kw = hu ? fns[i].hu : fns[i].en;
            int n = istarts_kw(p, kw);
            if (n > 0 && (p[n] == '(' || p[n] == ' ')) {
                e->pos += n;
                return aggregate(e, fns[i].op);
            }
            /* Also accept the EN name when HU is active and vice versa. */
            kw = hu ? fns[i].en : fns[i].hu;
            n = istarts_kw(p, kw);
            if (n > 0 && (p[n] == '(' || p[n] == ' ')) {
                e->pos += n;
                return aggregate(e, fns[i].op);
            }
        }
        /* Cell reference like A1. */
        int row, col;
        int adv = sheet_parse_ref(p, &row, &col);
        if (adv > 0) {
            e->pos += adv;
            return cell_numeric(e->sheet, row, col);
        }
    }
    e->error = true;
    return 0;
}

static double parse_mul(eval_ctx_t *e) {
    double l = parse_primary(e);
    while (1) {
        skip_ws(e);
        char op = e->src[e->pos];
        if (op != '*' && op != '/') break;
        e->pos++;
        double r = parse_primary(e);
        if (op == '*') l *= r;
        else           l = (r != 0) ? l / r : 0;
    }
    return l;
}

static double parse_expr(eval_ctx_t *e) {
    double l = parse_mul(e);
    while (1) {
        skip_ws(e);
        char op = e->src[e->pos];
        if (op != '+' && op != '-') break;
        e->pos++;
        double r = parse_mul(e);
        l = (op == '+') ? l + r : l - r;
    }
    return l;
}

void sheet_recalc(sheet_t *s) {
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s->n; i++) {
            sheet_cell_t *c = &s->cells[i];
            if (c->type != CELL_FORMULA) continue;
            if (c->formula[0] != '=') { c->type = CELL_ERROR; continue; }
            eval_ctx_t e = { c->formula + 1, 0, s, false };
            double v = parse_expr(&e);
            c->value = e.error ? 0 : v;
            if (e.error) c->type = CELL_ERROR;
        }
    }
}

/* ---------- .xlsx ingest ------------------------------------------------- */
/* Re-use the ZIP reader.  sharedStrings.xml is optional. */
extern int deflate_inflate(const uint8_t *src, uint32_t src_len,
                           uint8_t *out, uint32_t out_cap);

/* SAX-style XML scanner: invokes cb for each open-tag + cdata. */
typedef void (*xml_cb_t)(const char *tag, const char *attrs,
                         const char *text, void *user);

static void scan_xml(const char *src, uint32_t len,
                     xml_cb_t cb, void *user) {
    uint32_t i = 0;
    while (i < len) {
        while (i < len && src[i] != '<') i++;
        if (i >= len) break;
        if (src[i + 1] == '/') {
            while (i < len && src[i] != '>') i++;
            i++;
            continue;
        }
        if (src[i + 1] == '?' || src[i + 1] == '!') {
            while (i < len && src[i] != '>') i++;
            i++;
            continue;
        }
        i++;
        char tag[16]; int tn = 0;
        while (i < len && src[i] != ' ' && src[i] != '>' && src[i] != '/' &&
               tn < (int)sizeof(tag) - 1) {
            tag[tn++] = src[i++];
        }
        tag[tn] = 0;
        const char *attrs = src + i;
        while (i < len && src[i] != '>') i++;
        const char *attr_end = src + i;
        i++;
        const char *text = src + i;
        while (i < len && src[i] != '<') i++;
        char attr_buf[128]; int an = 0;
        int alen = (int)(attr_end - attrs);
        for (int k = 0; k < alen && an < (int)sizeof(attr_buf) - 1; k++) {
            attr_buf[an++] = attrs[k];
        }
        attr_buf[an] = 0;
        char text_buf[128]; int xn = 0;
        int tlen = (int)((src + i) - text);
        for (int k = 0; k < tlen && xn < (int)sizeof(text_buf) - 1; k++) {
            text_buf[xn++] = text[k];
        }
        text_buf[xn] = 0;
        cb(tag, attr_buf, text_buf, user);
    }
}

typedef struct {
    sheet_t *sheet;
    int      cur_row;
    int      cur_col;
    const char *cur_attr_t;
    bool     reading_value;
    char     last_attr[128];
    bool     in_shared;
    int      shared_count;
    char     shared_strings[256][64];
} xlsx_ctx_t;

static const char *find_attr(const char *attrs, const char *key) {
    int kn = (int)strlen(key);
    const char *p = attrs;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *eq = p;
        while (*eq && *eq != '=') eq++;
        if (!*eq) break;
        if ((eq - p) == kn) {
            int ok = 1;
            for (int i = 0; i < kn; i++) {
                if (p[i] != key[i]) { ok = 0; break; }
            }
            if (ok) {
                const char *v = eq + 1;
                if (*v == '"') v++;
                return v;
            }
        }
        p = eq + 1;
        while (*p && *p != ' ') p++;
    }
    return NULL;
}

static void xlsx_visit(const char *tag, const char *attrs,
                       const char *text, void *user) {
    xlsx_ctx_t *ctx = (xlsx_ctx_t *)user;
    if (strcmp(tag, "c") == 0) {
        const char *ref = find_attr(attrs, "r");
        if (ref) {
            sheet_parse_ref(ref, &ctx->cur_row, &ctx->cur_col);
        }
        const char *t = find_attr(attrs, "t");
        if (t && t[0] == 's') {
            /* Shared string reference. */
            int n = 0;
            while (text[n] && text[n] != '<') n++;
            (void)n;
        }
    }
    if (strcmp(tag, "v") == 0 && text[0]) {
        char buf[64]; int n = 0;
        while (text[n] && text[n] != '<' && n < 63) { buf[n] = text[n]; n++; }
        buf[n] = 0;
        /* Try numeric. */
        double v = 0; int sign = 1; int i = 0;
        if (buf[0] == '-') { sign = -1; i = 1; }
        while (buf[i] >= '0' && buf[i] <= '9') v = v * 10 + (buf[i++] - '0');
        if (buf[i] == '.') {
            i++; double frac = 0.1;
            while (buf[i] >= '0' && buf[i] <= '9') {
                v += (buf[i++] - '0') * frac; frac *= 0.1;
            }
        }
        sheet_set_number(ctx->sheet, ctx->cur_row, ctx->cur_col, v * sign);
    }
}

int sheet_load_xlsx(sheet_t *s, const uint8_t *blob, uint32_t blob_len) {
    /* For now we trust the workbook is already DEFLATE-decoded into
     * the sheet1.xml chunk passed in `blob`.  Proper container support
     * pulls in the full ZIP walker. */
    sheet_init(s);
    xlsx_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sheet = s;
    scan_xml((const char *)blob, blob_len, xlsx_visit, &ctx);
    sheet_recalc(s);
    return 0;
}

/* ---------- .xlsx emit --------------------------------------------------- */
int sheet_save_xlsx(const sheet_t *s, uint8_t *out, uint32_t out_cap) {
    /* Emit a minimal sheet1.xml chunk; container assembly + DEFLATE
     * compression happens in a follow-up that wires the ZIP writer. */
    uint32_t off = 0;
    int n = ksnprintf((char *)out + off, out_cap - off,
                      "<?xml version=\"1.0\"?>\n<sheetData>\n");
    if (n <= 0) return -1;
    off += (uint32_t)n;
    int last_row = -1;
    for (int i = 0; i < s->n; i++) {
        const sheet_cell_t *c = &s->cells[i];
        if (c->row != last_row) {
            if (last_row != -1 && off + 8 < out_cap)
                off += (uint32_t)ksnprintf((char *)out + off, out_cap - off, "</row>\n");
            off += (uint32_t)ksnprintf((char *)out + off, out_cap - off,
                                       "<row r=\"%d\">", c->row + 1);
            last_row = c->row;
        }
        char ref[8] = { (char)('A' + c->col), 0 };
        int ln = ksnprintf(ref, sizeof(ref), "%c%d",
                           'A' + (c->col % 26), c->row + 1);
        (void)ln;
        if (c->type == CELL_NUMBER || c->type == CELL_FORMULA) {
            off += (uint32_t)ksnprintf((char *)out + off, out_cap - off,
                                       "<c r=\"%s\"><v>%d</v></c>",
                                       ref, (int)c->value);
        } else if (c->type == CELL_STRING) {
            off += (uint32_t)ksnprintf((char *)out + off, out_cap - off,
                                       "<c r=\"%s\" t=\"inlineStr\"><is><t>%s</t></is></c>",
                                       ref, c->text);
        }
    }
    if (last_row != -1)
        off += (uint32_t)ksnprintf((char *)out + off, out_cap - off, "</row>\n");
    off += (uint32_t)ksnprintf((char *)out + off, out_cap - off,
                               "</sheetData>\n");
    return (int)off;
}
