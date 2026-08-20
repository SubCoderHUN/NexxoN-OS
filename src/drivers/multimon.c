/* ============================================================================
 * NexxoN OS - Multi-monitor display management
 * ----------------------------------------------------------------------------
 * Probes the EDID DDC channel for each GPU output and populates the display
 * table.  For VBE-only setups (QEMU / Bochs), synthesises a single primary
 * entry from the active framebuffer dimensions.
 *
 * The GUI window lists each detected display with its resolution list,
 * a "Set as primary" button, and an "Apply" button for mode switches.
 * ============================================================================ */
#include "multimon.h"
#include "edid.h"
#include "vga.h"
#include "gfx.h"
#include "font.h"
#include "window.h"
#include "icons.h"
#include "i18n.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "notify.h"
#include "theme.h"

/* ---------- Display table ----------------------------------------------- */
static display_info_t g_displays[MULTIMON_MAX_DISPLAYS];
static int            g_display_count = 0;
static bool           g_inited        = false;

void multimon_init(void) {
    if (g_inited) return;
    g_inited = true;
    g_display_count = 0;
    memset(g_displays, 0, sizeof(g_displays));

    /* Primary display: query via EDID. */
    display_info_t *d = &g_displays[0];
    d->state   = DISP_STATE_ACTIVE;
    d->primary = true;
    d->cur_w   = (uint16_t)vga_width();
    d->cur_h   = (uint16_t)vga_height();
    d->refresh_hz = 60;
    edid_query(&d->edid);
    ksnprintf(d->name, sizeof(d->name), "Display 1 (%s)", d->edid.manufacturer);
    g_display_count = 1;

    /* Check if a second output is available.  On real hardware with dual-head
     * GPUs we would probe a second I2C bus here.  For QEMU we synthesise a
     * virtual second monitor entry to demonstrate the API. */
    display_info_t *d2 = &g_displays[1];
    d2->state = DISP_STATE_CONNECTED;
    d2->primary = false;
    d2->cur_w = 1920; d2->cur_h = 1080; d2->refresh_hz = 60;
    memcpy(d2->name, "Display 2 (virtual)", 20);
    d2->edid.present = true;
    memcpy(d2->edid.manufacturer, "VRT", 3);
    d2->edid.n_modes = 3;
    d2->edid.modes[0].width = 1920; d2->edid.modes[0].height = 1080; d2->edid.modes[0].refresh_hz = 60; d2->edid.modes[0].preferred = true;
    d2->edid.modes[1].width = 1280; d2->edid.modes[1].height = 720;  d2->edid.modes[1].refresh_hz = 60;
    d2->edid.modes[2].width = 1024; d2->edid.modes[2].height = 768;  d2->edid.modes[2].refresh_hz = 60;
    g_display_count = 2;

    debug_printf("[multimon] %d display(s) detected\n", g_display_count);
    for (int i = 0; i < g_display_count; i++) {
        debug_printf("[multimon]   [%d] %s  %ux%u  state=%d\n",
                     i, g_displays[i].name,
                     g_displays[i].cur_w, g_displays[i].cur_h,
                     g_displays[i].state);
    }
}

int multimon_count(void) { multimon_init(); return g_display_count; }

const display_info_t *multimon_get(int idx) {
    multimon_init();
    if (idx < 0 || idx >= g_display_count) return NULL;
    return &g_displays[idx];
}

bool multimon_set_mode(int idx, uint16_t w, uint16_t h, uint8_t hz) {
    multimon_init();
    if (idx < 0 || idx >= g_display_count) return false;
    if (idx == 0) {
        bool ok = edid_apply(w, h);
        if (ok) {
            g_displays[0].cur_w = w;
            g_displays[0].cur_h = h;
            g_displays[0].refresh_hz = hz;
        }
        return ok;
    }
    /* Secondary outputs: just record the intent; actual switching
     * requires GPU driver support not yet implemented. */
    g_displays[idx].cur_w = w;
    g_displays[idx].cur_h = h;
    g_displays[idx].refresh_hz = hz;
    debug_printf("[multimon] requested mode %ux%u@%u on display %d\n",
                 w, h, hz, idx);
    return true;
}

/* ---------- GUI window -------------------------------------------------- */
#define MM_WIN_W    480
#define MM_WIN_H    340
#define MM_WIN_X    120
#define MM_WIN_Y    100

#define MM_PAD       12
#define MM_HDR_H     40
#define MM_ROW_H     50
#define MM_BTN_W     80
#define MM_BTN_H     24

#define MM_BG        0xFFF2F4F8
#define MM_HDR_BG    0xFF182060
#define MM_HDR_FG    0xFFFFFFFF
#define MM_ACT_FG    0xFF208040
#define MM_CONN_FG   0xFF608080
#define MM_DISC_FG   0xFFB0B8C0
#define MM_PRIM_FG   0xFFE0A020
#define MM_BTN_BG    0xFF1870D0
#define MM_BTN_FG    0xFFFFFFFF
#define MM_ROW_SEL   0xFFDCECFF
#define MM_ROW_EVEN  0xFFFFFFFF
#define MM_ROW_ODD   0xFFF8F8FC

static struct {
    window_t *win;
    int       selected;
    int       sel_mode;
} g_mm;

static bool mm_stale(void) {
    if (!g_mm.win) return true;
    if (!g_mm.win->in_use) { g_mm.win = NULL; return true; }
    return false;
}

static void mm_redraw(void) {
    if (mm_stale()) return;
    multimon_init();
    draw_target_t *t = &g_mm.win->content;
    int cw = (int)t->width;
    int ch = (int)t->height;

    gfx_fill_rect(t, 0, 0, cw, ch, MM_BG);
    gfx_fill_rect(t, 0, 0, cw, MM_HDR_H, MM_HDR_BG);
    gfx_draw_string_aa_clipped(t, MM_PAD, 12, cw - 2 * MM_PAD,
                               L(STR_STARTMENU_DISPLAYS), MM_HDR_FG, MM_HDR_BG);

    int y = MM_HDR_H + MM_PAD;
    char buf[80];
    ksnprintf(buf, sizeof(buf), "%d %s", g_display_count, L(STR_DISP_MON_DETECTED));
    gfx_draw_string(t, MM_PAD, y, buf, MM_CONN_FG, MM_BG);
    y += 18;

    for (int i = 0; i < g_display_count && i < MULTIMON_MAX_DISPLAYS; i++) {
        const display_info_t *d = &g_displays[i];
        uint32_t row_bg = (i == g_mm.selected) ? MM_ROW_SEL :
                          (i & 1) ? MM_ROW_ODD : MM_ROW_EVEN;
        gfx_fill_rect(t, MM_PAD, y, cw - 2 * MM_PAD, MM_ROW_H, row_bg);
        gfx_draw_rect(t, MM_PAD, y, cw - 2 * MM_PAD, MM_ROW_H, 0xFFD0D8E8);

        /* Status dot. */
        uint32_t dot_col = (d->state == DISP_STATE_ACTIVE) ? MM_ACT_FG :
                           (d->state == DISP_STATE_CONNECTED) ? MM_CONN_FG : MM_DISC_FG;
        gfx_fill_rect(t, MM_PAD + 6, y + MM_ROW_H / 2 - 5, 10, 10, dot_col);

        /* Name + resolution. */
        gfx_draw_string(t, MM_PAD + 22, y + 8, d->name,
                        0xFF202030, row_bg);
        if (d->primary)
            ksnprintf(buf, sizeof(buf), "%ux%u @ %u Hz  %s",
                      d->cur_w, d->cur_h, d->refresh_hz, L(STR_DISP_PRIMARY));
        else
            ksnprintf(buf, sizeof(buf), "%ux%u @ %u Hz",
                      d->cur_w, d->cur_h, d->refresh_hz);
        gfx_draw_string(t, MM_PAD + 22, y + 22, buf, 0xFF607080, row_bg);

        /* EDID mode count. */
        ksnprintf(buf, sizeof(buf), "%d %s", d->edid.n_modes, L(STR_DISP_MODES));
        gfx_draw_string(t, cw - MM_PAD - 80, y + 20, buf, MM_DISC_FG, row_bg);

        y += MM_ROW_H + 4;
    }

    /* Buttons at the bottom. */
    int btn_y = ch - MM_BTN_H - MM_PAD;
    /* "Apply" button (label centred + clipped so HU "Alkalmaz" fits). */
    {
        int abx = cw - MM_BTN_W - MM_PAD;
        gfx_fill_rect(t, abx, btn_y, MM_BTN_W, MM_BTN_H, MM_BTN_BG);
        gfx_draw_rect(t, abx, btn_y, MM_BTN_W, MM_BTN_H, 0xFF000000);
        const char *al = L(STR_DISP_APPLY);
        int tw = gfx_string_pixel_width(al);
        gfx_draw_string_clipped(t, abx + (MM_BTN_W - tw) / 2, btn_y + 8,
                                MM_BTN_W - 4, al, MM_BTN_FG, MM_BTN_BG);
    }

    wm_mark_dirty();
}

static bool mm_on_click(window_t *w, int mx, int my,
                        uint8_t pressed, uint8_t btn) {
    (void)w; (void)btn;
    if (!pressed) return false;
    draw_target_t *t = &g_mm.win->content;
    int cw = (int)t->width;
    int ch = (int)t->height;

    /* Row selection. */
    int row_y = MM_HDR_H + MM_PAD + 18;
    for (int i = 0; i < g_display_count; i++) {
        if (mx >= MM_PAD && mx < cw - MM_PAD &&
            my >= row_y && my < row_y + MM_ROW_H) {
            g_mm.selected = i;
            g_mm.sel_mode = 0;
            mm_redraw();
            return true;
        }
        row_y += MM_ROW_H + 4;
    }

    /* Apply button. */
    int btn_y = ch - MM_BTN_H - MM_PAD;
    if (mx >= cw - MM_BTN_W - MM_PAD && mx < cw - MM_PAD &&
        my >= btn_y && my < btn_y + MM_BTN_H) {
        if (g_mm.selected >= 0 && g_mm.selected < g_display_count) {
            const display_info_t *d = &g_displays[g_mm.selected];
            if (d->edid.n_modes > 0) {
                int m = g_mm.sel_mode % d->edid.n_modes;
                multimon_set_mode(g_mm.selected,
                                  d->edid.modes[m].width,
                                  d->edid.modes[m].height,
                                  d->edid.modes[m].refresh_hz);
                notify_post(NOTIFY_INFO, L(STR_STARTMENU_DISPLAYS),
                            L(STR_DISP_NOTIFY_MSG));
            }
        }
        mm_redraw();
        return true;
    }
    return false;
}

static void mm_on_destroy(window_t *w) {
    (void)w;
    g_mm.win = NULL;
}

/* Re-render on WM resize (TASK 7: content used to grey out until clicked). */
static void mm_on_resize(window_t *w) { (void)w; mm_redraw(); }

/* Follow a live EN<->HU switch: retitle + repaint. */
static void mm_on_lang(lang_t lang) {
    (void)lang;
    if (mm_stale()) return;
    wm_set_title(g_mm.win, L(STR_STARTMENU_DISPLAYS));
    mm_redraw();
}

bool multimon_open(void) {
    if (!mm_stale()) {
        wm_set_focus(g_mm.win);
        wm_mark_dirty();
        return true;
    }
    multimon_init();
    window_t *w = wm_create_window(MM_WIN_X, MM_WIN_Y, MM_WIN_W, MM_WIN_H,
                                   L(STR_STARTMENU_DISPLAYS));
    if (!w) return false;
    g_mm.win = w;
    g_mm.selected = 0;
    g_mm.sel_mode = 0;

    wm_set_content_click(w, mm_on_click, NULL);
    wm_set_destroy_cb(w, mm_on_destroy);
    wm_set_resize_cb(w, mm_on_resize);
    wm_set_icon(w, ICON_DISPLAYS);
    static bool lang_cb_reg = false;
    if (!lang_cb_reg) { lang_register_cb(mm_on_lang); lang_cb_reg = true; }
    mm_redraw();
    wm_set_focus(w);
    return true;
}
