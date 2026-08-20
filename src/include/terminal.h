/* ============================================================================
 * NexxoN OS - Text terminal (drawn on top of the VGA framebuffer)
 * ----------------------------------------------------------------------------
 * In v4 the terminal can render into ANY draw_target_t.  Pre-GUI bring-up
 * code aims it at the main screen; once the window manager exists, the
 * shell calls term_set_target() to redirect into the console window's
 * private framebuffer.
 * ============================================================================ */
#ifndef NEXXON_TERMINAL_H
#define NEXXON_TERMINAL_H

#include "types.h"
#include "vga.h"
#include "gfx.h"

#define TERM_COLS_MAX   128
#define TERM_ROWS_MAX   96

void term_init        (void);
void term_clear       (void);
void term_set_color   (vga_color_t fg, vga_color_t bg);
void term_get_color   (vga_color_t *fg, vga_color_t *bg);
void term_putc        (char c);
void term_puts        (const char *s);
void term_printf      (const char *fmt, ...);

void term_set_cursor  (int col, int row);
void term_get_cursor  (int *col, int *row);

void term_idle_show   (void);
void term_idle_hide   (void);
void term_idle_tick   (void);

void term_raw_char    (int col, int row, char c, vga_color_t fg, vga_color_t bg);

uint32_t term_cols(void);
uint32_t term_rows(void);

/* Move the cursor one column left WITHOUT erasing the cell (contrast with
 * term_putc('\b') which both moves and erases).  Used by read_line() for
 * in-place cursor repositioning during line editing. */
void term_cursor_move_left(void);

/* Scroll the terminal upward until the cursor is on the last visible row.
 * Call before printing the shell prompt to keep it pinned to the bottom. */
void term_pin_to_bottom(void);

/* v4: aim the terminal at a different draw_target_t.  Resets cursor and
 * clears the new target's pixels using the current background colour. */
void term_set_target  (draw_target_t *t);
draw_target_t *term_get_target(void);

/* Resize-aware: the terminal already drew into a framebuffer, but the WM
 * just gave us a new (resized) one.  Recompute the grid bounds against
 * the new dimensions and re-rasterise every cached cell.  History
 * survives — no black flash, no wiped scrollback. */
void term_retarget_preserve(draw_target_t *t);

/* Scrollback navigation (TASK 9).  PgUp/PgDn from the shell calls
 * term_scroll_up / term_scroll_down to pan a 256-row ring of historic
 * lines into view.  term_scroll_to_bottom snaps back to the live tail. */
void     term_scroll_up      (int rows);
void     term_scroll_down    (int rows);
void     term_scroll_to_bottom(void);
uint32_t term_scroll_offset  (void);

/* Notification callback fired after every visible state change (printable
 * char, scroll, clear).  Used by the window manager to know when the
 * console window is dirty without us having to depend on window.h here. */
typedef void (*term_dirty_cb_t)(void);
void term_set_dirty_cb(term_dirty_cb_t cb);

/* Optional finer-grained callback: reports the content-relative rect that
 * changed (currently the single idle-spinner cell) so the compositor can
 * repaint just that box instead of the whole screen.  When unset the terminal
 * falls back to the plain dirty callback. */
typedef void (*term_region_dirty_cb_t)(int cx, int cy, int cw, int ch);
void term_set_region_dirty_cb(term_region_dirty_cb_t cb);

/* Output capture for shell pipes and file redirection.
 * Between term_capture_start() and term_capture_stop(), all term_putc /
 * term_puts / term_printf output is written to `buf` (NUL-terminated) and
 * NOT rendered on screen.  Returns bytes written (excluding NUL). */
void     term_capture_start(char *buf, uint32_t sz);
uint32_t term_capture_stop (void);
bool     term_capturing    (void);

#endif /* NEXXON_TERMINAL_H */

