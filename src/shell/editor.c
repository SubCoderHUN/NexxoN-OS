/* ============================================================================
 * NexxoN OS - Full-screen text editor  (v3.0, WM-aware)
 * ----------------------------------------------------------------------------
 * v3.0 changes over v2:
 *   - No longer writes directly to the raw VESA framebuffer.
 *   - editor_run() opens a dedicated WM window; all drawing goes into
 *     that window's content draw_target_t via gfx_draw_char/fill_rect.
 *   - wm_mark_dirty() is called after every render so the compositor picks
 *     up the change on the next wm_tick().
 *   - The editor window is destroyed on exit and the console window
 *     automatically regains focus.
 *
 * Layout (rows are 0-indexed inside the window's content area):
 *
 *      Row 0                    title bar
 *      Rows 1 .. content_rows-2 editor body (line numbers + text)
 *      Row content_rows-1       status bar
 * ============================================================================ */
#include "editor.h"
#include "terminal.h"
#include "vga.h"
#include "gfx.h"
#include "font.h"
#include "keyboard.h"
#include "nxfs.h"
#include "vfs.h"
#include "string.h"
#include "debug.h"
#include "window.h"
#include "icons.h"
#include "clipboard.h"
#include "i18n.h"
#include "dialogs.h"
#include "nxscript.h"

/* ---------- Tunables ----------------------------------------------------- */
#define EDITOR_MAX_LINES   512
#define EDITOR_LINE_LEN    256
#define EDITOR_LNUM_COLS   6        /* "%4d  " : 4 digits + 2 padding */

/* Colours (ARGB, matching the gfx_* / draw_target palette). */
#define EDITOR_BG          0xFF002060
#define EDITOR_FG          0xFFFFFFFF
#define EDITOR_LNUM_FG     0xFFFFE040
#define EDITOR_TITLE_BG    0xFF207070
#define EDITOR_TITLE_FG    0xFFFFFFFF
#define EDITOR_STATUS_BG   0xFFC0C0C0
#define EDITOR_STATUS_FG   0xFF000000
#define EDITOR_CURSOR_FG   0xFF002060
#define EDITOR_CURSOR_BG   0xFFFFFFFF

/* Window dimensions (content pixels, not total). */
#define EDITOR_WIN_X    20
#define EDITOR_WIN_Y    10
#define EDITOR_WIN_W    940
#define EDITOR_WIN_H    630

/* ---------- Editor state ------------------------------------------------- */
static char     g_lines[EDITOR_MAX_LINES][EDITOR_LINE_LEN];
static int      g_line_count;
static int      g_cur_row;
static int      g_cur_col;
static int      g_scroll_row;
static int      g_scroll_col;
static char     g_filename[64];
static uint32_t g_inode;
/* VFS mode: when non-empty the buffer round-trips through vfs_read /
 * vfs_write_file on this absolute path ("/usb0/notes.txt") instead of the
 * NXFS inode — text files on a pendrive are edited and saved IN PLACE. */
static char     g_vfs_path[352];
static bool     g_modified;
static char     g_msg[128];

/* WM window and its content target. */
static window_t      *g_editor_win    = NULL;
static draw_target_t *g_editor_target = NULL;

/* Forward declarations — the WM callbacks are defined after editor_run
 * but referenced inside it. */
static bool editor_handle_key (window_t *w, int k);
static void editor_destroy_cb (window_t *w);
static void editor_resize_cb  (window_t *w);

/* ---------- Geometry (derived from window content size) ----------------- */
static inline int editor_cols(void) {
    return g_editor_target ? (int)(g_editor_target->width  / FONT_GLYPH_W) : 80;
}
static inline int editor_rows(void) {
    return g_editor_target ? (int)(g_editor_target->height / FONT_GLYPH_H) : 25;
}
static inline int body_top_row   (void) { return 1; }
static inline int body_bot_row   (void) { return editor_rows() - 2; }
static inline int body_height    (void) { return body_bot_row() - body_top_row() + 1; }
static inline int body_left_col  (void) { return EDITOR_LNUM_COLS + 1; }
static inline int body_text_width(void) { return editor_cols() - body_left_col(); }

static int line_len(int row) {
    if (row < 0 || row >= g_line_count) return 0;
    return (int)strlen(g_lines[row]);
}

/* ---------- Low-level cell drawing (into editor window content) --------- */
static void put_cell(int col, int row, char c, uint32_t fg, uint32_t bg) {
    if (!g_editor_target) return;
    if (col < 0 || row < 0) return;
    if (col >= editor_cols() || row >= editor_rows()) return;
    gfx_draw_char(g_editor_target, col * FONT_GLYPH_W, row * FONT_GLYPH_H, c, fg, bg);
}

static void put_str(int col, int row, const char *s, uint32_t fg, uint32_t bg) {
    while (*s && col < editor_cols()) {
        put_cell(col++, row, *s++, fg, bg);
    }
}

static void put_row_fill(int row, uint32_t bg) {
    if (!g_editor_target) return;
    gfx_fill_rect(g_editor_target, 0, row * FONT_GLYPH_H,
                  (int)g_editor_target->width, FONT_GLYPH_H, bg);
}

/* ---------- File <-> in-memory buffer ----------------------------------- */
static void load_file(uint32_t inode) {
    g_inode = inode;
    g_line_count = 0;
    g_cur_row = g_cur_col = 0;
    g_scroll_row = g_scroll_col = 0;
    g_modified = false;
    g_msg[0] = 0;

    for (int i = 0; i < EDITOR_MAX_LINES; i++) g_lines[i][0] = 0;

    static char raw[NXFS_MAX_BLOCKS * NXFS_SECTOR_SIZE];
    uint32_t got = 0;
    if (g_vfs_path[0]) {
        int n = vfs_read(g_vfs_path, raw, sizeof(raw));
        got = (n > 0) ? (uint32_t)n : 0;
    } else {
        int r = nxfs_read_file(inode, raw, sizeof(raw), &got);
        if (r != NXFS_OK) got = 0;
    }
    if (got == 0) {
        g_line_count = 1;
        g_lines[0][0] = 0;
        return;
    }

    int line = 0, col = 0;
    for (uint32_t i = 0; i < got; i++) {
        if (line >= EDITOR_MAX_LINES) break;
        char c = raw[i];
        if (c == '\n') {
            g_lines[line][col] = 0;
            line++;
            col = 0;
        } else if (c == '\r') {
            /* swallow */
        } else if (col + 1 < EDITOR_LINE_LEN) {
            g_lines[line][col++] = c;
        }
    }
    if (line < EDITOR_MAX_LINES && (col > 0 || line == 0)) {
        g_lines[line][col] = 0;
        line++;
    }
    g_line_count = line;
    if (g_line_count == 0) { g_line_count = 1; g_lines[0][0] = 0; }
}

static int save_file(void) {
    static char out[NXFS_MAX_BLOCKS * NXFS_SECTOR_SIZE];
    uint32_t pos = 0;
    for (int i = 0; i < g_line_count; i++) {
        const char *s = g_lines[i];
        while (*s) {
            if (pos + 1 >= sizeof(out)) goto truncated;
            out[pos++] = *s++;
        }
        if (i + 1 < g_line_count) {
            if (pos + 1 >= sizeof(out)) goto truncated;
            out[pos++] = '\n';
        }
    }
truncated:
    int r;
    if (g_vfs_path[0]) {
        r = (vfs_write_file(g_vfs_path, out, pos) >= 0) ? NXFS_OK : -1;
    } else {
        r = nxfs_write_file(g_inode, out, pos);
    }
    if (r == NXFS_OK) {
        g_modified = false;
        ksnprintf(g_msg, sizeof(g_msg), L(STR_EDIT_SAVED_FMT), pos);
    } else {
        ksnprintf(g_msg, sizeof(g_msg), L(STR_EDIT_SAVE_FAIL_FMT), r);
    }
    return r;
}

/* ---------- Rendering --------------------------------------------------- */
static void render_title_bar(void) {
    put_row_fill(0, EDITOR_TITLE_BG);
    char title[160];
    ksnprintf(title, sizeof(title), " NexxoN Edit v3.0   --   %s%s",
              g_filename, g_modified ? "  [modified]" : "");
    put_str(1, 0, title, EDITOR_TITLE_FG, EDITOR_TITLE_BG);

    char meta[64];
    const char *lay = (keyboard_get_layout() == KBD_LAYOUT_HU) ? "HU" : "EN";
    ksnprintf(meta, sizeof(meta), "  [%s]  L%d:C%d ", lay, g_cur_row + 1, g_cur_col + 1);
    int len = (int)strlen(meta);
    int x = editor_cols() - len;
    if (x < 0) x = 0;
    put_str(x, 0, meta, EDITOR_TITLE_FG, EDITOR_TITLE_BG);
}

static void render_status_bar(void) {
    int row = editor_rows() - 1;
    put_row_fill(row, EDITOR_STATUS_BG);

    const char *hints = " F2:Save  F5:Run  ESC:Exit  Arrows:Move  PgUp/Dn:Page  Home/End:Line  Bksp/Del:Erase ";
    put_str(0, row, hints, EDITOR_STATUS_FG, EDITOR_STATUS_BG);

    if (g_msg[0]) {
        int len = (int)strlen(g_msg);
        int x = editor_cols() - len - 1;
        if (x < 0) x = 0;
        put_str(x, row, g_msg, 0xFFFF0000, EDITOR_STATUS_BG);
    }
}

static void render_one_body_row(int screen_row) {
    int file_row = g_scroll_row + (screen_row - body_top_row());

    put_row_fill(screen_row, EDITOR_BG);

    if (file_row < 0 || file_row >= g_line_count) {
        put_cell(0, screen_row, '~', VGA_GRAY, EDITOR_BG);
        return;
    }

    char lnum[8];
    ksnprintf(lnum, sizeof(lnum), "%4d  ", file_row + 1);
    put_str(0, screen_row, lnum, EDITOR_LNUM_FG, EDITOR_BG);

    const char *s = g_lines[file_row];
    int slen = (int)strlen(s);
    int start = g_scroll_col;
    int x = body_left_col();
    int max_chars = body_text_width();
    for (int i = 0; i < max_chars && start + i < slen; i++) {
        put_cell(x + i, screen_row, s[start + i], EDITOR_FG, EDITOR_BG);
    }
}

static void render_cursor(void) {
    int screen_row = g_cur_row - g_scroll_row + body_top_row();
    int screen_col = g_cur_col - g_scroll_col + body_left_col();
    if (screen_row < body_top_row() || screen_row > body_bot_row()) return;
    if (screen_col < body_left_col() || screen_col >= editor_cols()) return;

    char c = ' ';
    if (g_cur_row >= 0 && g_cur_row < g_line_count) {
        int len = line_len(g_cur_row);
        if (g_cur_col >= 0 && g_cur_col < len) c = g_lines[g_cur_row][g_cur_col];
    }
    put_cell(screen_col, screen_row, c, EDITOR_CURSOR_FG, EDITOR_CURSOR_BG);
}

static void render_all(void) {
    render_title_bar();
    for (int r = body_top_row(); r <= body_bot_row(); r++) {
        render_one_body_row(r);
    }
    render_status_bar();
    render_cursor();
    wm_mark_dirty();
}

/* ---------- Scroll management ------------------------------------------- */
static void adjust_scroll(void) {
    int bh = body_height();
    int bw = body_text_width();

    if (g_cur_row < g_scroll_row) g_scroll_row = g_cur_row;
    if (g_cur_row >= g_scroll_row + bh) g_scroll_row = g_cur_row - bh + 1;
    if (g_scroll_row < 0) g_scroll_row = 0;

    if (g_cur_col < g_scroll_col) g_scroll_col = g_cur_col;
    if (g_cur_col >= g_scroll_col + bw) g_scroll_col = g_cur_col - bw + 1;
    if (g_scroll_col < 0) g_scroll_col = 0;
}

/* ---------- Editing ops -------------------------------------------------- */
static void clamp_cursor(void) {
    if (g_cur_row < 0) g_cur_row = 0;
    if (g_cur_row >= g_line_count) g_cur_row = g_line_count - 1;
    int len = line_len(g_cur_row);
    if (g_cur_col < 0) g_cur_col = 0;
    if (g_cur_col > len) g_cur_col = len;
}

static void insert_char_at_cursor(char c) {
    if (g_cur_row < 0 || g_cur_row >= g_line_count) return;
    char *line = g_lines[g_cur_row];
    int len = (int)strlen(line);
    if (len + 1 >= EDITOR_LINE_LEN) {
        ksnprintf(g_msg, sizeof(g_msg), "Line full (%d chars max).", EDITOR_LINE_LEN - 1);
        return;
    }
    for (int i = len; i >= g_cur_col; i--) line[i + 1] = line[i];
    line[g_cur_col] = c;
    g_cur_col++;
    g_modified = true;
}

static void delete_char_left(void) {
    if (g_cur_col > 0) {
        char *line = g_lines[g_cur_row];
        int len = (int)strlen(line);
        for (int i = g_cur_col - 1; i < len; i++) line[i] = line[i + 1];
        g_cur_col--;
        g_modified = true;
        return;
    }
    if (g_cur_row == 0) return;
    int prev_len = line_len(g_cur_row - 1);
    int cur_len  = line_len(g_cur_row);
    if (prev_len + cur_len + 1 >= EDITOR_LINE_LEN) {
        ksnprintf(g_msg, sizeof(g_msg), "Cannot join: line too long.");
        return;
    }
    memcpy(g_lines[g_cur_row - 1] + prev_len, g_lines[g_cur_row], cur_len + 1);
    for (int i = g_cur_row; i + 1 < g_line_count; i++)
        memcpy(g_lines[i], g_lines[i + 1], EDITOR_LINE_LEN);
    g_line_count--;
    g_cur_row--;
    g_cur_col = prev_len;
    g_modified = true;
}

static void delete_char_right(void) {
    char *line = g_lines[g_cur_row];
    int len = (int)strlen(line);
    if (g_cur_col < len) {
        for (int i = g_cur_col; i < len; i++) line[i] = line[i + 1];
        g_modified = true;
        return;
    }
    if (g_cur_row + 1 >= g_line_count) return;
    int next_len = line_len(g_cur_row + 1);
    if (len + next_len + 1 >= EDITOR_LINE_LEN) {
        ksnprintf(g_msg, sizeof(g_msg), "Cannot join: line too long.");
        return;
    }
    memcpy(g_lines[g_cur_row] + len, g_lines[g_cur_row + 1], next_len + 1);
    for (int i = g_cur_row + 1; i + 1 < g_line_count; i++)
        memcpy(g_lines[i], g_lines[i + 1], EDITOR_LINE_LEN);
    g_line_count--;
    g_modified = true;
}

static void split_line_at_cursor(void) {
    if (g_line_count >= EDITOR_MAX_LINES) {
        ksnprintf(g_msg, sizeof(g_msg), "Too many lines (%d max).", EDITOR_MAX_LINES);
        return;
    }
    for (int i = g_line_count; i > g_cur_row + 1; i--)
        memcpy(g_lines[i], g_lines[i - 1], EDITOR_LINE_LEN);
    char *cur  = g_lines[g_cur_row];
    char *next = g_lines[g_cur_row + 1];
    int curlen = (int)strlen(cur);
    int tail   = curlen - g_cur_col;
    if (tail < 0) tail = 0;
    memcpy(next, cur + g_cur_col, (size_t)tail);
    next[tail] = 0;
    cur[g_cur_col] = 0;
    g_line_count++;
    g_cur_row++;
    g_cur_col = 0;
    g_modified = true;
}

/* ---------- Sentinel returned when the editor must exit immediately ---- */
/* Returned by editor_wait_key() when the editor's WM window was destroyed
 * out from under us - e.g. the user clicked the title-bar X button.  Falls
 * outside the 0..0xFF kbd range so it can never collide with a real keystroke. */
#define EDITOR_KEY_DESTROYED   0x10000

/* True if the editor's window is still alive and owned by us.  Centralised
 * so every wait + render call site has a single answer. */
static bool editor_window_alive(void) {
    return g_editor_win && g_editor_win->in_use;
}

/* Discard everything currently in the kbd ring buffer.  Run before any
 * modal prompt (confirm_discard, future Save-As dialog, ...) so a fast
 * typist's queued keystrokes don't auto-answer the dialog. */
static void editor_drain_input(void) {
    while (keyboard_has_data()) (void)keyboard_wait_getc();
}

/* ---------- Idle wait: tick the WM while waiting for a keystroke. -------- */
/* The editor's wait loop must drive wm_tick() the same way the shell's
 * read_line does - otherwise the compositor never paints the editor window
 * after wm_mark_dirty() and the screen looks frozen even though the editor
 * itself is running fine.
 *
 * The wait also has to spot external window destruction (mouse-click on
 * the X button) so the loop above can break out gracefully instead of
 * spinning forever rendering into a freed framebuffer. */
static int editor_wait_key(void) {
    extern void pnp_tick(void);
    while (!keyboard_has_data()) {
        if (!editor_window_alive()) return EDITOR_KEY_DESTROYED;
        pnp_tick();          /* drive native USB keyboard/mouse (post-usbnative) */
        wm_tick();
        __asm__ volatile ("sti; hlt");
    }
    return keyboard_wait_getc();
}

/* ---------- Confirm-discard prompt --------------------------------------- */
static bool confirm_discard(void) {
    int row = editor_rows() - 1;
    put_row_fill(row, 0xFFBB0000);
    const char *msg = " Unsaved!  F2:save & exit  Y:discard  other:cancel ";
    put_str(0, row, msg, EDITOR_TITLE_FG, 0xFFBB0000);
    wm_mark_dirty();
    /* Force the prompt onto the screen NOW so the user actually sees the
     * choice before they pick - the previous code only marked dirty and
     * the next compose could be a frame away. */
    wm_present();
    /* Stale keystrokes ('y' you typed earlier in the file, an Enter that
     * triggered the modified state, ...) used to auto-resolve this dialog
     * before the user could read it.  Drain them. */
    editor_drain_input();

    while (1) {
        int k = editor_wait_key();
        if (k == EDITOR_KEY_DESTROYED) return true;     /* X = discard */
        if (k == KEY_F2)               { save_file(); return true; }
        if (k == 'y' || k == 'Y')      return true;
        if (k == 'n' || k == 'N' || k == KEY_ESCAPE) return false;
        /* Anything else: re-show the prompt (it may have been over-
         * written by render passes) and keep waiting. */
        put_row_fill(row, 0xFFBB0000);
        put_str(0, row, msg, EDITOR_TITLE_FG, 0xFFBB0000);
        wm_mark_dirty();
        wm_present();
    }
}

/* ---------- Public entry points ------------------------------------------ */
static void editor_run_core(uint32_t inode, const char *display_name);

void editor_run(uint32_t inode, const char *display_name) {
    g_vfs_path[0] = 0;                       /* NXFS-inode mode */
    editor_run_core(inode, display_name);
}

/* Open a file living on a MOUNTED volume ("/usb0/notes.txt"): the buffer
 * loads via vfs_read and F2 saves back via vfs_write_file, so pendrive
 * text files are edited in place. */
void editor_run_vfs(const char *vfs_path, const char *display_name) {
    if (!vfs_path || !vfs_path[0]) return;
    strncpy(g_vfs_path, vfs_path, sizeof(g_vfs_path) - 1);
    g_vfs_path[sizeof(g_vfs_path) - 1] = 0;
    editor_run_core(0, display_name);
}

static void editor_run_core(uint32_t inode, const char *display_name) {
    debug_printf("[editor] editor_run(inode=%u, vfs='%s', name=\"%s\")\n",
                 inode, g_vfs_path, display_name ? display_name : "(null)");

    strncpy(g_filename, display_name ? display_name : "(noname)",
            sizeof(g_filename) - 1);
    g_filename[sizeof(g_filename) - 1] = 0;

    /* Reset all editor state BEFORE we touch the window or the FS so a
     * second efile invocation cannot trip over leftover pointers from the
     * previous session. */
    g_editor_win    = NULL;
    g_editor_target = NULL;
    g_line_count    = 0;
    g_cur_row = g_cur_col = 0;
    g_scroll_row = g_scroll_col = 0;
    g_modified = false;
    g_msg[0] = 0;

    /* Open a dedicated window for the editor. */
    debug_printf("[editor] requesting WM window %dx%d @ (%d,%d)\n",
                 EDITOR_WIN_W, EDITOR_WIN_H, EDITOR_WIN_X, EDITOR_WIN_Y);
    g_editor_win = wm_create_window(EDITOR_WIN_X, EDITOR_WIN_Y,
                                    EDITOR_WIN_W, EDITOR_WIN_H,
                                    L(STR_APP_EDITOR));
    if (!g_editor_win) {
        debug_fail("editor", "wm_create_window returned NULL");
        term_printf("efile: no window slot available\n");
        return;
    }
    g_editor_target = &g_editor_win->content;
    wm_set_focus(g_editor_win);
    /* ISSUE 3: no more wm_push_modal — the editor is now a peer of
     * every other top-level window.  Background windows can take focus
     * via the regular click path. */
    debug_printf("[editor] window ready: id=%d content %ux%u\n",
                 g_editor_win->id,
                 g_editor_target->width, g_editor_target->height);

    debug_printf("[editor] loading file (inode=%u) ...\n", inode);
    load_file(inode);
    debug_printf("[editor] file loaded, %d lines\n", g_line_count);

    /* Register the per-window key + destroy callbacks BEFORE the first
     * paint so a fast user can't beat the dispatch table into a race. */
    wm_set_key_handler(g_editor_win, editor_handle_key);
    wm_set_destroy_cb (g_editor_win, editor_destroy_cb);
    /* TASK 7: re-render on resize so the text area never greys out (the
     * row count is derived from the content height, so it reflows too). */
    wm_set_resize_cb  (g_editor_win, editor_resize_cb);
    wm_set_icon       (g_editor_win, ICON_EDITOR);

    render_all();
    /* Force one immediate compose so the editor is visible before any
     * keystrokes arrive — every subsequent refresh goes through the
     * normal dirty-flag path inside wm_tick. */
    wm_present();
    debug_printf("[editor] window mounted; returning to caller (non-blocking)\n");
}

/* ISSUE 3: per-window key handler invoked by wm_dispatch_keys() when
 * the editor window holds focus.  Replaces the v3 blocking key loop
 * with an event-driven model so the desktop, taskbar, and every other
 * background window remain interactive while the editor is open. */
static bool editor_handle_key(window_t *w, int k) {
    if (w != g_editor_win || !g_editor_win->in_use) return false;
    g_msg[0] = 0;

    switch (k) {
        case KEY_CTRL_C:
        case KEY_ESCAPE:
            keyboard_clear_abort();
            if (g_modified) {
                /* Synchronous discard prompt — dialog_yes_no pumps
                 * wm_tick internally so the rest of the desktop keeps
                 * compositing while the user reads the choice. */
                bool discard = dialog_yes_no(
                    L(STR_DLG_UNSAVED_TITLE),
                    L(STR_DLG_UNSAVED_BODY),
                    L(STR_DLG_UNSAVED_QUESTION));
                if (!discard) { render_all(); return true; }
            }
            debug_printf("[editor] ESC/Ctrl+C exit (modified=%d)\n",
                         (int)g_modified);
            wm_destroy_window(g_editor_win);   /* fires editor_destroy_cb */
            return true;

        case KEY_F2:
            save_file();
            break;

        case KEY_F5: {
            if (save_file() != NXFS_OK) break;
            static char run_src[NXFS_MAX_BLOCKS * NXFS_SECTOR_SIZE];
            uint32_t pos = 0;
            for (int i = 0; i < g_line_count && pos + 2 < sizeof(run_src); i++) {
                const char *s = g_lines[i];
                while (*s && pos + 1 < sizeof(run_src)) run_src[pos++] = *s++;
                if (pos + 1 < sizeof(run_src)) run_src[pos++] = '\n';
            }
            run_src[pos] = '\0';
            int rr = nxscript_eval(run_src);
            if (rr != NX_OK)
                ksnprintf(g_msg, sizeof(g_msg), "Script error: %s",
                          nxscript_last_error());
            else
                ksnprintf(g_msg, sizeof(g_msg), "Script ran OK.");
            break;
        }

        case KEY_UP:
            if (g_cur_row > 0) g_cur_row--;
            clamp_cursor();
            break;
        case KEY_DOWN:
            if (g_cur_row + 1 < g_line_count) g_cur_row++;
            clamp_cursor();
            break;
        case KEY_LEFT:
            if (g_cur_col > 0) g_cur_col--;
            else if (g_cur_row > 0) { g_cur_row--; g_cur_col = line_len(g_cur_row); }
            break;
        case KEY_RIGHT:
            if (g_cur_col < line_len(g_cur_row)) g_cur_col++;
            else if (g_cur_row + 1 < g_line_count) { g_cur_row++; g_cur_col = 0; }
            break;
        case KEY_HOME: g_cur_col = 0; break;
        case KEY_END:  g_cur_col = line_len(g_cur_row); break;

        case KEY_PGUP: {
            int step = body_height();
            g_cur_row -= step;
            if (g_cur_row < 0) g_cur_row = 0;
            g_scroll_row -= step;
            if (g_scroll_row < 0) g_scroll_row = 0;
            clamp_cursor();
            break;
        }
        case KEY_PGDN: {
            int step = body_height();
            g_cur_row += step;
            if (g_cur_row >= g_line_count) g_cur_row = g_line_count - 1;
            g_scroll_row += step;
            clamp_cursor();
            break;
        }

        case KEY_BACKSPACE: delete_char_left();  break;
        case KEY_DEL:       delete_char_right(); break;
        case KEY_ENTER:     split_line_at_cursor(); break;

        /* TASK 18: clipboard integration. */
        case KEY_CTRL_V: {
            const char *p = clipboard_peek_text();
            if (p) {
                while (*p) {
                    if (*p == '\n') split_line_at_cursor();
                    else if (*p >= 0x20 || *p == '\t') insert_char_at_cursor(*p);
                    p++;
                }
                ksnprintf(g_msg, sizeof(g_msg), "%s", L(STR_EDIT_PASTE));
            }
            break;
        }
        case KEY_CTRL_X: {
            /* Copy current line to clipboard and delete it. */
            if (g_cur_row >= 0 && g_cur_row < g_line_count) {
                clipboard_set_text(g_lines[g_cur_row], 0);
                /* Shift up. */
                for (int i = g_cur_row; i + 1 < g_line_count; i++) {
                    memcpy(g_lines[i], g_lines[i + 1], EDITOR_LINE_LEN);
                }
                if (g_line_count > 1) g_line_count--;
                else g_lines[0][0] = 0;
                if (g_cur_row >= g_line_count) g_cur_row = g_line_count - 1;
                g_cur_col = 0;
                g_modified = true;
                ksnprintf(g_msg, sizeof(g_msg), "%s", L(STR_EDIT_CUT));
            }
            break;
        }
        case KEY_CTRL_F:
            ksnprintf(g_msg, sizeof(g_msg), "%s", L(STR_EDIT_FIND_PROMPT));
            break;

        default:
            if (k == '\t') {
                for (int i = 0; i < 4; i++) insert_char_at_cursor(' ');
            } else if ((k >= 0x20 && k < 0x7F) || (k >= 0xA0 && k <= 0xFF)) {
                insert_char_at_cursor((char)k);
            }
            break;
    }
    adjust_scroll();
    render_all();
    return true;
}

/* ISSUE 3: cleanup runs from the WM destroy notification path so close
 * paths (X button, taskbar Bezárás, ESC, external destroy) all converge
 * here.  Pointer is cleared before the framebuffer is freed so any
 * concurrent renderer that races us bails on the NULL check. */
static void editor_destroy_cb(window_t *w) {
    if (w != g_editor_win) return;
    g_editor_win    = NULL;
    g_editor_target = NULL;
    /* Drain any keystrokes the user buffered during the session so they
     * don't auto-fill the shell prompt. */
    while (keyboard_has_data()) (void)keyboard_wait_getc();
    wm_refocus_shell();
    wm_mark_dirty();
    debug_printf("[editor] destroy_cb: focus handed back to shell\n");
}

/* TASK 7: the WM resized our window — keep the cursor on screen and repaint
 * the whole (reflowed) content so it never shows a stale grey buffer. */
static void editor_resize_cb(window_t *w) {
    if (w != g_editor_win || !editor_window_alive()) return;
    adjust_scroll();
    render_all();
}

