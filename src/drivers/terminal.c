/* ============================================================================
 * NexxoN OS - Text terminal (v4.0, render-target aware)
 * ----------------------------------------------------------------------------
 * Owns:
 *   * A shadow buffer of (char, fg, bg) cells for fast scroll/re-render.
 *   * A pointer to the current draw_target_t (defaults to the screen).
 *   * The animated idle spinner and an optional dirty-callback that the
 *     window manager subscribes to so console output triggers recompose.
 * ============================================================================ */
#include "terminal.h"
#include "vga.h"
#include "gfx.h"
#include "font.h"
#include "string.h"
#include "debug.h"

typedef struct {
    char        ch;
    vga_color_t fg;
    vga_color_t bg;
} term_cell_t;

static term_cell_t      g_cells[TERM_ROWS_MAX][TERM_COLS_MAX];
static uint32_t         g_cols;
static uint32_t         g_rows;
static int              g_col;
static int              g_row;
static vga_color_t      g_fg = VGA_LTGRAY;
static vga_color_t      g_bg = VGA_BLACK;

static bool             g_idle_on     = false;
static int              g_idle_frame  = 0;
static const char       g_idle_glyphs[4] = { '|', '/', '-', '\\' };

static draw_target_t   *g_target  = NULL;
static term_dirty_cb_t  g_dirty_cb = NULL;

/* Output capture state (shell pipe / redirection). */
static char    *g_cap_buf = NULL;
static uint32_t g_cap_sz  = 0;
static uint32_t g_cap_pos = 0;

/* ---------- Scrollback ring buffer (TASK 9) ----------------------------- *
 * Every row that scrolls off the top is pushed into g_history; PgUp/PgDn
 * pans g_view_offset across that history.  Offset 0 means "live tail" —
 * any input or new output snaps the viewport back to the bottom.        */
#define TERM_HISTORY_ROWS  256
static term_cell_t g_history[TERM_HISTORY_ROWS][TERM_COLS_MAX];
static uint32_t    g_history_count = 0;     /* rows ever pushed (capped at HISTORY_ROWS) */
static uint32_t    g_history_head  = 0;     /* next write index into the ring */
static uint32_t    g_view_offset   = 0;     /* 0 = live, >0 = scrolled back N rows */

static void repaint_full(void);    /* fwd decl */

static term_region_dirty_cb_t g_region_cb = NULL;
void term_set_region_dirty_cb(term_region_dirty_cb_t cb) { g_region_cb = cb; }

static inline void notify_dirty(void) {
    if (g_dirty_cb) g_dirty_cb();
}

/* Report a single changed cell as a small region (when a region callback is
 * registered) instead of a full-screen dirty — used by the high-frequency
 * idle spinner.  Falls back to the full dirty callback. */
static inline void notify_cell_dirty(int col, int row) {
    if (g_region_cb)
        g_region_cb(col * FONT_GLYPH_W, row * FONT_GLYPH_H, FONT_GLYPH_W, FONT_GLYPH_H);
    else
        notify_dirty();
}

static inline void rasterise_cell(int col, int row) {
    const term_cell_t *c = &g_cells[row][col];
    gfx_draw_char(g_target, col * FONT_GLYPH_W, row * FONT_GLYPH_H,
                  c->ch, c->fg, c->bg);
}

static void recompute_grid(void) {
    if (!g_target) return;
    g_cols = g_target->width  / FONT_GLYPH_W;
    g_rows = g_target->height / FONT_GLYPH_H;
    if (g_cols > TERM_COLS_MAX) g_cols = TERM_COLS_MAX;
    if (g_rows > TERM_ROWS_MAX) g_rows = TERM_ROWS_MAX;
}

void term_init(void) {
    g_target = gfx_screen();
    recompute_grid();
    g_col = g_row = 0;
    g_fg  = VGA_LTGRAY;
    g_bg  = VGA_BLACK;
    g_idle_on    = false;
    g_idle_frame = 0;
    term_clear();
}

uint32_t term_cols(void) { return g_cols; }
uint32_t term_rows(void) { return g_rows; }

void term_clear(void) {
    for (uint32_t r = 0; r < g_rows; r++) {
        for (uint32_t c = 0; c < g_cols; c++) {
            g_cells[r][c].ch = ' ';
            g_cells[r][c].fg = g_fg;
            g_cells[r][c].bg = g_bg;
        }
    }
    if (g_target) gfx_clear(g_target, g_bg);
    g_col = 0;
    g_row = 0;
    notify_dirty();
}

void term_set_color(vga_color_t fg, vga_color_t bg) { g_fg = fg; g_bg = bg; }
void term_get_color(vga_color_t *fg, vga_color_t *bg) {
    if (fg) *fg = g_fg;
    if (bg) *bg = g_bg;
}

void term_set_cursor(int col, int row) {
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if ((uint32_t)col >= g_cols) col = (int)g_cols - 1;
    if ((uint32_t)row >= g_rows) row = (int)g_rows - 1;
    g_col = col;
    g_row = row;
}

void term_get_cursor(int *col, int *row) {
    if (col) *col = g_col;
    if (row) *row = g_row;
}

void term_raw_char(int col, int row, char c, vga_color_t fg, vga_color_t bg) {
    if (col < 0 || row < 0) return;
    if ((uint32_t)col >= g_cols || (uint32_t)row >= g_rows) return;
    g_cells[row][col].ch = c;
    g_cells[row][col].fg = fg;
    g_cells[row][col].bg = bg;
    rasterise_cell(col, row);
    notify_dirty();
}

static void scroll_one(void) {
    /* Snapshot the row about to fall off the top into the scrollback
     * ring so the user can pan it back into view with PgUp. */
    memcpy(g_history[g_history_head], g_cells[0], sizeof(g_cells[0]));
    g_history_head = (g_history_head + 1u) % TERM_HISTORY_ROWS;
    if (g_history_count < TERM_HISTORY_ROWS) g_history_count++;

    for (uint32_t r = 0; r + 1 < g_rows; r++) {
        memcpy(g_cells[r], g_cells[r + 1], sizeof(g_cells[0]));
    }
    for (uint32_t c = 0; c < g_cols; c++) {
        g_cells[g_rows - 1][c].ch = ' ';
        g_cells[g_rows - 1][c].fg = g_fg;
        g_cells[g_rows - 1][c].bg = g_bg;
    }
    if (g_target) gfx_scroll_up(g_target, FONT_GLYPH_H, g_bg);
    for (uint32_t c = 0; c < g_cols; c++) {
        rasterise_cell((int)c, (int)g_rows - 1);
    }
}

static void newline(void) {
    g_col = 0;
    g_row++;
    if ((uint32_t)g_row >= g_rows) {
        scroll_one();
        g_row = (int)g_rows - 1;
    }
}

void term_putc(char c) {
    /* When a pipe/redirect capture is active, divert output to the buffer
     * instead of the screen so the user doesn't see the command's raw output. */
    if (g_cap_buf) {
        if (g_cap_pos + 1 < g_cap_sz) {
            g_cap_buf[g_cap_pos++] = c;
            g_cap_buf[g_cap_pos]   = 0;
        }
        return;
    }
    /* New output always jumps the viewport back to the live tail so
     * shell prompts and progress lines don't appear "stuck" behind a
     * forgotten scroll-back. */
    if (g_view_offset != 0) {
        g_view_offset = 0;
        repaint_full();
    }
    bool was_idle = g_idle_on;
    if (was_idle) term_idle_hide();

    switch (c) {
        case '\n': newline(); break;
        case '\r': g_col = 0; break;
        case '\b':
            if (g_col > 0) {
                g_col--;
                g_cells[g_row][g_col].ch = ' ';
                rasterise_cell(g_col, g_row);
            }
            break;
        case '\t':
            do { term_putc(' '); } while (g_col % 4 != 0);
            break;
        default: {
            if ((uint32_t)g_col >= g_cols) newline();
            g_cells[g_row][g_col].ch = c;
            g_cells[g_row][g_col].fg = g_fg;
            g_cells[g_row][g_col].bg = g_bg;
            rasterise_cell(g_col, g_row);
            g_col++;
            break;
        }
    }
    if (was_idle) term_idle_show();
    notify_dirty();
}

void term_puts(const char *s) { while (*s) term_putc(*s++); }

void term_printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_puts(buf);
}

/* ---------- Output capture for shell pipes / redirection ---------------- */
void term_capture_start(char *buf, uint32_t sz) {
    g_cap_buf = buf;
    g_cap_sz  = sz;
    g_cap_pos = 0;
    if (buf && sz > 0) buf[0] = 0;
}

uint32_t term_capture_stop(void) {
    uint32_t n = g_cap_pos;
    if (g_cap_buf && g_cap_pos < g_cap_sz)
        g_cap_buf[g_cap_pos] = 0;
    g_cap_buf = NULL;
    g_cap_sz  = 0;
    g_cap_pos = 0;
    return n;
}

bool term_capturing(void) { return g_cap_buf != NULL; }

/* ---------- Idle spinner ------------------------------------------------- */
void term_idle_show(void) {
    if (!g_target || (uint32_t)g_col >= g_cols || (uint32_t)g_row >= g_rows) return;
    g_idle_on = true;
    gfx_draw_char(g_target, g_col * FONT_GLYPH_W, g_row * FONT_GLYPH_H,
                  g_idle_glyphs[g_idle_frame & 3], VGA_YELLOW, g_bg);
    notify_cell_dirty(g_col, g_row);
}

void term_idle_hide(void) {
    if (!g_idle_on) return;
    g_idle_on = false;
    rasterise_cell(g_col, g_row);
    notify_cell_dirty(g_col, g_row);
}

void term_idle_tick(void) {
    if (!g_idle_on || !g_target) return;
    g_idle_frame = (g_idle_frame + 1) & 3;
    gfx_draw_char(g_target, g_col * FONT_GLYPH_W, g_row * FONT_GLYPH_H,
                  g_idle_glyphs[g_idle_frame], VGA_YELLOW, g_bg);
    notify_cell_dirty(g_col, g_row);
}

/* ---------- Non-destructive cursor movement (for line editing) ---------- */
void term_cursor_move_left(void) {
    if (g_col > 0) { g_col--; notify_dirty(); }
}

/* ---------- Pin cursor to the last row (bottom-anchor prompt) ----------- */
void term_pin_to_bottom(void) {
    /* scroll_one() shifts cell content up but NEVER changes g_row, so
     * putting it in a "while (g_row < g_rows-1)" loop would be infinite.
     *
     * Correct approach: simply teleport the cursor to the last row.
     * Content above is untouched; subsequent output writes there and
     * scrolls naturally via the existing newline() / scroll_one() path. */
    if ((uint32_t)g_row < g_rows - 1) {
        g_row = (int)g_rows - 1;
        g_col = 0;
    }
}

/* ---------- Target swap (used by the WM) -------------------------------- */
void term_set_target(draw_target_t *t) {
    if (!t) return;
    g_target = t;
    recompute_grid();
    g_col = g_row = 0;
    g_idle_on = false;
    term_clear();           /* also fires dirty cb */
}

draw_target_t *term_get_target(void) { return g_target; }

void term_set_dirty_cb(term_dirty_cb_t cb) { g_dirty_cb = cb; }

/* ---------- Scrollback viewing (TASK 9) -------------------------------- *
 * Pull the cell that should be rendered at logical (col, row) given the
 * current g_view_offset.  When the viewport is at the live tail (offset
 * 0) we read from g_cells; when scrolled back we synthesise rows from
 * the history ring + the top of g_cells.                              */
static const term_cell_t *viewport_cell(uint32_t col, uint32_t row) {
    if (g_view_offset == 0) {
        return &g_cells[row][col];
    }
    /* Logical row from top of viewport.  Effective row in the combined
     * (history ++ live) stream = (history_count - view_offset) + row.
     * Anything in [0, view_offset) of that range maps into the history
     * ring; the rest falls through into g_cells. */
    uint32_t hist_start = (g_history_count >= g_view_offset)
                          ? (g_history_count - g_view_offset)
                          : 0;
    uint32_t r_in_stream = hist_start + row;
    if (r_in_stream < g_history_count) {
        uint32_t ring_idx = (g_history_head + TERM_HISTORY_ROWS
                             - g_history_count + r_in_stream)
                            % TERM_HISTORY_ROWS;
        return &g_history[ring_idx][col];
    }
    uint32_t live_r = r_in_stream - g_history_count;
    if (live_r >= g_rows) live_r = g_rows - 1;
    return &g_cells[live_r][col];
}

static void repaint_full(void) {
    if (!g_target) return;
    gfx_clear(g_target, g_bg);
    for (uint32_t r = 0; r < g_rows; r++) {
        for (uint32_t c = 0; c < g_cols; c++) {
            const term_cell_t *cell = viewport_cell(c, r);
            gfx_draw_char(g_target, (int)(c * FONT_GLYPH_W),
                          (int)(r * FONT_GLYPH_H),
                          cell->ch, cell->fg, cell->bg);
        }
    }
    /* Scrollback indicator: a tiny vertical bar on the right edge whose
     * top tracks the viewport's position within the combined history +
     * live stream.  Skipped when there's no scrollback to show. */
    if (g_history_count > 0) {
        uint32_t total      = g_history_count + g_rows;
        uint32_t fb_h       = g_target->height;
        uint32_t fb_w       = g_target->width;
        int      bar_w      = 4;
        int      bar_x      = (int)fb_w - bar_w - 1;
        int      thumb_h    = (int)((fb_h * g_rows) / total);
        if (thumb_h < 8) thumb_h = 8;
        if (thumb_h > (int)fb_h) thumb_h = (int)fb_h;
        uint32_t bottom_row = (g_history_count - g_view_offset) + g_rows;
        uint32_t top_row    = bottom_row - g_rows;
        int      thumb_y    = (int)((fb_h * top_row) / total);
        gfx_fill_rect(g_target, bar_x, 0, bar_w, (int)fb_h, 0xFF202028);
        gfx_fill_rect(g_target, bar_x, thumb_y, bar_w, thumb_h,
                      g_view_offset == 0 ? 0xFF60A060 : 0xFFFFE040);
    }
    notify_dirty();
}

void term_scroll_up(int rows) {
    if (rows <= 0) return;
    if (g_history_count == 0) return;
    uint32_t want = g_view_offset + (uint32_t)rows;
    if (want > g_history_count) want = g_history_count;
    if (want == g_view_offset) return;
    g_view_offset = want;
    repaint_full();
}

void term_scroll_down(int rows) {
    if (rows <= 0) return;
    if (g_view_offset == 0) return;
    if ((uint32_t)rows > g_view_offset) g_view_offset = 0;
    else g_view_offset -= (uint32_t)rows;
    repaint_full();
}

void term_scroll_to_bottom(void) {
    if (g_view_offset == 0) return;
    g_view_offset = 0;
    repaint_full();
}

uint32_t term_scroll_offset(void) { return g_view_offset; }

/* Resize-aware target swap.  Unlike term_set_target() this preserves
 * the cached cell grid so a window resize doesn't wipe the user's
 * scrollback.  Called from the shell-window resize callback. */
void term_retarget_preserve(draw_target_t *t) {
    if (!t) return;
    g_target = t;
    uint32_t new_cols = t->width  / FONT_GLYPH_W;
    uint32_t new_rows = t->height / FONT_GLYPH_H;
    if (new_cols > TERM_COLS_MAX) new_cols = TERM_COLS_MAX;
    if (new_rows > TERM_ROWS_MAX) new_rows = TERM_ROWS_MAX;
    if (new_cols == 0 || new_rows == 0) return;

    /* If the visible area shrank we lose nothing useful — the cells beyond
     * the new bounds simply aren't rasterised.  If the area grew, the
     * extra rows/cols already hold the default ' '/fg/bg from term_init,
     * so the new area paints as expected without explicit clearing. */
    g_cols = new_cols;
    g_rows = new_rows;
    if ((uint32_t)g_col >= g_cols) g_col = (int)g_cols - 1;
    if ((uint32_t)g_row >= g_rows) g_row = (int)g_rows - 1;

    /* Paint over the entire new framebuffer with the current bg so any
     * stale chrome-grey from window_resize_framebuffer disappears in a
     * single pass before we redraw text. */
    gfx_clear(g_target, g_bg);
    for (uint32_t r = 0; r < g_rows; r++) {
        for (uint32_t c = 0; c < g_cols; c++) {
            rasterise_cell((int)c, (int)r);
        }
    }
    notify_dirty();
}
