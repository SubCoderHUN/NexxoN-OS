/* ============================================================================
 * NexxoN OS - Device Manager (PCI inventory)  (v1.0)
 * ----------------------------------------------------------------------------
 * Window-hosted formatted table of every PCI device the bus walker turns
 * up.  Refreshes on open (the bus is static at runtime); a manual rescan
 * button re-enumerates from scratch.
 *
 * Layout (content rect):
 *   Row 0          Header strip ("Bus:Dev.Fn  Vendor   Class  Description")
 *   Rows 1..N-2    One PCI device per row, scrollable
 *   Row N-1        Status bar: "<n> devices  -  click row to highlight"
 *
 * Singleton.  Re-opening just refocuses + rescans.
 * ============================================================================ */
#include "apps.h"
#include "window.h"
#include "icons.h"
#include "gfx.h"
#include "theme.h"
#include "font.h"
#include "vga.h"
#include "i18n.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "pci.h"

#define DM_W            560
#define DM_H            340
#define DM_MIN_W        420
#define DM_MIN_H        220
#define DM_BG           0xFFF2F5FA
#define DM_HDR_BG       0xFF14203C
#define DM_FG           0xFF25313F
#define DM_FG_HOT       0xFFFFE040
#define DM_SEL_BG       0xFF1E6FE0
#define DM_ROW_H        16
#define DM_BTN_W        66
#define DM_BTN_H        18
#define DM_BTN_BG       0xFF205020
#define DM_BTN_BG_HOT   0xFF308040
#define DM_BTN_FG       0xFFFFFFFF
#define DM_MAX_DEV      96

static window_t    *g_dm_win   = NULL;
static pci_device_t g_dm_devs[DM_MAX_DEV];
static int          g_dm_count = 0;
static int          g_dm_sel   = -1;
static int          g_dm_scroll= 0;

static void dm_redraw(void);

static void dm_rescan(void) {
    g_dm_count = pci_enumerate(g_dm_devs, DM_MAX_DEV);
    debug_printf("[devmgr] rescan: %d PCI device%s\n",
                 g_dm_count, g_dm_count == 1 ? "" : "s");
}

static int dm_visible(draw_target_t *t) {
    int top = 50;
    int bottom = (int)t->height - 24;
    return (bottom - top) / DM_ROW_H;
}

static bool dm_click(window_t *w, int cx, int cy, uint8_t pressed,
                     uint8_t btn) {
    (void)btn;
    if (w != g_dm_win) return false;
    if (!(pressed & MOUSE_BTN_LEFT)) return true;
    /* Rescan button: top-right. */
    int btn_x = (int)w->content.width - DM_BTN_W - 8;
    int btn_y = 4;
    if (cx >= btn_x && cx < btn_x + DM_BTN_W &&
        cy >= btn_y && cy < btn_y + DM_BTN_H) {
        dm_rescan();
        g_dm_sel = -1;
        dm_redraw();
        return true;
    }
    int top = 50;
    int row = (cy - top) / DM_ROW_H;
    if (cy < top || row < 0) { g_dm_sel = -1; dm_redraw(); return true; }
    int idx = g_dm_scroll + row;
    if (idx < 0 || idx >= g_dm_count) idx = -1;
    g_dm_sel = idx;
    dm_redraw();
    return true;
}

static void dm_resize_cb(window_t *w) { (void)w; dm_redraw(); }
static void dm_destroy_cb(window_t *w) { if (w == g_dm_win) g_dm_win = NULL; }

static void dm_redraw(void) {
    if (!g_dm_win || !g_dm_win->in_use) { g_dm_win = NULL; return; }
    draw_target_t *t = &g_dm_win->content;
    gfx_clear(t, DM_BG);

    /* Header banner: glossy glass band. */
    gfx_fill_rect(t, 0, 0, (int)t->width, 44, DM_HDR_BG);
    gfx_blend_rect(t, 0, 0, (int)t->width, 22, 0x1CFFFFFFu);
    gfx_blend_rect(t, 0, 43, (int)t->width, 1, GLASS_EDGE_DARK);
    gfx_draw_string(t, 8, 4, "NexxoN Device Manager",
                    0xFFF2F6FBu, DM_HDR_BG);
    char title[80];
    ksnprintf(title, sizeof(title), "%d PCI device%s discovered",
              g_dm_count, g_dm_count == 1 ? "" : "s");
    gfx_draw_string(t, 8, 4 + FONT_GLYPH_H + 4, title, 0xFFA9BCE8u, DM_HDR_BG);
    gfx_draw_string(t, 8, 4 + 2 * (FONT_GLYPH_H + 2) + 2,
                    "Bus:Dev.Fn  Vendor:Device  Class.Sub.PI  Description",
                    0xFF8FA3C4u, DM_HDR_BG);

    /* Rescan button. */
    int btn_x = (int)t->width - DM_BTN_W - 8;
    int btn_y = 4;
    int mx = mouse_x(), my = mouse_y();
    int abs_x = g_dm_win->x + WM_BORDER + btn_x;
    int abs_y = g_dm_win->y + WM_BORDER + WM_TITLE_H + 2 + btn_y;
    bool hot = (mx >= abs_x && mx < abs_x + DM_BTN_W &&
                my >= abs_y && my < abs_y + DM_BTN_H);
    gfx_draw_button_aero(t, btn_x, btn_y, DM_BTN_W, DM_BTN_H, "Rescan",
                         DM_BTN_BG, hot);

    /* Device rows. */
    int visible = dm_visible(t);
    if (g_dm_scroll < 0) g_dm_scroll = 0;
    int max_scroll = g_dm_count - visible;
    if (max_scroll < 0) max_scroll = 0;
    if (g_dm_scroll > max_scroll) g_dm_scroll = max_scroll;

    int top = 50;
    for (int i = 0; i < visible && g_dm_scroll + i < g_dm_count; i++) {
        int idx = g_dm_scroll + i;
        const pci_device_t *d = &g_dm_devs[idx];
        int row_y = top + i * DM_ROW_H;
        bool selected = (idx == g_dm_sel);
        if (selected) {
            gfx_fill_rect(t, 0, row_y, (int)t->width, DM_ROW_H, DM_SEL_BG);
        }
        char line[160];
        ksnprintf(line, sizeof(line),
                  "%02x:%02x.%x   %04x:%04x   %02x.%02x.%02x   %s",
                  d->bus, d->device, d->function,
                  d->vendor_id, d->device_id,
                  d->class_code, d->subclass, d->prog_if,
                  pci_class_name(d->class_code, d->subclass, d->prog_if));
        int max_chars = ((int)t->width - 16) / FONT_GLYPH_W;
        if (max_chars > 0 && (int)strlen(line) > max_chars) {
            line[max_chars - 1] = '.';
            line[max_chars - 2] = '.';
            line[max_chars]     = 0;
        }
        gfx_draw_string(t, 8, row_y + (DM_ROW_H - FONT_GLYPH_H) / 2,
                        line, selected ? 0xFFFFFFFFu : DM_FG,
                        selected ? DM_SEL_BG : DM_BG);
    }

    /* Status row. */
    int sy = (int)t->height - 20;
    gfx_fill_rect(t, 0, sy, (int)t->width, 20, DM_HDR_BG);
    char status[80];
    if (g_dm_sel >= 0 && g_dm_sel < g_dm_count) {
        const pci_device_t *d = &g_dm_devs[g_dm_sel];
        ksnprintf(status, sizeof(status),
                  "Selected: %02x:%02x.%x  BAR0=0x%08x  BAR5=0x%08x",
                  d->bus, d->device, d->function, d->bar[0], d->bar[5]);
    } else if (g_dm_count > visible) {
        ksnprintf(status, sizeof(status),
                  "  showing %d-%d of %d",
                  g_dm_scroll + 1,
                  g_dm_scroll + visible > g_dm_count ? g_dm_count
                                                     : g_dm_scroll + visible,
                  g_dm_count);
    } else {
        ksnprintf(status, sizeof(status),
                  "%d device%s - click row to inspect, [Rescan] re-enumerates",
                  g_dm_count, g_dm_count == 1 ? "" : "s");
    }
    int max_chars = ((int)t->width - 16) / FONT_GLYPH_W;
    if (max_chars > 0 && (int)strlen(status) > max_chars) {
        status[max_chars - 1] = '.';
        status[max_chars - 2] = '.';
        status[max_chars]     = 0;
    }
    gfx_draw_string(t, 8, sy + (20 - FONT_GLYPH_H) / 2,
                    status, 0xFFC7D4E8u, DM_HDR_BG);
    wm_mark_dirty();
}

bool devmgr_open(void) {
    if (g_dm_win && g_dm_win->in_use) {
        wm_set_focus(g_dm_win);
        dm_rescan();
        dm_redraw();
        return true;
    }
    g_dm_win = wm_create_window(80, 60, DM_W, DM_H, L(STR_APP_DEVMGR));
    if (!g_dm_win) return false;
    wm_set_content_click(g_dm_win, dm_click, NULL);
    wm_set_resizable(g_dm_win, true, DM_MIN_W, DM_MIN_H);
    wm_set_resize_cb(g_dm_win, dm_resize_cb);
    wm_set_destroy_cb(g_dm_win, dm_destroy_cb);
    wm_set_icon(g_dm_win, ICON_DEVMGR);
    g_dm_sel = -1;
    g_dm_scroll = 0;
    dm_rescan();
    dm_redraw();
    return true;
}
