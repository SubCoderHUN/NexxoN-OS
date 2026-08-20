/* ============================================================================
 * NexxoN OS - NexSheet  (Excel-style spreadsheet GUI editor)
 * ----------------------------------------------------------------------------
 * GUI wrapper around the existing sheet.c formula evaluator.  The data
 * model is the sparse sheet_t with one parallel cell-formatting record
 * per live cell (bold/italic/underline + horizontal/vertical alignment
 * + foreground/background colour + font size + merge-anchor info).
 *
 * Window layout (top to bottom):
 *
 *      +----------------------------------------+
 *      | menu bar  [File] [Edit] [Format] [...] |
 *      | toolbar   [B] [I] [U] | font 8/12/16/24|
 *      |           | Fg Bg | L C R | T C B      |
 *      |           | Merge | Save  | Open       |
 *      +----------------------------------------+
 *      | formula bar:  A1  |  =SUM(B1:B10)      |
 *      +----------------------------------------+
 *      |       A     B     C     D     E   ...  |
 *      |  1                                     |
 *      |  2                                     |
 *      | ...                                    |
 *      +----------------------------------------+
 *      | status bar                             |
 *      +----------------------------------------+
 *
 * File I/O:
 *      .xlsx via sheet_load_xlsx / sheet_save_xlsx (delegated)
 *      .csv  via local parser / writer (RFC 4180 with "" escapes)
 *
 * The toolbar buttons mutate the *currently selected* cell (or active
 * range when the user has multi-selected via Shift+arrow).  All
 * formatting is per-cell; there is no row/column-level format yet.
 *
 * Cell merging stores width/height in the anchor's cell_fmt_t.  Spanned
 * cells write the anchor's (row,col) into their own mr_anchor_*, so
 * paint can skip them and clicks resolve back to the anchor.
 * ============================================================================ */
#include "apps.h"
#include "window.h"
#include "icons.h"
#include "gfx.h"
#include "theme.h"
#include "font.h"
#include "vga.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "keyboard.h"
#include "pit.h"
#include "i18n.h"
#include "sheet.h"
#include "nxfs.h"
#include "dialogs.h"

/* ---- Geometry -------------------------------------------------------- */
#define NS_W                 980
#define NS_H                 660
#define NS_MIN_W             720
#define NS_MIN_H             480

#define NS_MENU_H            22
#define NS_TOOLBAR_H         32
#define NS_FBAR_H            24
#define NS_HEADER_H          22       /* column-letter row              */
#define NS_HEADER_W          40       /* row-number column              */
#define NS_STATUS_H          22

#define NS_DEFAULT_COL_W     86
#define NS_DEFAULT_ROW_H     20
#define NS_MIN_COL_W         32
#define NS_MIN_ROW_H         14

#define NS_MAX_VIEW_COLS     SHEET_MAX_COLS   /* 32 */
#define NS_MAX_VIEW_ROWS     SHEET_MAX_ROWS   /* 1024 */

/* ---- Palette --------------------------------------------------------- */
#define NS_BG                0xFFFFFFFF
#define NS_GRID              0xFFD0D0D6
#define NS_HDR_BG            0xFFE8E8EE
#define NS_HDR_FG            0xFF303040
#define NS_HDR_SEL_BG        0xFF4080E0
#define NS_HDR_SEL_FG        0xFFFFFFFF
#define NS_FBAR_BG           0xFFF6F6FA
#define NS_FBAR_FG           0xFF1A1A1F
#define NS_TBAR_BG           0xFFE6E8EE
#define NS_TBAR_FG           0xFF202028
#define NS_TBAR_BTN          0xFFD0D4DC
#define NS_TBAR_BTN_HOT      0xFFFFFFFF
#define NS_TBAR_BTN_ON       0xFFFFE066
#define NS_MENU_BG           0xFFEFEFF4
#define NS_MENU_FG           0xFF1A1A1F
#define NS_STATUS_BG         0xFFEFEFF4
#define NS_STATUS_FG         0xFF505058
#define NS_CELL_SEL          0x404080FF        /* blue 25% alpha */
#define NS_CELL_FG_DEFAULT   0xFF000000
#define NS_CELL_BG_DEFAULT   0x00000000        /* alpha 0 = no fill, show NS_BG */
#define NS_RANGE_SEL_OUTLINE 0xFF1860C8

/* ---- Per-cell formatting (parallel array indexed by sheet_t::cells) -- */
typedef struct {
    uint8_t  used:1;           /* 1 if this slot mirrors a live cell      */
    uint8_t  bold:1;
    uint8_t  italic:1;
    uint8_t  underline:1;
    uint8_t  halign:2;         /* 0=auto 1=left 2=center 3=right          */
    uint8_t  valign:2;         /* 0=auto 1=top 2=center 3=bottom          */
    uint8_t  font_scale;       /* 1..4 (8/16/24/32 px tall), 0 = default 1 */
    uint8_t  font_family;      /* 0=sans 1=mono 2=serif (visual hint only) */
    uint8_t  _pad;
    uint32_t fg_color;         /* 0 = NS_CELL_FG_DEFAULT */
    uint32_t bg_color;         /* alpha 0 = transparent                    */
    int16_t  mr_h;             /* >= 1; 1 = single cell                    */
    int16_t  mr_w;
    int16_t  mr_anchor_row;    /* if part of merge, anchor coords          */
    int16_t  mr_anchor_col;
} ns_fmt_t;

/* ---- Workbook + UI state -------------------------------------------- */
static sheet_t   g_book;
static ns_fmt_t  g_fmt[SHEET_MAX_CELLS];

/* Column widths / row heights — sparse "0 = default" lookups. */
static uint16_t  g_col_w[NS_MAX_VIEW_COLS];
static uint16_t  g_row_h[NS_MAX_VIEW_ROWS];

/* Active cell + selection range (range is [sel_r0..sel_r1] x [sel_c0..sel_c1]). */
static int       g_active_r = 0;
static int       g_active_c = 0;
static int       g_sel_r0 = 0, g_sel_c0 = 0;
static int       g_sel_r1 = 0, g_sel_c1 = 0;

/* Scroll offset of the visible grid (in cells). */
static int       g_scroll_row = 0;
static int       g_scroll_col = 0;

/* Edit mode: formula-bar input buffer + cursor. */
static bool      g_editing       = false;
static char      g_edit_buf[SHEET_FORMULA_MAX];
static int       g_edit_len      = 0;
static int       g_edit_caret    = 0;

/* Column-drag resize state. */
static int       g_resize_col    = -1;
static int       g_resize_start_x= 0;
static int       g_resize_start_w= 0;
static int       g_resize_row    = -1;
static int       g_resize_start_y= 0;
static int       g_resize_start_h= 0;

/* Open file (display name, may be ""). */
static char      g_filename[NXFS_NAME_MAX] = {0};

static window_t *g_ns_win = NULL;
static char      g_ns_status[160] = "Ready";

/* ---- Tiny font palette swatches the toolbar exposes ------------------ */
static const uint32_t NS_FG_PALETTE[8] = {
    0xFF000000, 0xFFC00000, 0xFFD06000, 0xFFC0A000,
    0xFF008060, 0xFF1860C8, 0xFF6020A0, 0xFFFFFFFF,
};
static const uint32_t NS_BG_PALETTE[8] = {
    0x00000000,                                 /* "no fill"          */
    0xFFFFF099, 0xFFD0F0FF, 0xFFDFFADF,
    0xFFFFE0E8, 0xFFE8E8EE, 0xFFFFB060, 0xFFB0B0B0,
};

/* ====================================================================
 *                          Helpers
 * ==================================================================== */

static int ns_col_width(int col) {
    if (col < 0 || col >= NS_MAX_VIEW_COLS) return NS_DEFAULT_COL_W;
    return g_col_w[col] ? g_col_w[col] : NS_DEFAULT_COL_W;
}

static int ns_row_height(int row) {
    if (row < 0 || row >= NS_MAX_VIEW_ROWS) return NS_DEFAULT_ROW_H;
    return g_row_h[row] ? g_row_h[row] : NS_DEFAULT_ROW_H;
}

static int ns_grid_top(const window_t *w) {
    (void)w;
    return NS_MENU_H + NS_TOOLBAR_H + NS_FBAR_H;
}

static int ns_grid_bottom(const window_t *w) {
    return (int)w->content.height - NS_STATUS_H;
}

/* Look up the parallel fmt slot for a (row,col) cell — returns NULL if
 * the cell isn't materialised in g_book.  Use ns_fmt_get_create to
 * force creation alongside the cell. */
static ns_fmt_t *ns_fmt_lookup(int row, int col) {
    for (int i = 0; i < g_book.n; i++) {
        if (g_book.cells[i].row == row && g_book.cells[i].col == col) {
            /* A materialised cell whose fmt slot was never initialised via
             * ns_fmt_get_create is all-zero — crucially mr_anchor_row/col
             * are 0, NOT -1, which the merge mid-span checks would read as
             * "anchored at A1".  That made every data cell except A1
             * invisible (paint skip) and un-clickable (click redirected to
             * A1).  Treat unused slots exactly like "no fmt". */
            return g_fmt[i].used ? &g_fmt[i] : NULL;
        }
    }
    return NULL;
}

static ns_fmt_t *ns_fmt_get_create(int row, int col) {
    sheet_cell_t *c = sheet_cell(&g_book, row, col);
    if (!c) return NULL;
    int idx = (int)(c - g_book.cells);
    ns_fmt_t *f = &g_fmt[idx];
    if (!f->used) {
        memset(f, 0, sizeof(*f));
        f->used = 1;
        f->mr_h = 1;
        f->mr_w = 1;
        f->mr_anchor_row = -1;
        f->mr_anchor_col = -1;
    }
    return f;
}

/* Append "A", "B", ..., "Z", "AA", ... into out (NUL-terminated). */
static void ns_col_label(int col, char *out, int cap) {
    char tmp[8];
    int n = 0;
    int c = col;
    do {
        tmp[n++] = (char)('A' + (c % 26));
        c = c / 26 - 1;
    } while (c >= 0 && n < (int)sizeof(tmp));
    int j = 0;
    while (n > 0 && j < cap - 1) out[j++] = tmp[--n];
    out[j] = 0;
}

static void ns_cell_label(int row, int col, char *out, int cap) {
    char cl[8];
    ns_col_label(col, cl, sizeof(cl));
    ksnprintf(out, cap, "%s%d", cl, row + 1);
}

/* Format a numeric value into a short human string (drops trailing zeros). */
static void ns_fmt_double(double v, char *out, int cap) {
    if (cap <= 0) return;
    if (v != v) { ksnprintf(out, cap, "#NaN"); return; }
    /* integer fast path */
    if (v >= -2147483648.0 && v <= 2147483647.0) {
        int iv = (int)v;
        if ((double)iv == v) {
            ksnprintf(out, cap, "%d", iv);
            return;
        }
    }
    /* fixed 4-decimal, then trim trailing zeros + dot */
    int sign = (v < 0) ? 1 : 0;
    if (sign) v = -v;
    int whole = (int)v;
    double frac = v - (double)whole;
    int micro = (int)(frac * 10000.0 + 0.5);
    if (micro >= 10000) { whole++; micro -= 10000; }
    char tmp[40];
    ksnprintf(tmp, sizeof(tmp), "%s%d.%04d",
              sign ? "-" : "", whole, micro);
    int n = (int)strlen(tmp);
    while (n > 0 && tmp[n - 1] == '0') n--;
    if (n > 0 && tmp[n - 1] == '.') n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, tmp, (size_t)n);
    out[n] = 0;
}

/* Render-time display string for a cell (formula -> value, etc.). */
static void ns_cell_display(const sheet_cell_t *c, char *out, int cap) {
    if (!c) { if (cap > 0) out[0] = 0; return; }
    switch (c->type) {
        case CELL_EMPTY:  out[0] = 0; break;
        case CELL_STRING: strncpy(out, c->text, (size_t)cap - 1);
                          out[cap - 1] = 0; break;
        case CELL_FORMULA:
        case CELL_NUMBER: ns_fmt_double(c->value, out, cap); break;
        case CELL_ERROR:  strncpy(out, "#ERR", (size_t)cap - 1);
                          out[cap - 1] = 0; break;
    }
}

/* Convert a freshly-typed string into a CELL_NUMBER / CELL_STRING /
 * CELL_FORMULA mutation.  Mirrors editor_set_input from sheet.c. */
static void ns_apply_input(int row, int col, const char *src) {
    if (!src) return;
    if (src[0] == 0) {
        sheet_cell_t *c = sheet_at(&g_book, row, col);
        if (c) { c->type = CELL_EMPTY; c->value = 0;
                 c->text[0] = 0; c->formula[0] = 0; }
        return;
    }
    if (src[0] == '=') {
        sheet_set_formula(&g_book, row, col, src);
        sheet_recalc(&g_book);
        return;
    }
    /* Try numeric */
    const char *p = src;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    bool has_digit = false;
    double whole = 0;
    while (*p >= '0' && *p <= '9') { whole = whole * 10 + (*p - '0'); p++; has_digit = true; }
    double frac = 0, scale = 1;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') { frac = frac * 10 + (*p - '0'); scale *= 10; p++; has_digit = true; }
    }
    if (has_digit && *p == 0) {
        sheet_set_number(&g_book, row, col, sign * (whole + frac / scale));
        return;
    }
    /* Fallback: string */
    sheet_set_string(&g_book, row, col, src);
}

static void ns_set_status(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    kvsnprintf(g_ns_status, sizeof(g_ns_status), fmt, ap);
    va_end(ap);
}

/* ====================================================================
 *                Layout: cell-to-pixel mapping
 * ==================================================================== */

static int ns_col_x(int col) {
    int x = NS_HEADER_W;
    for (int c = g_scroll_col; c < col; c++) x += ns_col_width(c);
    return x;
}

static int ns_row_y(const window_t *w, int row) {
    int y = ns_grid_top(w) + NS_HEADER_H;
    for (int r = g_scroll_row; r < row; r++) y += ns_row_height(r);
    return y;
}

/* Reverse: given a content-relative pixel, find the (row, col).  Returns
 * row=-1/col=-1 if the pixel is in a header area.  Also fills sub-pixel
 * offset for edge-resize detection. */
static void ns_hit_cell(const window_t *w, int x, int y,
                        int *out_row, int *out_col) {
    int grid_top = ns_grid_top(w);
    if (out_row) *out_row = -1;
    if (out_col) *out_col = -1;

    /* Column index */
    if (x >= NS_HEADER_W && out_col) {
        int cx = NS_HEADER_W;
        for (int c = g_scroll_col; c < NS_MAX_VIEW_COLS; c++) {
            int cw = ns_col_width(c);
            if (x < cx + cw) { *out_col = c; break; }
            cx += cw;
            if (cx >= (int)w->content.width) break;
        }
    }
    /* Row index */
    if (y >= grid_top + NS_HEADER_H && out_row) {
        int ry = grid_top + NS_HEADER_H;
        for (int r = g_scroll_row; r < NS_MAX_VIEW_ROWS; r++) {
            int rh = ns_row_height(r);
            if (y < ry + rh) { *out_row = r; break; }
            ry += rh;
            if (ry >= (int)w->content.height - NS_STATUS_H) break;
        }
    }
}

/* If the cell at (row,col) is the head of a multi-cell merge, return
 * the spanned width in cells / height in cells.  If it is mid-span,
 * return -1 in *cols.  If single, return 1. */
static int ns_merge_span(int row, int col, int *out_cols, int *out_rows) {
    ns_fmt_t *f = ns_fmt_lookup(row, col);
    if (!f || !f->used) {
        if (out_cols) *out_cols = 1;
        if (out_rows) *out_rows = 1;
        return 0;
    }
    if (f->mr_anchor_row >= 0 && f->mr_anchor_col >= 0 &&
        (f->mr_anchor_row != row || f->mr_anchor_col != col)) {
        /* mid-span */
        if (out_cols) *out_cols = -1;
        if (out_rows) *out_rows = -1;
        return -1;
    }
    if (out_cols) *out_cols = (f->mr_w > 0) ? f->mr_w : 1;
    if (out_rows) *out_rows = (f->mr_h > 0) ? f->mr_h : 1;
    return 0;
}

/* ====================================================================
 *                File I/O — CSV
 * ==================================================================== */

/* Append a CSV-quoted field to buf at *pos.  Doubles embedded quotes,
 * wraps in quotes if the field contains comma/quote/newline. */
static void ns_csv_emit_field(char *buf, uint32_t cap, uint32_t *pos,
                              const char *s) {
    bool needs_quote = false;
    for (const char *p = s; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            needs_quote = true; break;
        }
    }
    if (*pos + 1 >= cap) return;
    if (needs_quote && *pos + 1 < cap) buf[(*pos)++] = '"';
    for (const char *p = s; *p && *pos + 2 < cap; p++) {
        if (*p == '"' && *pos + 2 < cap) buf[(*pos)++] = '"';
        buf[(*pos)++] = *p;
    }
    if (needs_quote && *pos + 1 < cap) buf[(*pos)++] = '"';
}

static int ns_save_csv(uint32_t inode) {
    static uint8_t out[64 * 1024];
    uint32_t pos = 0;

    /* Find the workbook extents. */
    int max_r = 0, max_c = 0;
    for (int i = 0; i < g_book.n; i++) {
        if (g_book.cells[i].type == CELL_EMPTY) continue;
        if (g_book.cells[i].row > max_r) max_r = g_book.cells[i].row;
        if (g_book.cells[i].col > max_c) max_c = g_book.cells[i].col;
    }
    for (int r = 0; r <= max_r; r++) {
        for (int c = 0; c <= max_c; c++) {
            char buf[SHEET_STR_MAX + 8];
            sheet_cell_t *cell = sheet_at(&g_book, r, c);
            if (cell && cell->type == CELL_FORMULA) {
                strncpy(buf, cell->formula, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = 0;
            } else {
                ns_cell_display(cell, buf, sizeof(buf));
            }
            ns_csv_emit_field((char *)out, sizeof(out), &pos, buf);
            if (c < max_c && pos + 1 < sizeof(out)) out[pos++] = ',';
        }
        if (pos + 2 < sizeof(out)) { out[pos++] = '\r'; out[pos++] = '\n'; }
    }
    return nxfs_write_file(inode, out, pos);
}

static int ns_load_csv(uint32_t inode) {
    static uint8_t in[64 * 1024];
    uint32_t got = 0;
    int r = nxfs_read_file(inode, in, sizeof(in) - 1, &got);
    if (r != NXFS_OK) return r;
    in[got] = 0;

    sheet_init(&g_book);
    memset(g_fmt, 0, sizeof(g_fmt));

    int row = 0, col = 0;
    char field[SHEET_STR_MAX];
    int fn = 0;
    bool in_quote = false;

    for (uint32_t i = 0; i < got + 1; i++) {
        char ch = (i < got) ? (char)in[i] : '\0';
        if (in_quote) {
            if (ch == '"' && i + 1 < got && in[i + 1] == '"') {
                if (fn < (int)sizeof(field) - 1) field[fn++] = '"';
                i++;
            } else if (ch == '"') {
                in_quote = false;
            } else if (ch == 0) {
                in_quote = false;
                goto commit_field;
            } else {
                if (fn < (int)sizeof(field) - 1) field[fn++] = ch;
            }
            continue;
        }
        if (ch == '"' && fn == 0) { in_quote = true; continue; }
        if (ch == ',' || ch == '\n' || ch == '\r' || ch == '\0') {
        commit_field:
            field[fn] = 0;
            if (fn > 0 || ch == ',') {
                ns_apply_input(row, col, field);
            }
            fn = 0;
            if (ch == ',') { col++; }
            else if (ch == '\n') { row++; col = 0; }
            else if (ch == '\r') { /* swallow; next \n triggers row++ */ }
            if (ch == 0) break;
            continue;
        }
        if (fn < (int)sizeof(field) - 1) field[fn++] = ch;
    }
    sheet_recalc(&g_book);
    return NXFS_OK;
}

/* ====================================================================
 *                File I/O — XLSX (delegated)
 * ==================================================================== */
static int ns_save_xlsx(uint32_t inode) {
    static uint8_t out[256 * 1024];
    int n = sheet_save_xlsx(&g_book, out, sizeof(out));
    if (n < 0) return n;
    return nxfs_write_file(inode, out, (uint32_t)n);
}

static int ns_load_xlsx(uint32_t inode) {
    static uint8_t in[256 * 1024];
    uint32_t got = 0;
    int r = nxfs_read_file(inode, in, sizeof(in), &got);
    if (r != NXFS_OK) return r;
    sheet_init(&g_book);
    memset(g_fmt, 0, sizeof(g_fmt));
    int rr = sheet_load_xlsx(&g_book, in, got);
    sheet_recalc(&g_book);
    return rr;
}

/* Pick CSV vs XLSX based on filename suffix. */
static bool ns_path_is_xlsx(const char *name) {
    int n = (int)strlen(name);
    if (n < 5) return false;
    const char *suf = name + n - 5;
    return (suf[0] == '.' &&
            (suf[1] == 'x' || suf[1] == 'X') &&
            (suf[2] == 'l' || suf[2] == 'L') &&
            (suf[3] == 's' || suf[3] == 'S') &&
            (suf[4] == 'x' || suf[4] == 'X'));
}

static int ns_save_to_name(const char *name) {
    if (!name || !name[0]) return -1;
    uint32_t ino = 0;
    if (nxfs_resolve(0, name, &ino) != NXFS_OK) {
        if (nxfs_create_file(0, name, &ino) != NXFS_OK) return -1;
    }
    int r;
    if (ns_path_is_xlsx(name)) r = ns_save_xlsx(ino);
    else                       r = ns_save_csv(ino);
    if (r == NXFS_OK) {
        strncpy(g_filename, name, sizeof(g_filename) - 1);
        g_filename[sizeof(g_filename) - 1] = 0;
        ns_set_status("Saved %s", name);
    } else {
        ns_set_status("Save failed (%d)", r);
    }
    return r;
}

static int ns_open_from_name(const char *name) {
    if (!name || !name[0]) return -1;
    uint32_t ino = 0;
    if (nxfs_resolve(0, name, &ino) != NXFS_OK) {
        ns_set_status("Open: %s not found", name);
        return -1;
    }
    int r;
    if (ns_path_is_xlsx(name)) r = ns_load_xlsx(ino);
    else                       r = ns_load_csv(ino);
    if (r == NXFS_OK) {
        strncpy(g_filename, name, sizeof(g_filename) - 1);
        g_filename[sizeof(g_filename) - 1] = 0;
        ns_set_status("Loaded %s", name);
    } else {
        ns_set_status("Load failed (%d)", r);
    }
    return r;
}

/* ====================================================================
 *                Drawing
 * ==================================================================== */
static void ns_redraw(void);   /* fwd */

/* ---- Dropdown menu state and tables ----------------------------------- */
typedef struct { const char *lbl; bool sep; } ns_mi_t;

static const ns_mi_t g_ns_file_mi[] = {
    {"New Sheet",  false}, {"Open...",    false}, {"Save",       false},
    {"Save As...", false}, {NULL,         true }, {"Close",      false},
    {NULL,         false}
};
static const ns_mi_t g_ns_edit_mi[] = {
    {"Select All", false}, {"Delete",     false},
    {NULL,         false}
};
static const ns_mi_t g_ns_fmt_mi[] = {
    {"Bold",       false}, {"Italic",     false}, {"Underline",  false},
    {NULL,         true }, {"Merge Cells",false}, {"Unmerge",    false},
    {NULL,         false}
};
static const ns_mi_t g_ns_view_mi[] = {
    {"Go to A1",   false},
    {NULL,         false}
};
static const ns_mi_t g_ns_help_mi[] = {
    {"About NexSheet", false},
    {NULL,             false}
};
static const ns_mi_t *g_ns_ddrop[5] = {
    g_ns_file_mi, g_ns_edit_mi, g_ns_fmt_mi, g_ns_view_mi, g_ns_help_mi
};
static int  g_ns_menu_open    = -1;
static int  g_ns_menu_hot     = -1;
static int  g_ns_menu_item_x[5];
static int  g_ns_menu_item_w[5];

static bool g_ns_drag_sel  = false;
static int  g_ns_drag_r0   = -1;
static int  g_ns_drag_c0   = -1;

#define NS_DDROP_ITEM_H  (FONT_GLYPH_H + 6)
#define NS_DDROP_W       180

/* Draw a single rectangle outline. */
static void ns_outline(draw_target_t *t, int x, int y, int w, int h, uint32_t c) {
    gfx_draw_rect(t, x, y, w, h, c);
}

/* Draw a flat button on the toolbar.  Returns true if hot. */
static bool ns_draw_toolbar_btn(draw_target_t *t, int x, int y, int w, int h,
                                 const char *lbl, bool active,
                                 int mx, int my) {
    bool hot = mx >= x && mx < x + w && my >= y && my < y + h;
    uint32_t bg = active ? NS_TBAR_BTN_ON :
                  hot    ? NS_TBAR_BTN_HOT : NS_TBAR_BTN;
    gfx_fill_rect(t, x, y, w, h, bg);
    ns_outline(t, x, y, w, h, 0xFF8090A0);
    int tx = x + (w - (int)strlen(lbl) * FONT_GLYPH_W) / 2;
    int ty = y + (h - FONT_GLYPH_H) / 2;
    gfx_draw_string(t, tx, ty, lbl, NS_TBAR_FG, bg);
    return hot;
}

static int ns_swatch_x(int kind, int idx) {
    /* Static layout: pinned next to "Fg"/"Bg" labels (computed at draw). */
    (void)kind; (void)idx;
    return 0;
}

/* Paint the menu bar and any open dropdown. */

static void ns_paint_dropdown(draw_target_t *t, int menu_idx) {
    const ns_mi_t *items = g_ns_ddrop[menu_idx];
    int x = g_ns_menu_item_x[menu_idx];
    if (x + NS_DDROP_W > (int)t->width) x = (int)t->width - NS_DDROP_W;
    int y = NS_MENU_H;

    /* Measure total height. */
    int h = 4;
    for (int i = 0; items[i].lbl || items[i].sep; i++)
        h += items[i].sep ? 5 : NS_DDROP_ITEM_H;

    /* Shadow */
    gfx_blend_rect(t, x + 3, y + 3, NS_DDROP_W, h, 0x50000000u);
    /* Background + border */
    gfx_fill_rect(t, x, y, NS_DDROP_W, h, 0xFFF8F8FCu);
    gfx_draw_rect(t, x, y, NS_DDROP_W, h, 0xFF8090A0u);

    int iy = y + 2;
    int idx = 0;
    for (int i = 0; items[i].lbl || items[i].sep; i++) {
        if (items[i].sep) {
            gfx_draw_hline(t, x + 4, iy + 2, NS_DDROP_W - 8, 0xFFB0B0B8u);
            iy += 5;
        } else {
            bool hot = (g_ns_menu_hot == idx);
            uint32_t ibg = hot ? NS_RANGE_SEL_OUTLINE : 0xFFF8F8FCu;
            uint32_t ifg = hot ? 0xFFFFFFFFu : NS_MENU_FG;
            if (hot) gfx_fill_rect(t, x + 1, iy, NS_DDROP_W - 2,
                                   NS_DDROP_ITEM_H, ibg);
            gfx_draw_string(t, x + 8, iy + (NS_DDROP_ITEM_H - FONT_GLYPH_H) / 2,
                            items[i].lbl, ifg, ibg);
            iy += NS_DDROP_ITEM_H;
            idx++;
        }
    }
}

static void ns_paint_menu(draw_target_t *t) {
    gfx_fill_rect(t, 0, 0, (int)t->width, NS_MENU_H, NS_MENU_BG);
    gfx_blend_rect(t, 0, 0, (int)t->width, NS_MENU_H / 2, GLASS_GLOSS);
    gfx_blend_rect(t, 0, NS_MENU_H - 1, (int)t->width, 1, GLASS_EDGE_DARK);
    static const char *labels[] = { "File", "Edit", "Format", "View", "Help" };
    int x = 8;
    for (int i = 0; i < 5; i++) {
        int lw = (int)strlen(labels[i]) * FONT_GLYPH_W;
        int iw = lw + 14;
        g_ns_menu_item_x[i] = x;
        g_ns_menu_item_w[i] = iw;
        bool hot = (g_ns_menu_open == i);
        uint32_t mbg = hot ? NS_RANGE_SEL_OUTLINE : NS_MENU_BG;
        uint32_t mfg = hot ? 0xFFFFFFFFu : NS_MENU_FG;
        if (hot) gfx_fill_rect(t, x, 0, iw, NS_MENU_H, mbg);
        gfx_draw_string(t, x + 7, (NS_MENU_H - FONT_GLYPH_H) / 2,
                        labels[i], mfg, mbg);
        x += iw;
    }
    if (g_ns_menu_open >= 0) ns_paint_dropdown(t, g_ns_menu_open);
}

/* Paint the toolbar.  We re-compute button hit rectangles each frame
 * and store them in static globals for the click handler to consult. */
static int g_btn_bold_x, g_btn_italic_x, g_btn_under_x;
static int g_btn_size_x[4];           /* 8/12/16/24 */
static int g_btn_fg_x[8];
static int g_btn_bg_x[8];
static int g_btn_alignL_x, g_btn_alignC_x, g_btn_alignR_x;
static int g_btn_valignT_x, g_btn_valignC_x, g_btn_valignB_x;
static int g_btn_merge_x, g_btn_unmerge_x;
static int g_btn_save_x, g_btn_open_x;
static int g_btn_y;
#define NS_BTN_W   22
#define NS_BTN_H   24
#define NS_BTN_GAP 4

static void ns_paint_toolbar(draw_target_t *t) {
    int x = 6;
    int y = NS_MENU_H + (NS_TOOLBAR_H - NS_BTN_H) / 2;
    g_btn_y = y;
    gfx_fill_rect(t, 0, NS_MENU_H, (int)t->width, NS_TOOLBAR_H, NS_TBAR_BG);
    gfx_blend_rect(t, 0, NS_MENU_H, (int)t->width, NS_TOOLBAR_H / 2, 0x12FFFFFFu);
    gfx_draw_hline(t, 0, NS_MENU_H + NS_TOOLBAR_H - 1,
                   (int)t->width, 0xFFB0B0B8);

    ns_fmt_t *cf = ns_fmt_lookup(g_active_r, g_active_c);
    int mx = mouse_x() - g_ns_win->x - WM_BORDER;
    int my = mouse_y() - g_ns_win->y - WM_BORDER - WM_TITLE_H;

    g_btn_bold_x   = x; ns_draw_toolbar_btn(t, x, y, NS_BTN_W, NS_BTN_H, "B", cf && cf->bold,   mx, my); x += NS_BTN_W + NS_BTN_GAP;
    g_btn_italic_x = x; ns_draw_toolbar_btn(t, x, y, NS_BTN_W, NS_BTN_H, "I", cf && cf->italic, mx, my); x += NS_BTN_W + NS_BTN_GAP;
    g_btn_under_x  = x; ns_draw_toolbar_btn(t, x, y, NS_BTN_W, NS_BTN_H, "U", cf && cf->underline, mx, my); x += NS_BTN_W + NS_BTN_GAP;

    /* Font size buttons */
    x += NS_BTN_GAP;
    static const char *sz_lbl[4] = { "8", "12", "16", "24" };
    static const uint8_t sz_scale[4] = { 1, 1, 2, 3 };   /* 12 is shown but rendered as scale 1 */
    for (int i = 0; i < 4; i++) {
        g_btn_size_x[i] = x;
        uint8_t s = cf ? cf->font_scale : 0;
        bool on = (s == sz_scale[i] && (i != 0 || cf == NULL || cf->font_scale == 0 || s == 1));
        /* Treat scale 0 (default) as button "12". */
        if (cf && cf->font_scale == 0 && i == 1) on = true;
        ns_draw_toolbar_btn(t, x, y, NS_BTN_W, NS_BTN_H, sz_lbl[i], on, mx, my);
        x += NS_BTN_W + NS_BTN_GAP;
    }

    /* Foreground colour palette */
    x += NS_BTN_GAP;
    gfx_draw_string(t, x, y + 8, "Fg", NS_TBAR_FG, NS_TBAR_BG);
    x += 18;
    for (int i = 0; i < 8; i++) {
        g_btn_fg_x[i] = x;
        gfx_fill_rect(t, x, y + 2, 14, NS_BTN_H - 4, NS_FG_PALETTE[i]);
        ns_outline(t, x, y + 2, 14, NS_BTN_H - 4, 0xFF606060);
        x += 16;
    }

    /* Background colour palette */
    x += NS_BTN_GAP;
    gfx_draw_string(t, x, y + 8, "Bg", NS_TBAR_FG, NS_TBAR_BG);
    x += 18;
    for (int i = 0; i < 8; i++) {
        g_btn_bg_x[i] = x;
        if (NS_BG_PALETTE[i] == 0) {
            /* "no fill" — show a diagonal slash on white. */
            gfx_fill_rect(t, x, y + 2, 14, NS_BTN_H - 4, 0xFFFFFFFF);
            for (int k = 0; k < 14; k++)
                gfx_putpixel(t, x + k, y + 2 + (NS_BTN_H - 4) - 1 - k, 0xFFC04040);
        } else {
            gfx_fill_rect(t, x, y + 2, 14, NS_BTN_H - 4, NS_BG_PALETTE[i]);
        }
        ns_outline(t, x, y + 2, 14, NS_BTN_H - 4, 0xFF606060);
        x += 16;
    }

    /* Horizontal align */
    x += NS_BTN_GAP;
    g_btn_alignL_x = x; ns_draw_toolbar_btn(t, x, y, NS_BTN_W, NS_BTN_H, "L", cf && cf->halign == 1, mx, my); x += NS_BTN_W + NS_BTN_GAP;
    g_btn_alignC_x = x; ns_draw_toolbar_btn(t, x, y, NS_BTN_W, NS_BTN_H, "C", cf && cf->halign == 2, mx, my); x += NS_BTN_W + NS_BTN_GAP;
    g_btn_alignR_x = x; ns_draw_toolbar_btn(t, x, y, NS_BTN_W, NS_BTN_H, "R", cf && cf->halign == 3, mx, my); x += NS_BTN_W + NS_BTN_GAP;

    /* Vertical align */
    x += NS_BTN_GAP;
    g_btn_valignT_x = x; ns_draw_toolbar_btn(t, x, y, NS_BTN_W, NS_BTN_H, "T", cf && cf->valign == 1, mx, my); x += NS_BTN_W + NS_BTN_GAP;
    g_btn_valignC_x = x; ns_draw_toolbar_btn(t, x, y, NS_BTN_W, NS_BTN_H, "M", cf && cf->valign == 2, mx, my); x += NS_BTN_W + NS_BTN_GAP;
    g_btn_valignB_x = x; ns_draw_toolbar_btn(t, x, y, NS_BTN_W, NS_BTN_H, "B", cf && cf->valign == 3, mx, my); x += NS_BTN_W + NS_BTN_GAP;

    /* Merge / Unmerge */
    x += NS_BTN_GAP;
    g_btn_merge_x   = x; ns_draw_toolbar_btn(t, x, y, 56, NS_BTN_H, "Merge",   false, mx, my); x += 56 + NS_BTN_GAP;
    g_btn_unmerge_x = x; ns_draw_toolbar_btn(t, x, y, 64, NS_BTN_H, "Unmerge", false, mx, my); x += 64 + NS_BTN_GAP;

    /* File ops */
    x += NS_BTN_GAP;
    g_btn_save_x = x; ns_draw_toolbar_btn(t, x, y, 50, NS_BTN_H, "Save", false, mx, my); x += 50 + NS_BTN_GAP;
    g_btn_open_x = x; ns_draw_toolbar_btn(t, x, y, 50, NS_BTN_H, "Open", false, mx, my); x += 50 + NS_BTN_GAP;
}

/* Paint the formula bar showing the cell label + editable input. */
static void ns_paint_formula_bar(draw_target_t *t) {
    int y0 = NS_MENU_H + NS_TOOLBAR_H;
    gfx_fill_rect(t, 0, y0, (int)t->width, NS_FBAR_H, NS_FBAR_BG);
    gfx_draw_hline(t, 0, y0 + NS_FBAR_H - 1, (int)t->width, 0xFFB0B0B8);

    char ref[16];
    ns_cell_label(g_active_r, g_active_c, ref, sizeof(ref));
    gfx_draw_string(t, 8, y0 + (NS_FBAR_H - FONT_GLYPH_H) / 2,
                    ref, NS_FBAR_FG, NS_FBAR_BG);

    int input_x = 60;
    gfx_fill_rect(t, input_x, y0 + 2, (int)t->width - input_x - 8,
                  NS_FBAR_H - 4, 0xFFFFFFFF);
    ns_outline(t, input_x, y0 + 2, (int)t->width - input_x - 8,
               NS_FBAR_H - 4, 0xFFB0B0B8);

    char buf[SHEET_FORMULA_MAX];
    if (g_editing) {
        strncpy(buf, g_edit_buf, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
    } else {
        sheet_cell_t *c = sheet_at(&g_book, g_active_r, g_active_c);
        if (c && c->type == CELL_FORMULA) {
            strncpy(buf, c->formula, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
        } else {
            ns_cell_display(c, buf, sizeof(buf));
        }
    }

    int max_chars = ((int)t->width - input_x - 16) / FONT_GLYPH_W;
    if (max_chars < 0) max_chars = 0;
    int blen = (int)strlen(buf);
    int draw_off = 0;
    if (blen > max_chars) draw_off = blen - max_chars;
    gfx_draw_string(t, input_x + 4,
                    y0 + (NS_FBAR_H - FONT_GLYPH_H) / 2,
                    buf + draw_off, NS_FBAR_FG, 0xFFFFFFFF);

    if (g_editing) {
        int caret_screen = g_edit_caret - draw_off;
        if (caret_screen >= 0 && caret_screen <= max_chars) {
            int cx = input_x + 4 + caret_screen * FONT_GLYPH_W;
            gfx_fill_rect(t, cx, y0 + 5, 1, NS_FBAR_H - 10, NS_FBAR_FG);
        }
    }
}

/* Paint a single cell at its computed position. */
static void ns_paint_one_cell(draw_target_t *t, int row, int col,
                               int x, int y, int cw, int ch) {
    ns_fmt_t *f = ns_fmt_lookup(row, col);

    /* Mid-span cells of a merge are painted by the anchor — skip.
     * (used guard: a zeroed fmt slot must never read as anchored-at-A1.) */
    if (f && f->used && f->mr_anchor_row >= 0 && f->mr_anchor_col >= 0 &&
        (f->mr_anchor_row != row || f->mr_anchor_col != col)) {
        return;
    }

    /* Compute spanned cw / ch if this cell is a merge anchor. */
    int span_cols = 1, span_rows = 1;
    if (f && (f->mr_w > 1 || f->mr_h > 1)) {
        span_cols = f->mr_w;
        span_rows = f->mr_h;
        for (int c2 = 1; c2 < span_cols; c2++) cw += ns_col_width(col + c2);
        for (int r2 = 1; r2 < span_rows; r2++) ch += ns_row_height(row + r2);
    }

    /* Background fill. */
    uint32_t bg = (f && f->bg_color != 0) ? f->bg_color : NS_BG;
    gfx_fill_rect(t, x, y, cw, ch, bg);

    /* Content. */
    sheet_cell_t *c = sheet_at(&g_book, row, col);
    if (c && c->type != CELL_EMPTY) {
        char disp[SHEET_STR_MAX + 8];
        ns_cell_display(c, disp, sizeof(disp));

        uint32_t fg = (f && f->fg_color != 0) ? f->fg_color : NS_CELL_FG_DEFAULT;
        int scale = (f && f->font_scale) ? f->font_scale : 1;
        if (scale < 1) scale = 1;
        if (scale > 4) scale = 4;

        int adv = (scale == 1) ? FONT_GLYPH_W : gfx_ttf_advance(scale);
        int glyph_h = FONT_GLYPH_H * scale;

        /* Horizontal alignment.  Numbers default-right, strings default-left. */
        int halign = (f && f->halign) ? f->halign : 0;
        if (halign == 0) {
            halign = (c->type == CELL_NUMBER || c->type == CELL_FORMULA) ? 3 : 1;
        }
        int valign = (f && f->valign) ? f->valign : 0;
        if (valign == 0) valign = 2;     /* middle by default */

        int text_w = (int)strlen(disp) * adv;
        int tx;
        if      (halign == 2) tx = x + (cw - text_w) / 2;
        else if (halign == 3) tx = x + cw - text_w - 4;
        else                  tx = x + 4;

        int ty;
        if      (valign == 1) ty = y + 2;
        else if (valign == 3) ty = y + ch - glyph_h - 2;
        else                  ty = y + (ch - glyph_h) / 2;

        if (tx < x + 2) tx = x + 2;

        bool italic = (f && f->italic);

        /* Clip horizontally by drawing only chars that fit.  Italic text
         * (scale 1) renders sheared over the already-filled cell background. */
        if (scale == 1) {
            if (italic)
                gfx_draw_string_italic_clipped(t, tx, ty, cw - 6, disp, fg);
            else
                gfx_draw_string_clipped(t, tx, ty, cw - 6, disp, fg, bg);
        } else {
            gfx_draw_string_ttf(t, tx, ty, disp, scale, fg, bg);
        }

        /* Bold = redraw shifted by 1px to the right (cheap synthetic bold). */
        if (f && f->bold) {
            if (scale == 1) {
                if (italic)
                    gfx_draw_string_italic_clipped(t, tx + 1, ty, cw - 7, disp, fg);
                else
                    gfx_draw_string_clipped(t, tx + 1, ty, cw - 7, disp, fg, bg);
            } else {
                gfx_draw_string_ttf(t, tx + 1, ty, disp, scale, fg, bg);
            }
        }

        /* Underline */
        if (f && f->underline) {
            int uy = ty + glyph_h + 1;
            int uw = text_w; if (uw > cw - 6) uw = cw - 6;
            gfx_fill_rect(t, tx, uy, uw, 1, fg);
        }
    }
}

static void ns_paint_grid(draw_target_t *t) {
    int grid_top = ns_grid_top(g_ns_win);

    /* Column header bar */
    gfx_fill_rect(t, NS_HEADER_W, grid_top,
                  (int)t->width - NS_HEADER_W, NS_HEADER_H, NS_HDR_BG);
    int x = NS_HEADER_W;
    for (int c = g_scroll_col; c < NS_MAX_VIEW_COLS; c++) {
        int cw = ns_col_width(c);
        if (x >= (int)t->width) break;
        uint32_t bg = (c >= g_sel_c0 && c <= g_sel_c1) ? NS_HDR_SEL_BG : NS_HDR_BG;
        uint32_t fg = (c >= g_sel_c0 && c <= g_sel_c1) ? NS_HDR_SEL_FG : NS_HDR_FG;
        gfx_fill_rect(t, x, grid_top, cw, NS_HEADER_H, bg);
        char lbl[8];
        ns_col_label(c, lbl, sizeof(lbl));
        int tx = x + (cw - (int)strlen(lbl) * FONT_GLYPH_W) / 2;
        gfx_draw_string(t, tx, grid_top + (NS_HEADER_H - FONT_GLYPH_H) / 2,
                        lbl, fg, bg);
        gfx_draw_vline(t, x + cw - 1, grid_top, NS_HEADER_H, NS_GRID);
        x += cw;
    }
    gfx_draw_hline(t, NS_HEADER_W, grid_top + NS_HEADER_H - 1,
                   (int)t->width - NS_HEADER_W, NS_GRID);

    /* Row header bar */
    gfx_fill_rect(t, 0, grid_top, NS_HEADER_W, NS_HEADER_H, NS_HDR_BG);

    int y = grid_top + NS_HEADER_H;
    for (int r = g_scroll_row; r < NS_MAX_VIEW_ROWS; r++) {
        int rh = ns_row_height(r);
        if (y >= ns_grid_bottom(g_ns_win)) break;
        uint32_t bg = (r >= g_sel_r0 && r <= g_sel_r1) ? NS_HDR_SEL_BG : NS_HDR_BG;
        uint32_t fg = (r >= g_sel_r0 && r <= g_sel_r1) ? NS_HDR_SEL_FG : NS_HDR_FG;
        gfx_fill_rect(t, 0, y, NS_HEADER_W, rh, bg);
        char lbl[8];
        ksnprintf(lbl, sizeof(lbl), "%d", r + 1);
        gfx_draw_string(t, 6, y + (rh - FONT_GLYPH_H) / 2, lbl, fg, bg);
        gfx_draw_hline(t, 0, y + rh - 1, NS_HEADER_W, NS_GRID);
        y += rh;
    }
    gfx_draw_vline(t, NS_HEADER_W - 1, grid_top, ns_grid_bottom(g_ns_win) - grid_top, NS_GRID);

    /* Cells + grid lines */
    int yy = grid_top + NS_HEADER_H;
    for (int r = g_scroll_row; r < NS_MAX_VIEW_ROWS; r++) {
        int rh = ns_row_height(r);
        if (yy >= ns_grid_bottom(g_ns_win)) break;
        int xx = NS_HEADER_W;
        for (int c = g_scroll_col; c < NS_MAX_VIEW_COLS; c++) {
            int cw = ns_col_width(c);
            if (xx >= (int)t->width) break;

            ns_paint_one_cell(t, r, c, xx, yy, cw, rh);

            /* grid lines */
            gfx_draw_vline(t, xx + cw - 1, yy, rh, NS_GRID);
            gfx_draw_hline(t, xx,          yy + rh - 1, cw, NS_GRID);
            xx += cw;
        }
        yy += rh;
    }

    /* Selection range semi-transparent fill (only when range > 1 cell). */
    if (g_sel_r0 != g_sel_r1 || g_sel_c0 != g_sel_c1) {
        int ssx = ns_col_x(g_sel_c0);
        int ssy = ns_row_y(g_ns_win, g_sel_r0);
        int sex = ns_col_x(g_sel_c1);
        int sey = ns_row_y(g_ns_win, g_sel_r1);
        int ssw = sex + ns_col_width(g_sel_c1) - ssx;
        int ssh = sey + ns_row_height(g_sel_r1) - ssy;
        if (ssx >= NS_HEADER_W && ssy >= grid_top + NS_HEADER_H)
            gfx_blend_rect(t, ssx, ssy, ssw, ssh, NS_CELL_SEL);
    }

    /* Active cell highlight (light-blue fill so the cursor is always obvious). */
    {
        int ax  = ns_col_x(g_active_c);
        int ay  = ns_row_y(g_ns_win, g_active_r);
        int acw = ns_col_width(g_active_c);
        int ach = ns_row_height(g_active_r);
        if (ax >= NS_HEADER_W && ay >= grid_top + NS_HEADER_H)
            gfx_blend_rect(t, ax, ay, acw, ach, 0x3C1860C8u);
    }

    /* Selection outline (double-width blue border). */
    int sx = ns_col_x(g_sel_c0);
    int sy = ns_row_y(g_ns_win, g_sel_r0);
    int ex = ns_col_x(g_sel_c1);
    int ey = ns_row_y(g_ns_win, g_sel_r1);
    int ew = ns_col_width(g_sel_c1);
    int eh = ns_row_height(g_sel_r1);
    int rx = sx, ry = sy, rw = (ex + ew) - sx, rh = (ey + eh) - sy;
    if (rx >= NS_HEADER_W && ry >= grid_top + NS_HEADER_H) {
        ns_outline(t, rx,     ry,     rw,     rh,     NS_RANGE_SEL_OUTLINE);
        ns_outline(t, rx + 1, ry + 1, rw - 2, rh - 2, NS_RANGE_SEL_OUTLINE);
    }
}

static void ns_paint_status(draw_target_t *t) {
    int y0 = (int)t->height - NS_STATUS_H;
    gfx_fill_rect(t, 0, y0, (int)t->width, NS_STATUS_H, NS_STATUS_BG);
    gfx_draw_hline(t, 0, y0, (int)t->width, 0xFFB0B0B8);
    char line[160];
    ksnprintf(line, sizeof(line), "%s%s  |  %s",
              g_filename[0] ? g_filename : "(untitled)",
              g_editing ? " *" : "",
              g_ns_status);
    gfx_draw_string_clipped(t, 8, y0 + (NS_STATUS_H - FONT_GLYPH_H) / 2,
                            (int)t->width - 16, line, NS_STATUS_FG, NS_STATUS_BG);
}

static void ns_redraw(void) {
    if (!g_ns_win || !g_ns_win->in_use) return;
    draw_target_t *t = &g_ns_win->content;
    gfx_clear(t, NS_BG);
    ns_paint_menu(t);
    ns_paint_toolbar(t);
    ns_paint_formula_bar(t);
    ns_paint_grid(t);
    ns_paint_status(t);
    wm_mark_dirty();
}

/* ====================================================================
 *                Commit / cancel edit
 * ==================================================================== */
static void ns_begin_edit(bool clear) {
    g_editing = true;
    if (clear) {
        g_edit_buf[0] = 0;
        g_edit_len = 0;
        g_edit_caret = 0;
    } else {
        sheet_cell_t *c = sheet_at(&g_book, g_active_r, g_active_c);
        if (c && c->type == CELL_FORMULA) {
            strncpy(g_edit_buf, c->formula, sizeof(g_edit_buf) - 1);
        } else if (c) {
            ns_cell_display(c, g_edit_buf, sizeof(g_edit_buf));
        } else {
            g_edit_buf[0] = 0;
        }
        g_edit_buf[sizeof(g_edit_buf) - 1] = 0;
        g_edit_len   = (int)strlen(g_edit_buf);
        g_edit_caret = g_edit_len;
    }
}

static void ns_commit_edit(void) {
    if (!g_editing) return;
    ns_apply_input(g_active_r, g_active_c, g_edit_buf);
    sheet_recalc(&g_book);
    g_editing = false;
}

static void ns_cancel_edit(void) {
    g_editing = false;
}

/* ====================================================================
 *                Toolbar actions
 * ==================================================================== */

static void ns_for_selection(void (*fn)(ns_fmt_t *f)) {
    for (int r = g_sel_r0; r <= g_sel_r1; r++) {
        for (int c = g_sel_c0; c <= g_sel_c1; c++) {
            ns_fmt_t *f = ns_fmt_get_create(r, c);
            if (f) fn(f);
        }
    }
}

/* Per-attribute mutators used by ns_for_selection. */
static int g_apply_size;
static uint32_t g_apply_fg, g_apply_bg;
static int g_apply_halign, g_apply_valign;
static void ns_mut_bold     (ns_fmt_t *f) { f->bold ^= 1; }
static void ns_mut_italic   (ns_fmt_t *f) { f->italic ^= 1; }
static void ns_mut_under    (ns_fmt_t *f) { f->underline ^= 1; }
static void ns_mut_size     (ns_fmt_t *f) { f->font_scale = (uint8_t)g_apply_size; }
static void ns_mut_fg       (ns_fmt_t *f) { f->fg_color = g_apply_fg; }
static void ns_mut_bg       (ns_fmt_t *f) { f->bg_color = g_apply_bg; }
static void ns_mut_halign   (ns_fmt_t *f) { f->halign = (uint8_t)g_apply_halign; }
static void ns_mut_valign   (ns_fmt_t *f) { f->valign = (uint8_t)g_apply_valign; }

static void ns_action_merge(void) {
    if (g_sel_r0 == g_sel_r1 && g_sel_c0 == g_sel_c1) {
        ns_set_status("Merge: select more than one cell first");
        return;
    }
    ns_fmt_t *anchor = ns_fmt_get_create(g_sel_r0, g_sel_c0);
    if (!anchor) { ns_set_status("Merge: cell table full"); return; }
    anchor->mr_h = (int16_t)(g_sel_r1 - g_sel_r0 + 1);
    anchor->mr_w = (int16_t)(g_sel_c1 - g_sel_c0 + 1);
    anchor->mr_anchor_row = (int16_t)g_sel_r0;
    anchor->mr_anchor_col = (int16_t)g_sel_c0;
    /* Mark every spanned cell. */
    for (int r = g_sel_r0; r <= g_sel_r1; r++) {
        for (int c = g_sel_c0; c <= g_sel_c1; c++) {
            if (r == g_sel_r0 && c == g_sel_c0) continue;
            ns_fmt_t *f = ns_fmt_get_create(r, c);
            if (!f) continue;
            f->mr_h = 0;
            f->mr_w = 0;
            f->mr_anchor_row = (int16_t)g_sel_r0;
            f->mr_anchor_col = (int16_t)g_sel_c0;
        }
    }
    ns_set_status("Merged %dx%d cells",
                  g_sel_c1 - g_sel_c0 + 1, g_sel_r1 - g_sel_r0 + 1);
}

static void ns_action_unmerge(void) {
    ns_fmt_t *anchor = ns_fmt_lookup(g_active_r, g_active_c);
    if (!anchor || !anchor->used || (anchor->mr_w <= 1 && anchor->mr_h <= 1)) {
        /* maybe we're inside a merge — find anchor. */
        if (anchor && anchor->mr_anchor_row >= 0) {
            int ar = anchor->mr_anchor_row, ac = anchor->mr_anchor_col;
            anchor = ns_fmt_lookup(ar, ac);
        }
    }
    if (!anchor || (anchor->mr_w <= 1 && anchor->mr_h <= 1)) {
        ns_set_status("Unmerge: not a merged cell");
        return;
    }
    int rs = anchor->mr_anchor_row, cs = anchor->mr_anchor_col;
    int rh = anchor->mr_h, rw = anchor->mr_w;
    for (int r = rs; r < rs + rh; r++) {
        for (int c = cs; c < cs + rw; c++) {
            ns_fmt_t *f = ns_fmt_lookup(r, c);
            if (!f) continue;
            f->mr_h = 1; f->mr_w = 1;
            f->mr_anchor_row = -1;
            f->mr_anchor_col = -1;
        }
    }
    ns_set_status("Unmerged");
}

static void ns_action_save(void) {
    char prompt[64];
    ksnprintf(prompt, sizeof(prompt), "Filename (.xlsx or .csv):");
    char name[NXFS_NAME_MAX];
    if (!dialog_input("NexSheet — Save", prompt,
                      g_filename[0] ? g_filename : "sheet.csv",
                      name, sizeof(name))) {
        return;
    }
    ns_save_to_name(name);
}

static void ns_action_open(void) {
    char name[NXFS_NAME_MAX];
    if (!dialog_input("NexSheet — Open", "Filename:",
                      g_filename[0] ? g_filename : "sheet.csv",
                      name, sizeof(name))) {
        return;
    }
    ns_open_from_name(name);
}

/* ====================================================================
 *                Mouse + Keyboard handlers
 * ==================================================================== */

static bool ns_click(window_t *w, int cx, int cy, uint8_t pressed, uint8_t btn);
static bool ns_key  (window_t *w, int k);
static void ns_destroy_cb(window_t *w);
static void ns_resize_cb (window_t *w);

/* ---- Dropdown menu action dispatch ----------------------------------- */
static void ns_dispatch_menu_action(int menu, int item) {
    switch (menu) {
    case 0: /* File */
        switch (item) {
        case 0: /* New Sheet */
            sheet_init(&g_book);
            memset(g_fmt, 0, sizeof(g_fmt));
            for (int i = 0; i < NS_MAX_VIEW_COLS; i++) g_col_w[i] = 0;
            for (int i = 0; i < NS_MAX_VIEW_ROWS; i++) g_row_h[i] = 0;
            g_filename[0] = 0;
            g_active_r = g_active_c = 0;
            g_sel_r0 = g_sel_r1 = g_sel_c0 = g_sel_c1 = 0;
            g_scroll_row = g_scroll_col = 0;
            ns_set_status("New sheet created");
            break;
        case 1: /* Open */ ns_action_open();  break;
        case 2: /* Save */ ns_action_save();  break;
        case 3: /* Save As */
            g_filename[0] = 0;
            ns_action_save();
            break;
        case 4: /* Close */
            if (g_ns_win) wm_destroy_window(g_ns_win);
            break;
        }
        break;
    case 1: /* Edit */
        switch (item) {
        case 0: /* Select All */
            g_sel_r0 = 0; g_sel_c0 = 0;
            g_sel_r1 = NS_MAX_VIEW_ROWS - 1;
            g_sel_c1 = NS_MAX_VIEW_COLS - 1;
            ns_set_status("All cells selected");
            break;
        case 1: /* Delete */
            for (int r = g_sel_r0; r <= g_sel_r1; r++) {
                for (int c = g_sel_c0; c <= g_sel_c1; c++) {
                    sheet_cell_t *sc = sheet_at(&g_book, r, c);
                    if (sc) { sc->type = CELL_EMPTY; sc->value = 0;
                              sc->text[0] = 0; sc->formula[0] = 0; }
                }
            }
            ns_set_status("Deleted content of %d cells",
                          (g_sel_r1 - g_sel_r0 + 1) * (g_sel_c1 - g_sel_c0 + 1));
            break;
        }
        break;
    case 2: /* Format */
        switch (item) {
        case 0: ns_for_selection(ns_mut_bold);    break;
        case 1: ns_for_selection(ns_mut_italic);  break;
        case 2: ns_for_selection(ns_mut_under);   break;
        case 3: ns_action_merge();                break;
        case 4: ns_action_unmerge();              break;
        }
        break;
    case 3: /* View */
        if (item == 0) { /* Go to A1 */
            g_scroll_row = 0; g_scroll_col = 0;
            g_active_r = 0;   g_active_c = 0;
            g_sel_r0 = g_sel_r1 = g_sel_c0 = g_sel_c1 = 0;
            ns_set_status("Jumped to A1");
        }
        break;
    case 4: /* Help */
        if (item == 0) {
            const char *lines[] = {
                "NexSheet v1.0  —  NexxoN OS Spreadsheet",
                "Supports CSV and XLSX file formats.",
                "Shift/Ctrl+click or drag to select a range.",
                "Toolbar: Bold / Italic / Underline / Merge.",
                "Formulas start with =  (e.g. =SUM(A1:A10))",
            };
            dialog_info("About NexSheet", lines, 5);
        }
        break;
    }
}

/* ---- Mouse ----------------------------------------------------------- */
static bool ns_handle_toolbar_click(int cx, int cy) {
    if (cy < NS_MENU_H || cy >= NS_MENU_H + NS_TOOLBAR_H) return false;

    /* Helper: hit a NS_BTN_W-wide button starting at x. */
#define HIT_BTN(X)  (cx >= (X) && cx < (X) + NS_BTN_W && cy >= g_btn_y && cy < g_btn_y + NS_BTN_H)
#define HIT_WIDE(X, W)  (cx >= (X) && cx < (X) + (W) && cy >= g_btn_y && cy < g_btn_y + NS_BTN_H)

    if (HIT_BTN(g_btn_bold_x))   { ns_for_selection(ns_mut_bold);   return true; }
    if (HIT_BTN(g_btn_italic_x)) { ns_for_selection(ns_mut_italic); return true; }
    if (HIT_BTN(g_btn_under_x))  { ns_for_selection(ns_mut_under);  return true; }
    static const uint8_t sz_scale[4] = { 1, 1, 2, 3 };
    for (int i = 0; i < 4; i++) {
        if (HIT_BTN(g_btn_size_x[i])) {
            g_apply_size = sz_scale[i];
            ns_for_selection(ns_mut_size);
            return true;
        }
    }
    /* Foreground swatches */
    for (int i = 0; i < 8; i++) {
        if (cx >= g_btn_fg_x[i] && cx < g_btn_fg_x[i] + 14 &&
            cy >= g_btn_y + 2 && cy < g_btn_y + 2 + NS_BTN_H - 4) {
            g_apply_fg = NS_FG_PALETTE[i];
            ns_for_selection(ns_mut_fg);
            return true;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (cx >= g_btn_bg_x[i] && cx < g_btn_bg_x[i] + 14 &&
            cy >= g_btn_y + 2 && cy < g_btn_y + 2 + NS_BTN_H - 4) {
            g_apply_bg = NS_BG_PALETTE[i];
            ns_for_selection(ns_mut_bg);
            return true;
        }
    }
    if (HIT_BTN(g_btn_alignL_x)) { g_apply_halign = 1; ns_for_selection(ns_mut_halign); return true; }
    if (HIT_BTN(g_btn_alignC_x)) { g_apply_halign = 2; ns_for_selection(ns_mut_halign); return true; }
    if (HIT_BTN(g_btn_alignR_x)) { g_apply_halign = 3; ns_for_selection(ns_mut_halign); return true; }
    if (HIT_BTN(g_btn_valignT_x)){ g_apply_valign = 1; ns_for_selection(ns_mut_valign); return true; }
    if (HIT_BTN(g_btn_valignC_x)){ g_apply_valign = 2; ns_for_selection(ns_mut_valign); return true; }
    if (HIT_BTN(g_btn_valignB_x)){ g_apply_valign = 3; ns_for_selection(ns_mut_valign); return true; }
    if (HIT_WIDE(g_btn_merge_x,   56)) { ns_action_merge();   return true; }
    if (HIT_WIDE(g_btn_unmerge_x, 64)) { ns_action_unmerge(); return true; }
    if (HIT_WIDE(g_btn_save_x,    50)) { ns_action_save();    return true; }
    if (HIT_WIDE(g_btn_open_x,    50)) { ns_action_open();    return true; }

#undef HIT_BTN
#undef HIT_WIDE
    return true;   /* swallow click inside toolbar even if no button hit */
}

/* Returns true if we started a column- or row-resize drag, false otherwise. */
static bool ns_check_edge_resize(window_t *w, int cx, int cy) {
    int grid_top = ns_grid_top(w);
    /* Column resize: cursor near a column header right edge. */
    if (cy >= grid_top && cy < grid_top + NS_HEADER_H && cx >= NS_HEADER_W) {
        int x = NS_HEADER_W;
        for (int c = g_scroll_col; c < NS_MAX_VIEW_COLS; c++) {
            int cw = ns_col_width(c);
            int right = x + cw - 1;
            if (cx >= right - 2 && cx <= right + 2) {
                g_resize_col = c;
                g_resize_start_x = cx;
                g_resize_start_w = cw;
                ns_set_status("Drag to resize column %c", 'A' + c);
                return true;
            }
            x += cw;
            if (x >= (int)w->content.width) break;
        }
    }
    /* Row resize: cursor near a row header bottom edge. */
    if (cx >= 0 && cx < NS_HEADER_W && cy >= grid_top + NS_HEADER_H) {
        int y = grid_top + NS_HEADER_H;
        for (int r = g_scroll_row; r < NS_MAX_VIEW_ROWS; r++) {
            int rh = ns_row_height(r);
            int bot = y + rh - 1;
            if (cy >= bot - 2 && cy <= bot + 2) {
                g_resize_row = r;
                g_resize_start_y = cy;
                g_resize_start_h = rh;
                ns_set_status("Drag to resize row %d", r + 1);
                return true;
            }
            y += rh;
            if (y >= ns_grid_bottom(w)) break;
        }
    }
    return false;
}

static bool ns_click(window_t *w, int cx, int cy, uint8_t pressed, uint8_t btn) {
    (void)btn;
    if (w != g_ns_win) return false;

    /* Button-up: end resize drag and drag-selection. */
    if (!(pressed & MOUSE_BTN_LEFT)) {
        if (g_resize_col >= 0 || g_resize_row >= 0) {
            g_resize_col = -1;
            g_resize_row = -1;
            ns_redraw();
        }
        if (g_ns_drag_sel) {
            g_ns_drag_sel = false;
            ns_redraw();
        }
        return true;
    }

    /* Commit any in-progress edit. */
    if (g_editing) ns_commit_edit();

    /* ---- Menu bar click ----------------------------------------------- */
    if (cy < NS_MENU_H) {
        int hit = -1;
        for (int i = 0; i < 5; i++) {
            if (cx >= g_ns_menu_item_x[i] &&
                cx < g_ns_menu_item_x[i] + g_ns_menu_item_w[i]) {
                hit = i; break;
            }
        }
        g_ns_menu_open = (hit == g_ns_menu_open) ? -1 : hit;
        g_ns_menu_hot  = -1;
        ns_redraw();
        return true;
    }

    /* ---- Open dropdown click ------------------------------------------ */
    if (g_ns_menu_open >= 0) {
        const ns_mi_t *items = g_ns_ddrop[g_ns_menu_open];
        int dx = g_ns_menu_item_x[g_ns_menu_open];
        if (dx + NS_DDROP_W > (int)w->content.width)
            dx = (int)w->content.width - NS_DDROP_W;
        int dy = NS_MENU_H;
        int dh = 4;
        for (int i = 0; items[i].lbl || items[i].sep; i++)
            dh += items[i].sep ? 5 : NS_DDROP_ITEM_H;

        if (cx >= dx && cx < dx + NS_DDROP_W && cy >= dy && cy < dy + dh) {
            int iy = dy + 2, idx = 0;
            for (int i = 0; items[i].lbl || items[i].sep; i++) {
                if (items[i].sep) { iy += 5; continue; }
                if (cy >= iy && cy < iy + NS_DDROP_ITEM_H) {
                    int save_menu = g_ns_menu_open;
                    g_ns_menu_open = -1;
                    g_ns_menu_hot  = -1;
                    ns_dispatch_menu_action(save_menu, idx);
                    ns_redraw();
                    return true;
                }
                iy += NS_DDROP_ITEM_H;
                idx++;
            }
        }
        g_ns_menu_open = -1;
        g_ns_menu_hot  = -1;
        ns_redraw();
        return true;
    }

    /* ---- Toolbar ------------------------------------------------------ */
    if (cy < NS_MENU_H + NS_TOOLBAR_H) {
        ns_handle_toolbar_click(cx, cy);
        ns_redraw();
        return true;
    }

    /* ---- Formula bar -------------------------------------------------- */
    if (cy < NS_MENU_H + NS_TOOLBAR_H + NS_FBAR_H && cx >= 60) {
        ns_begin_edit(false);
        ns_redraw();
        return true;
    }

    /* ---- Column / row edge-resize ------------------------------------ */
    if (ns_check_edge_resize(w, cx, cy)) {
        ns_redraw();
        return true;
    }

    /* ---- Grid cell click --------------------------------------------- */
    int row = -1, col = -1;
    ns_hit_cell(w, cx, cy, &row, &col);
    if (row < 0 || col < 0) return true;

    /* Resolve mid-merge to anchor (used guard: see ns_fmt_lookup). */
    ns_fmt_t *f = ns_fmt_lookup(row, col);
    if (f && f->used && f->mr_anchor_row >= 0 && f->mr_anchor_col >= 0) {
        row = f->mr_anchor_row;
        col = f->mr_anchor_col;
    }

    bool shift_held = keyboard_shift_held();
    bool ctrl_held  = keyboard_ctrl_held();
    if (shift_held || ctrl_held) {
        /* Extend selection to include the clicked cell. */
        if (row < g_sel_r0) g_sel_r0 = row;
        if (row > g_sel_r1) g_sel_r1 = row;
        if (col < g_sel_c0) g_sel_c0 = col;
        if (col > g_sel_c1) g_sel_c1 = col;
        g_active_r = row;
        g_active_c = col;
    } else {
        g_active_r = row; g_active_c = col;
        g_sel_r0 = row;   g_sel_r1 = row;
        g_sel_c0 = col;   g_sel_c1 = col;
        /* Begin drag selection. */
        g_ns_drag_sel = true;
        g_ns_drag_r0  = row;
        g_ns_drag_c0  = col;
    }
    ns_redraw();
    return true;
}

/* ---- Keyboard -------------------------------------------------------- */
static void ns_move(int dr, int dc) {
    int nr = g_active_r + dr;
    int nc = g_active_c + dc;
    if (nr < 0) nr = 0;
    if (nc < 0) nc = 0;
    if (nr >= NS_MAX_VIEW_ROWS) nr = NS_MAX_VIEW_ROWS - 1;
    if (nc >= NS_MAX_VIEW_COLS) nc = NS_MAX_VIEW_COLS - 1;
    g_active_r = nr;
    g_active_c = nc;
    g_sel_r0 = g_sel_r1 = nr;
    g_sel_c0 = g_sel_c1 = nc;
    /* Keep view scrolled to the active cell. */
    if (g_active_c < g_scroll_col) g_scroll_col = g_active_c;
    if (g_active_r < g_scroll_row) g_scroll_row = g_active_r;
    int vis_cols = ((int)g_ns_win->content.width - NS_HEADER_W) / NS_DEFAULT_COL_W;
    int vis_rows = (ns_grid_bottom(g_ns_win) - ns_grid_top(g_ns_win) - NS_HEADER_H)
                   / NS_DEFAULT_ROW_H;
    if (vis_cols < 1) vis_cols = 1;
    if (vis_rows < 1) vis_rows = 1;
    if (g_active_c >= g_scroll_col + vis_cols) g_scroll_col = g_active_c - vis_cols + 1;
    if (g_active_r >= g_scroll_row + vis_rows) g_scroll_row = g_active_r - vis_rows + 1;
}

static bool ns_key(window_t *w, int k) {
    if (w != g_ns_win || !w->in_use) return false;
    if (k == 0) return true;

    /* Edit mode: most keys feed the formula-bar buffer. */
    if (g_editing) {
        if (k == '\n' || k == '\r') {
            ns_commit_edit();
            ns_move(1, 0);
            ns_redraw();
            return true;
        }
        if (k == KEY_ESCAPE) {
            ns_cancel_edit();
            ns_redraw();
            return true;
        }
        if (k == '\b') {
            if (g_edit_caret > 0 && g_edit_len > 0) {
                memmove(g_edit_buf + g_edit_caret - 1,
                        g_edit_buf + g_edit_caret,
                        (size_t)(g_edit_len - g_edit_caret + 1));
                g_edit_caret--;
                g_edit_len--;
            }
            ns_redraw();
            return true;
        }
        if (k == KEY_DEL) {
            if (g_edit_caret < g_edit_len) {
                memmove(g_edit_buf + g_edit_caret,
                        g_edit_buf + g_edit_caret + 1,
                        (size_t)(g_edit_len - g_edit_caret));
                g_edit_len--;
            }
            ns_redraw();
            return true;
        }
        if (k == KEY_LEFT)  { if (g_edit_caret > 0) g_edit_caret--; ns_redraw(); return true; }
        if (k == KEY_RIGHT) { if (g_edit_caret < g_edit_len) g_edit_caret++; ns_redraw(); return true; }
        if (k == KEY_HOME)  { g_edit_caret = 0; ns_redraw(); return true; }
        if (k == KEY_END)   { g_edit_caret = g_edit_len; ns_redraw(); return true; }
        if (k >= 0x20 && k < 0x7F &&
            g_edit_len + 1 < (int)sizeof(g_edit_buf)) {
            memmove(g_edit_buf + g_edit_caret + 1,
                    g_edit_buf + g_edit_caret,
                    (size_t)(g_edit_len - g_edit_caret + 1));
            g_edit_buf[g_edit_caret] = (char)k;
            g_edit_caret++;
            g_edit_len++;
            ns_redraw();
            return true;
        }
        return true;
    }

    /* Navigation mode */
    /* Close open dropdown on any non-menu key. */
    if (g_ns_menu_open >= 0) {
        g_ns_menu_open = -1;
        g_ns_menu_hot  = -1;
    }
    /* Shift+Arrow extends the selection range instead of moving focus. */
    if (keyboard_shift_held()) {
        if (k == KEY_UP) {
            if (g_sel_r0 > 0) g_sel_r0--;
            ns_redraw(); return true;
        }
        if (k == KEY_DOWN) {
            if (g_sel_r1 < NS_MAX_VIEW_ROWS - 1) g_sel_r1++;
            ns_redraw(); return true;
        }
        if (k == KEY_LEFT) {
            if (g_sel_c0 > 0) g_sel_c0--;
            ns_redraw(); return true;
        }
        if (k == KEY_RIGHT) {
            if (g_sel_c1 < NS_MAX_VIEW_COLS - 1) g_sel_c1++;
            ns_redraw(); return true;
        }
    }
    if (k == KEY_UP)    { ns_move(-1,  0); ns_redraw(); return true; }
    if (k == KEY_DOWN)  { ns_move( 1,  0); ns_redraw(); return true; }
    if (k == KEY_LEFT)  { ns_move( 0, -1); ns_redraw(); return true; }
    if (k == KEY_RIGHT) { ns_move( 0,  1); ns_redraw(); return true; }
    if (k == '\t')      { ns_move( 0,  1); ns_redraw(); return true; }
    if (k == '\n' || k == '\r') {
        /* Enter on navigation: begin edit of the current cell. */
        ns_begin_edit(false);
        ns_redraw();
        return true;
    }
    if (k == KEY_F2) { ns_begin_edit(false); ns_redraw(); return true; }
    if (k == KEY_DEL || k == '\b') {
        sheet_cell_t *c = sheet_at(&g_book, g_active_r, g_active_c);
        if (c) { c->type = CELL_EMPTY; c->value = 0;
                 c->text[0] = 0; c->formula[0] = 0; }
        ns_redraw();
        return true;
    }
    if (k == KEY_ESCAPE) {
        /* Quit/cancel.  Confirm via small dialog later — for now do
         * nothing so the user can use Esc to clear status. */
        ns_set_status("Ready");
        ns_redraw();
        return true;
    }
    /* Start editing immediately on first printable keystroke. */
    if (k >= 0x20 && k < 0x7F) {
        ns_begin_edit(true);
        g_edit_buf[0] = (char)k;
        g_edit_buf[1] = 0;
        g_edit_len   = 1;
        g_edit_caret = 1;
        ns_redraw();
        return true;
    }
    return true;
}

static void ns_destroy_cb(window_t *w) {
    if (w == g_ns_win) {
        g_ns_win = NULL;
    }
}

static void ns_resize_cb(window_t *w) {
    (void)w;
    ns_redraw();
}

/* Ticker: resize drags, drag selection, dropdown hover. */
void nexsheet_tick(void) {
    if (!g_ns_win || !g_ns_win->in_use) return;
    static int last_mx = -1, last_my = -1;
    int mx = mouse_x() - g_ns_win->x - WM_BORDER;
    int my = mouse_y() - g_ns_win->y - WM_BORDER - WM_TITLE_H;
    bool btn_down = (mouse_btn() & MOUSE_BTN_LEFT) != 0;

    /* Column-resize drag. */
    if (g_resize_col >= 0) {
        int new_w = g_resize_start_w + (mx - g_resize_start_x);
        if (new_w < NS_MIN_COL_W) new_w = NS_MIN_COL_W;
        if (new_w > 800) new_w = 800;
        if (g_col_w[g_resize_col] != new_w) {
            g_col_w[g_resize_col] = (uint16_t)new_w;
            ns_redraw();
        }
    } else if (g_resize_row >= 0) {
        int new_h = g_resize_start_h + (my - g_resize_start_y);
        if (new_h < NS_MIN_ROW_H) new_h = NS_MIN_ROW_H;
        if (new_h > 400) new_h = 400;
        if (g_row_h[g_resize_row] != new_h) {
            g_row_h[g_resize_row] = (uint16_t)new_h;
            ns_redraw();
        }
    }

    /* Drag selection: extend sel_r1/c1 to cell under mouse. */
    if (g_ns_drag_sel) {
        if (!btn_down) {
            g_ns_drag_sel = false;
            ns_redraw();
        } else if (mx != last_mx || my != last_my) {
            int row = -1, col = -1;
            ns_hit_cell(g_ns_win, mx, my, &row, &col);
            if (row >= 0 && col >= 0) {
                int r0 = g_ns_drag_r0 < row ? g_ns_drag_r0 : row;
                int r1 = g_ns_drag_r0 > row ? g_ns_drag_r0 : row;
                int c0 = g_ns_drag_c0 < col ? g_ns_drag_c0 : col;
                int c1 = g_ns_drag_c0 > col ? g_ns_drag_c0 : col;
                if (r0 != g_sel_r0 || r1 != g_sel_r1 ||
                    c0 != g_sel_c0 || c1 != g_sel_c1) {
                    g_sel_r0 = r0; g_sel_r1 = r1;
                    g_sel_c0 = c0; g_sel_c1 = c1;
                    ns_redraw();
                }
            }
        }
    }

    /* Dropdown hover highlight + menu-bar cursor-follows-mouse. */
    if (g_ns_menu_open >= 0) {
        const ns_mi_t *items = g_ns_ddrop[g_ns_menu_open];
        int dx = g_ns_menu_item_x[g_ns_menu_open];
        if (g_ns_win && dx + NS_DDROP_W > (int)g_ns_win->content.width)
            dx = (int)g_ns_win->content.width - NS_DDROP_W;
        int dy = NS_MENU_H;
        int new_hot = -1;
        int iy = dy + 2, idx = 0;
        for (int i = 0; items[i].lbl || items[i].sep; i++) {
            if (items[i].sep) { iy += 5; continue; }
            if (mx >= dx && mx < dx + NS_DDROP_W &&
                my >= iy && my < iy + NS_DDROP_ITEM_H) {
                new_hot = idx; break;
            }
            iy += NS_DDROP_ITEM_H;
            idx++;
        }
        if (new_hot != g_ns_menu_hot) {
            g_ns_menu_hot = new_hot;
            ns_redraw();
        }
        /* Slide to adjacent menu when hovering over its header. */
        if (my >= 0 && my < NS_MENU_H) {
            for (int i = 0; i < 5; i++) {
                if (mx >= g_ns_menu_item_x[i] &&
                    mx < g_ns_menu_item_x[i] + g_ns_menu_item_w[i] &&
                    i != g_ns_menu_open) {
                    g_ns_menu_open = i;
                    g_ns_menu_hot  = -1;
                    ns_redraw();
                    break;
                }
            }
        }
    }

    /* Toolbar hot-state refresh. */
    if (mx != last_mx || my != last_my) {
        last_mx = mx; last_my = my;
        if (g_ns_menu_open < 0) ns_redraw();
    }
}

/* ====================================================================
 *                Entry points
 * ==================================================================== */
static int32_t nexsheet_open_impl(uint32_t unused) {
    (void)unused;
    if (g_ns_win && g_ns_win->in_use) {
        wm_set_focus(g_ns_win);
        ns_redraw();
        return 1;
    }
    g_ns_win = wm_create_window(40, 30, NS_W, NS_H, L(STR_APP_NEXSHEET));
    if (!g_ns_win) return 0;
    wm_set_destroy_cb (g_ns_win, ns_destroy_cb);   /* install FIRST */
    wm_set_content_click(g_ns_win, ns_click, NULL);
    wm_set_key_handler(g_ns_win, ns_key);
    wm_set_resizable  (g_ns_win, true, NS_MIN_W, NS_MIN_H);
    wm_set_resize_cb  (g_ns_win, ns_resize_cb);
    wm_set_icon       (g_ns_win, ICON_NEXSHEET);

    /* Initialise workbook state on first open only.  Re-opening keeps
     * the previous data so the user can dismiss the window without
     * losing their work. */
    static bool initialised = false;
    if (!initialised) {
        sheet_init(&g_book);
        memset(g_fmt, 0, sizeof(g_fmt));
        for (int i = 0; i < NS_MAX_VIEW_COLS; i++) g_col_w[i] = 0;
        for (int i = 0; i < NS_MAX_VIEW_ROWS; i++) g_row_h[i] = 0;
        g_filename[0] = 0;
        initialised = true;
    }
    g_active_r = g_active_c = 0;
    g_sel_r0 = g_sel_r1 = 0;
    g_sel_c0 = g_sel_c1 = 0;
    ns_set_status("Ready.  F2 to edit, Enter to commit, Tab/Arrows to move.");
    ns_redraw();
    return 1;
}

bool nexsheet_open(void) {
    extern int32_t app_run_ring3(int32_t (*fn)(uint32_t), uint32_t arg,
                                  const char *name);
    int32_t rv = app_run_ring3(nexsheet_open_impl, 0, "nexsheet");
    return rv != 0;
}

void nexsheet_open_file(const char *display_name) {
    if (!nexsheet_open()) return;
    if (display_name && display_name[0]) {
        ns_open_from_name(display_name);
        ns_redraw();
    }
}
