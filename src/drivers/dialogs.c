/* ============================================================================
 * NexxoN OS - Shared modal dialog primitives
 * ----------------------------------------------------------------------------
 * Hosts the three reusable dialogs (input / yes_no / info) so both the
 * file Explorer and the Desktop right-click handlers feed off ONE
 * implementation.  All three were previously private to explorer.c.
 *
 * BUG 2 fix (dialog_yes_no soft-lock on 'Y'):
 *   - Drain the kbd ring buffer at the top of the loop so a queued
 *     keystroke from before the dialog opened cannot starve the prompt
 *     of the user's actual 'Y' press.
 *   - keyboard_clear_abort() up front so a stale Ctrl+C from a previous
 *     dialog doesn't paint our 'Y' as "cancel".
 *   - Re-focus the modal window each iteration so a mouse-driven focus
 *     change cannot redirect input to another window mid-prompt.
 *   - Accept BOTH lowercase and uppercase, AND alternative confirmation
 *     keys (Enter / Space / 'I'/'i' for Hungarian "Igen") so a stuck
 *     modifier on one key (caps-lock, num-lock, weird BIOS) never makes
 *     the dialog unresponsive.
 *   - Echo unrecognised printable keys back to the dialog as a status
 *     line so the user has visible proof their keystrokes are landing.
 *
 * BUG 4 fix (dialog overflow):
 *   - Every dialog measures the widest line of text in pixels and grows
 *     its window width so the text always fits inside the borders, with
 *     16 px of padding on each side.
 *   - dialog_info supports multi-line text natively and grows its height
 *     to match (capped to the screen with a 16 px margin).
 * ============================================================================ */
#include "dialogs.h"
#include "window.h"
#include "gfx.h"
#include "font.h"
#include "vga.h"
#include "string.h"
#include "keyboard.h"
#include "mouse.h"
#include "pnp.h"
#include "i18n.h"
#include "debug.h"
#include "desktop.h"   /* desktop_draw_shortcut_icon() for the choice list */

#define DLG_BG          0xFFE8E8EC
#define DLG_TEXT        0xFF000000
#define DLG_HINT        0xFF505060
#define DLG_DANGER      0xFFB00000
#define DLG_OK          0xFF208030
#define DLG_INPUT_BG    0xFFFFFFFF
#define DLG_BORDER_LO   0xFF202020
#define DLG_BORDER_HI   0xFFFFFFFF
#define DLG_ACCENT      0xFF002878

#define DLG_PAD_X       12
#define DLG_PAD_Y       12
#define DLG_LINE_GAP    6

#define DLG_MIN_W       260
#define DLG_MAX_W       720
#define DLG_MIN_H       110
#define DLG_MAX_H_DELTA 32           /* keep at least this far from screen bottom */

#define INPUT_MAX_CHARS 80

/* ---------- OK / Cancel push-button bar --------------------------------- *
 * Modern buttons that replace the old "Y/Enter  N/Esc" keyboard hints.  The
 * buttons are the primary affordance; Enter (=OK) and Esc (=Cancel) remain
 * as keyboard accelerators.  Clicks arrive through the window content_click
 * callback and set g_dlg_btn_event, which dlg_wait_event() converts into a
 * synthetic DLG_KEY_* code so the dialog loops stay a single switch. */
#define DLG_BTN_W       92
#define DLG_BTN_H       28
#define DLG_BTN_GAP     10
#define DLG_KEY_OK      0x100
#define DLG_KEY_CANCEL  0x101
#define DLG_KEY_REDRAW  0x102

enum { DLG_EV_NONE = 0, DLG_EV_OK, DLG_EV_CANCEL };
static volatile int g_dlg_btn_event   = DLG_EV_NONE;
static bool         g_dlg_two_buttons = true;

static int text_pixels(const char *s) {
    return s ? (int)strlen(s) * FONT_GLYPH_W : 0;
}

/* Pick a dialog width that comfortably fits the widest given line.  All
 * widths are clamped to DLG_MIN_W..DLG_MAX_W. */
static int compute_dialog_width(const char *const *lines, int n_lines,
                                int extra) {
    int w_px = 0;
    for (int i = 0; i < n_lines; i++) {
        int lw = text_pixels(lines[i]);
        if (lw > w_px) w_px = lw;
    }
    if (extra > w_px) w_px = extra;
    int total = w_px + 2 * DLG_PAD_X + 2 * 3 /* WM_BORDER */;
    if (total < DLG_MIN_W) total = DLG_MIN_W;
    if (total > DLG_MAX_W) total = DLG_MAX_W;
    return total;
}

static window_t *dlg_open(const char *title, int w, int h) {
    int sw = (int)vga_width();
    int sh = (int)vga_height();
    if (h > sh - DLG_MAX_H_DELTA) h = sh - DLG_MAX_H_DELTA;
    if (w > sw - 4) w = sw - 4;
    int x = (sw - w) / 2;
    int y = (sh - h) / 2;
    if (y < 4) y = 4;
    if (x < 0) x = 0;
    window_t *win = wm_create_window(x, y, w, h, title);
    if (!win) {
        debug_printf("[dialogs] dlg_open: no window slot for \"%s\"\n",
                     title ? title : "");
        return NULL;
    }
    /* Dialogs are FIXED size - the user can drag them but not resize.
     * Resizing a modal would mean re-laying-out the text every drag
     * step which is more trouble than it's worth. */
    wm_set_resizable(win, false, 0, 0);
    /* Modal dialogs appear AND close INSTANTLY: the fade only advances on
     * compositor frames, but a modal's blocking event loop redraws on input,
     * so with the dirty-rect compositor the open fade can stall mid-way and the
     * close fade can leave a ghost frame.  Skip both for modals. */
    win->anim_open_ms = 0;
    win->no_anim      = true;
    wm_push_modal(win);
    wm_set_focus(win);
    return win;
}

static void dlg_close(window_t *win) {
    if (!win) return;
    wm_pop_modal(win);
    if (win->in_use) wm_destroy_window(win);
}

/* Compute the on-screen rectangles for the OK (and optional Cancel)
 * buttons, all in content-relative coordinates. */
static void dlg_button_layout(const draw_target_t *t, bool two,
                              int *ok_x, int *cancel_x, int *by) {
    *by = (int)t->height - DLG_BTN_H - 12;
    int right = (int)t->width - DLG_PAD_X;
    if (two) {
        *cancel_x = right - DLG_BTN_W;
        *ok_x     = *cancel_x - DLG_BTN_GAP - DLG_BTN_W;
    } else {
        *ok_x     = right - DLG_BTN_W;
        *cancel_x = -1;
    }
}

/* Hover test: convert a content-relative button rect to screen space and
 * compare against the cursor. */
static bool dlg_btn_hot(const window_t *win, int bx, int by) {
    int ax = win->x + WM_BORDER + bx;
    int ay = win->y + WM_BORDER + WM_TITLE_H + 2 + by;
    int mx = mouse_x(), my = mouse_y();
    return mx >= ax && mx < ax + DLG_BTN_W && my >= ay && my < ay + DLG_BTN_H;
}

/* Liquid Glass dialog body: soft vertical frost gradient, a 1 px top
 * highlight, and a faintly tinted button-bar zone with a hairline seam —
 * shared by every modal so popups look identical system-wide. */
static void dlg_paint_body(draw_target_t *t) {
    int h = (int)t->height, w = (int)t->width;
    for (int y = 0; y < h; y++) {
        /* 0xFFF5F7FB -> 0xFFE2E7F0 vertical lerp */
        int num = (h > 1) ? y * 256 / (h - 1) : 0;
        int r = 0xF5 + (0xE2 - 0xF5) * num / 256;
        int g = 0xF7 + (0xE7 - 0xF7) * num / 256;
        int b = 0xFB + (0xF0 - 0xFB) * num / 256;
        gfx_fill_rect(t, 0, y, w, 1,
                      0xFF000000u | ((uint32_t)r << 16) |
                      ((uint32_t)g << 8) | (uint32_t)b);
    }
    gfx_blend_rect(t, 0, 0, w, 1, 0x8CFFFFFFu);              /* top light   */
    int bar = DLG_BTN_H + 24;
    gfx_blend_rect(t, 0, h - bar, w, bar, 0x0F1A2A40u);      /* button zone */
    gfx_blend_rect(t, 0, h - bar, w, 1, 0x24000000u);        /* seam        */
    gfx_blend_rect(t, 0, h - bar + 1, w, 1, 0x66FFFFFFu);
}

static void dlg_draw_one_button(draw_target_t *t, int x, int y,
                                const char *label, bool primary, bool hot) {
    /* Shared Aero widget so dialogs match the rest of the chrome. */
    gfx_draw_button_aero(t, x, y, DLG_BTN_W, DLG_BTN_H, label,
                         primary ? DLG_ACCENT : 0xFF5A6B7Cu, hot);
}

/* Draw OK (primary) plus an optional Cancel button. */
static void dlg_draw_buttons(draw_target_t *t, const window_t *win, bool two) {
    int ok_x, cancel_x, by;
    dlg_button_layout(t, two, &ok_x, &cancel_x, &by);
    if (two)
        dlg_draw_one_button(t, cancel_x, by, L(STR_BTN_CANCEL), false,
                            dlg_btn_hot(win, cancel_x, by));
    dlg_draw_one_button(t, ok_x, by, L(STR_BTN_OK), true,
                        dlg_btn_hot(win, ok_x, by));
}

/* content_click handler shared by every dialog: turns a click on OK or
 * Cancel into a pending button event. */
static bool dlg_buttons_click(window_t *w, int cx, int cy,
                              uint8_t pressed, uint8_t btn) {
    (void)btn;
    if (!(pressed & MOUSE_BTN_LEFT)) return false;
    int ok_x, cancel_x, by;
    dlg_button_layout(&w->content, g_dlg_two_buttons, &ok_x, &cancel_x, &by);
    if (cy < by || cy >= by + DLG_BTN_H) return false;
    if (cx >= ok_x && cx < ok_x + DLG_BTN_W) {
        g_dlg_btn_event = DLG_EV_OK;
        return true;
    }
    if (g_dlg_two_buttons && cx >= cancel_x && cx < cancel_x + DLG_BTN_W) {
        g_dlg_btn_event = DLG_EV_CANCEL;
        return true;
    }
    return false;
}

/* Block until a key is pressed, a button is clicked, the mouse moves (so
 * hover state can repaint), or the window is destroyed externally (X
 * button).  Returns a key byte, one of DLG_KEY_*, or -1 on destroy. */
static int dlg_wait_event(window_t *win) {
    int lmx = mouse_x(), lmy = mouse_y();
    while (1) {
        if (!win->in_use) return -1;
        if (g_dlg_btn_event != DLG_EV_NONE) {
            int e = g_dlg_btn_event;
            g_dlg_btn_event = DLG_EV_NONE;
            return e == DLG_EV_OK ? DLG_KEY_OK : DLG_KEY_CANCEL;
        }
        if (keyboard_has_data()) return keyboard_wait_getc();
        /* Pump the PnP tick: it drives usbhid_poll(), i.e. the NATIVE USB
         * keyboard + mouse.  After the boot-time `usbnative` takeover that is
         * the only thing feeding input, so without this a modal dialog gets no
         * keystrokes or pointer updates and the whole UI appears frozen. */
        pnp_tick();
        wm_tick();
        if (mouse_x() != lmx || mouse_y() != lmy) return DLG_KEY_REDRAW;
        __asm__ volatile ("sti; hlt");
    }
}

/* Render multi-line text starting at (x, y) inside `t`.  Lines wrap on
 * spaces if a single line is wider than `max_px`; returns the next y
 * coordinate so callers can stack widgets below the block. */
static int draw_wrapped_text(draw_target_t *t, int x, int y, int max_px,
                             const char *text, uint32_t fg, uint32_t bg) {
    if (!text || !*text) return y;
    int max_chars = max_px / FONT_GLYPH_W;
    if (max_chars < 1) max_chars = 1;
    const char *p = text;
    while (*p) {
        int n = 0;
        while (p[n] && p[n] != '\n' && n < max_chars) n++;
        /* word-wrap: back off to last space if we ran out mid-word */
        if (p[n] && p[n] != '\n' && n == max_chars) {
            int back = n;
            while (back > 0 && p[back] != ' ') back--;
            if (back > 0) n = back;
        }
        for (int i = 0; i < n; i++) {
            gfx_draw_char(t, x + i * FONT_GLYPH_W, y, p[i], fg, bg);
        }
        y += FONT_GLYPH_H + 2;
        p += n;
        if (*p == ' ' || *p == '\n') p++;
    }
    return y;
}

/* ============================================================================
 * dialog_input
 * ========================================================================== */
bool dialog_input(const char *title, const char *prompt,
                  const char *initial, char *out, size_t out_sz) {
    /* Size the dialog to fit the prompt and the input box, plus the
     * OK/Cancel button bar at the bottom. */
    const char *lines[1] = { prompt ? prompt : "" };
    int extra = INPUT_MAX_CHARS * FONT_GLYPH_W;
    int btn_extra = 2 * DLG_BTN_W + DLG_BTN_GAP + 2 * DLG_PAD_X;
    if (btn_extra > extra) extra = btn_extra;
    int w = compute_dialog_width(lines, 1, extra);
    int h = DLG_PAD_Y + FONT_GLYPH_H + 6 + 22 + 16 + DLG_BTN_H + 12
            + WM_TITLE_H + 2 * WM_BORDER;
    if (h < DLG_MIN_H) h = DLG_MIN_H;

    window_t *win = dlg_open(title, w, h);
    if (!win) return false;

    /* Discard stale input so a queued Enter / typed char cannot auto-
     * answer the prompt (BUG 2 hardening, applied here for consistency). */
    keyboard_drain();
    keyboard_clear_abort();
    g_dlg_two_buttons = true;
    g_dlg_btn_event   = DLG_EV_NONE;
    wm_set_content_click(win, dlg_buttons_click, NULL);

    char buf[80];
    int  max_chars = (int)sizeof(buf) - 1;
    strncpy(buf, initial ? initial : "", (size_t)max_chars);
    buf[max_chars] = 0;
    int n   = (int)strlen(buf);
    int cur = n;

    bool result = false;
    while (1) {
        if (!win->in_use) break;

        draw_target_t *t = &win->content;
        dlg_paint_body(t);

        int y = DLG_PAD_Y;
        if (prompt && *prompt) {
            gfx_draw_string(t, DLG_PAD_X, y, prompt, DLG_TEXT, 0x00000000u);
            y += FONT_GLYPH_H + 6;
        }

        int eb_x = DLG_PAD_X;
        int eb_y = y;
        int eb_w = (int)t->width - 2 * DLG_PAD_X;
        int eb_h = 22;
        gfx_fill_rect(t, eb_x, eb_y, eb_w, eb_h, DLG_INPUT_BG);
        gfx_draw_rect(t, eb_x, eb_y, eb_w, eb_h, DLG_BORDER_LO);
        gfx_draw_hline(t, eb_x + 1, eb_y + 1, eb_w - 2, DLG_BORDER_HI);

        int text_x = eb_x + 4;
        int text_y = eb_y + (eb_h - FONT_GLYPH_H) / 2;
        gfx_draw_string(t, text_x, text_y, buf, DLG_TEXT, DLG_INPUT_BG);
        int caret_x = text_x + cur * FONT_GLYPH_W;
        if (caret_x < eb_x + eb_w - 1) {
            gfx_fill_rect(t, caret_x, text_y - 2, 1, FONT_GLYPH_H + 4, DLG_TEXT);
        }

        dlg_draw_buttons(t, win, true);

        wm_set_focus(win);
        wm_mark_dirty();

        int c = dlg_wait_event(win);
        if (c < 0) break;
        if (c == 0 || c == DLG_KEY_REDRAW) continue;

        if (c == DLG_KEY_OK || c == '\n' || c == '\r') { result = true; break; }
        if (c == DLG_KEY_CANCEL || c == 27 /* Esc */) break;
        if (c == 0x9C /* KEY_CTRL_C */) {
            keyboard_clear_abort();
            break;
        }
        if (c == '\b' /* BACKSPACE */) {
            if (cur > 0) {
                memmove(buf + cur - 1, buf + cur, (size_t)(n - cur));
                n--; cur--;
                buf[n] = 0;
            }
            continue;
        }
        if (c == 0x82 /* KEY_LEFT */)  { if (cur > 0) cur--; continue; }
        if (c == 0x83 /* KEY_RIGHT */) { if (cur < n) cur++; continue; }
        if (c == 0x84 /* KEY_HOME */)  { cur = 0; continue; }
        if (c == 0x85 /* KEY_END */)   { cur = n; continue; }

        /* Printable ASCII or Latin-2.  NXFS valid_name() validates at write. */
        if (((c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF))
            && n + 1 < max_chars) {
            memmove(buf + cur + 1, buf + cur, (size_t)(n - cur));
            buf[cur] = (char)c;
            n++; cur++;
            buf[n] = 0;
        }
    }

    dlg_close(win);

    if (result && out && out_sz > 0) {
        strncpy(out, buf, out_sz - 1);
        out[out_sz - 1] = 0;
    }
    return result;
}

/* ============================================================================
 * dialog_yes_no  (BUG 2 fix lives here)
 * ========================================================================== */
bool dialog_yes_no(const char *title, const char *line1, const char *line2) {
    const char *lines[2] = { line1 ? line1 : "", line2 ? line2 : "" };
    /* Reserve enough width for the two-button bar. */
    int extra = 2 * DLG_BTN_W + DLG_BTN_GAP + 2 * DLG_PAD_X;
    int w = compute_dialog_width(lines, 2, extra);
    int body_h = DLG_PAD_Y + FONT_GLYPH_H + 6;
    if (line2 && *line2) body_h += FONT_GLYPH_H + 8;
    body_h += 16 + DLG_BTN_H + 12;          /* gap + button bar + bottom pad */
    int h = body_h + WM_TITLE_H + 2 * WM_BORDER;
    if (h < DLG_MIN_H) h = DLG_MIN_H;

    window_t *win = dlg_open(title, w, h);
    if (!win) return false;

    /* Start from a clean keyboard state so a stale keystroke from before
     * the dialog opened cannot auto-answer it. */
    keyboard_drain();
    keyboard_clear_abort();
    g_dlg_two_buttons = true;
    g_dlg_btn_event   = DLG_EV_NONE;
    wm_set_content_click(win, dlg_buttons_click, NULL);

    bool answer = false;
    while (1) {
        if (!win->in_use) break;

        draw_target_t *t = &win->content;
        dlg_paint_body(t);

        int y = DLG_PAD_Y;
        gfx_draw_string_aa(t, DLG_PAD_X, y, line1 ? line1 : "",
                           DLG_TEXT, 0x00000000u);
        y += FONT_GLYPH_H + 6;
        if (line2 && *line2) {
            gfx_draw_string_aa(t, DLG_PAD_X, y, line2, DLG_DANGER, 0x00000000u);
            y += FONT_GLYPH_H + 8;
        }

        dlg_draw_buttons(t, win, true);

        /* Re-focus EVERY iteration so any mouse-driven focus change
         * elsewhere cannot redirect keystrokes away from us. */
        wm_set_focus(win);
        wm_mark_dirty();

        int c = dlg_wait_event(win);
        if (c < 0) { answer = false; break; }   /* X button = Cancel */
        if (c == DLG_KEY_REDRAW) continue;       /* hover repaint     */

        /* Normalise: strip a Ctrl modifier, then fold case, so OK/Cancel
         * can also be driven from the keyboard (Enter/Space/Y = OK,
         * Esc/N = Cancel) regardless of caps/num lock. */
        int lc = c;
        if (lc >= 1 && lc <= 26) lc += ('a' - 1);
        if (lc >= 'A' && lc <= 'Z') lc += 32;

        if (c == DLG_KEY_OK || c == '\n' || c == '\r' || c == ' ' ||
            lc == 'y' || lc == 'i') { answer = true;  break; }
        if (c == DLG_KEY_CANCEL || c == 27 || lc == 'n') { answer = false; break; }
        if (c == 0x9C /* Ctrl+C */) { keyboard_clear_abort(); answer = false; break; }
        /* Any other key: ignore and keep waiting. */
    }

    dlg_close(win);
    return answer;
}

/* ============================================================================
 * dialog_info
 * ========================================================================== */
void dialog_info(const char *title, const char *const *lines, int n_lines) {
    if (n_lines < 0) n_lines = 0;
    int extra = DLG_BTN_W + 2 * DLG_PAD_X;
    int w = compute_dialog_width(lines, n_lines, extra);
    int row_h = FONT_GLYPH_H + 4;
    int h = DLG_PAD_Y + n_lines * row_h + 16 + DLG_BTN_H + 12
            + WM_TITLE_H + 2 * WM_BORDER;
    if (h < DLG_MIN_H) h = DLG_MIN_H;

    window_t *win = dlg_open(title, w, h);
    if (!win) return;

    keyboard_drain();
    keyboard_clear_abort();
    g_dlg_two_buttons = false;            /* single OK button */
    g_dlg_btn_event   = DLG_EV_NONE;
    wm_set_content_click(win, dlg_buttons_click, NULL);

    while (1) {
        if (!win->in_use) break;

        draw_target_t *t = &win->content;
        dlg_paint_body(t);

        int max_px = (int)t->width - 2 * DLG_PAD_X;
        int y = DLG_PAD_Y;
        for (int i = 0; i < n_lines; i++) {
            if (!lines[i] || !lines[i][0]) {
                y += row_h;
                continue;
            }
            y = draw_wrapped_text(t, DLG_PAD_X, y, max_px, lines[i],
                                  DLG_TEXT, 0x00000000u);
        }

        dlg_draw_buttons(t, win, false);
        wm_set_focus(win);
        wm_mark_dirty();

        int c = dlg_wait_event(win);
        if (c < 0) break;
        if (c == 0 || c == DLG_KEY_REDRAW) continue;
        if (c == 0x9C) keyboard_clear_abort();
        break;                            /* OK / Enter / Esc / any key closes */
    }

    dlg_close(win);
}

/* ---------- Choice dialog (icon-capable option list) ------------------- *
 * A vertical list of clickable rows, each with a type-aware icon (rendered
 * via desktop_draw_shortcut_icon from labels[i] / icon_cmds[i]) + a label.
 * Returns the clicked index, or -1 on Cancel / Esc / close.  Powers the
 * "New File" type chooser and the "Add Shortcut" app picker. */
#define CHOICE_ROW_H    36
#define CHOICE_ICON_SZ  26
static volatile int g_choice_event = -2;    /* -2 none, -1 cancel, >=0 row */
static int          g_choice_n     = 0;
static int          g_choice_y0    = 0;

static bool choice_click(window_t *w, int cx, int cy, uint8_t pressed, uint8_t btn) {
    (void)btn;
    if (!(pressed & MOUSE_BTN_LEFT)) return false;
    int ok_x, cancel_x, by;
    dlg_button_layout(&w->content, false, &ok_x, &cancel_x, &by);
    if (cy >= by && cy < by + DLG_BTN_H && cx >= ok_x && cx < ok_x + DLG_BTN_W) {
        g_choice_event = -1; g_dlg_btn_event = DLG_EV_OK; return true;
    }
    int rel = cy - g_choice_y0;
    if (rel >= 0 && rel < g_choice_n * CHOICE_ROW_H) {
        g_choice_event = rel / CHOICE_ROW_H;
        g_dlg_btn_event = DLG_EV_OK;       /* wake dlg_wait_event */
        return true;
    }
    return false;
}

int dialog_choice(const char *title, const char *prompt,
                  const char *const *labels, const char *const *icon_cmds, int n) {
    if (n <= 0 || !labels) return -1;
    int prompt_h = (prompt && *prompt) ? (FONT_GLYPH_H + 10) : 6;
    int w = 360;
    int h = WM_TITLE_H + 2 * WM_BORDER + prompt_h + n * CHOICE_ROW_H + DLG_BTN_H + 30;
    window_t *win = dlg_open(title, w, h);
    if (!win) return -1;
    keyboard_drain();
    keyboard_clear_abort();
    g_choice_event    = -2;
    g_choice_n        = n;
    g_dlg_two_buttons = false;             /* single Cancel button */
    g_dlg_btn_event   = DLG_EV_NONE;
    wm_set_content_click(win, choice_click, NULL);

    int result = -1;
    while (1) {
        if (!win->in_use) break;
        draw_target_t *t = &win->content;
        dlg_paint_body(t);
        int y = DLG_PAD_Y;
        if (prompt && *prompt) {
            gfx_draw_string(t, DLG_PAD_X, y, prompt, DLG_TEXT, 0x00000000u);
            y += FONT_GLYPH_H + 10;
        }
        g_choice_y0 = y;
        int ax = win->x + WM_BORDER;
        int ay = win->y + WM_BORDER + WM_TITLE_H + 2;
        int mx = mouse_x(), my = mouse_y();
        int rw = (int)t->width - 2 * DLG_PAD_X;
        for (int i = 0; i < n; i++) {
            int ry = y + i * CHOICE_ROW_H;
            int rh = CHOICE_ROW_H - 4;
            bool hot = (mx >= ax + DLG_PAD_X && mx < ax + DLG_PAD_X + rw &&
                        my >= ay + ry && my < ay + ry + rh);
            gfx_fill_round_rect(t, DLG_PAD_X, ry, rw, rh, 5,
                                hot ? 0xFFDCE8F8u : 0xFFF4F6FAu);
            if (hot) gfx_blend_rect(t, DLG_PAD_X + 3, ry + 1, rw - 6, rh / 2, 0x22FFFFFFu);
            gfx_draw_round_rect(t, DLG_PAD_X, ry, rw, rh, 5, 0xFFC2CAD6u);
            desktop_draw_shortcut_icon(t, DLG_PAD_X + 5,
                                       ry + (rh - CHOICE_ICON_SZ) / 2, CHOICE_ICON_SZ,
                                       labels[i], icon_cmds ? icon_cmds[i] : NULL);
            gfx_draw_string_aa(t, DLG_PAD_X + 5 + CHOICE_ICON_SZ + 12,
                               ry + (rh - FONT_GLYPH_H) / 2,
                               labels[i], DLG_TEXT, 0x00000000u);
        }
        { int ok_x, cancel_x, by;
          dlg_button_layout(t, false, &ok_x, &cancel_x, &by);
          dlg_draw_one_button(t, ok_x, by, L(STR_BTN_CANCEL), false,
                              dlg_btn_hot(win, ok_x, by)); }

        wm_set_focus(win);
        wm_mark_dirty();
        int c = dlg_wait_event(win);
        if (c < 0) { result = -1; break; }
        if (g_choice_event != -2) { result = g_choice_event; break; }
        if (c == 27 || c == DLG_KEY_CANCEL) { result = -1; break; }
    }
    wm_set_content_click(win, NULL, NULL);
    g_dlg_two_buttons = true;
    dlg_close(win);
    return result;
}
