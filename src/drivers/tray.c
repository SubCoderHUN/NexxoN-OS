/* ============================================================================
 * NexxoN OS - System tray  (v2.0)
 * ----------------------------------------------------------------------------
 * Layout overhaul: every applet now reserves a non-overlapping bounding
 * box anchored to the right edge of the taskbar.  Order, right-to-left:
 *
 *     [ network ] [ notify ] [ ... gap ... ] [ horizontal volume slider ]
 *
 * The clock + date and calendar popup are owned by desktop.c and sit
 * even further left.  desktop.c calls tray_cluster_left() to know
 * where the cluster ends so it never positions the clock under us.
 *
 * The volume slider is a true draggable control: WM mouse-move events
 * are forwarded to tray_handle_drag() while the left button is held;
 * if the cursor lands inside the track, the master volume is set
 * proportionally and the audio mixer (audio_set_volume) sees it
 * immediately.  Click-with-no-drag toggles mute via tray_handle_click.
 *
 * Both the slider and the icons paint their own background plaques so
 * they sit cleanly on the gradient panel without ever touching the
 * clock plaque or the network bars.
 * ============================================================================ */
#include "tray.h"
#include "gfx.h"
#include "font.h"
#include "speaker.h"
#include "audio.h"
#include "e1000.h"
#include "net.h"
#include "netif.h"
#include "wifi.h"
#include "notify.h"
#include "theme.h"
#include "string.h"
#include "desktop.h"
#include "i18n.h"
#include "debug.h"

/* #11: small network-info popup toggled by clicking the tray network icon
 * (mirrors the calendar popup pattern).  Shows the connection type
 * (Wired / Wi-Fi) and the current IPv4 address. */
static bool g_netinfo_open = false;
bool tray_netinfo_open(void) { return g_netinfo_open; }
void tray_netinfo_close(void) { g_netinfo_open = false; }

/* ---- Right-anchored applet ordering (right-most index first) -------- */
enum { TR_NETWORK, TR_NOTIFY, TR_COUNT };

/* X coordinate of applet `idx` on the panel.  Index 0 is right-most. */
static int slot_x(int idx, int screen_w) {
    return screen_w - TRAY_MARGIN - (idx + 1) * TRAY_STRIDE;
}

/* Public geometry probes - used by desktop.c so the clock plaque can be
 * positioned to the LEFT of the slider, which itself sits to the LEFT
 * of the icon cluster.  All coordinates are absolute on the panel. */
int tray_cluster_right(int screen_w) { return screen_w - TRAY_MARGIN; }
int tray_cluster_left (int screen_w) {
    return slot_x(TR_COUNT - 1, screen_w);
}

int tray_volume_x(int screen_w) {
    /* Park the slider 8 px left of the icon cluster so its plaque
     * never touches the network bars. */
    return tray_cluster_left(screen_w) - TRAY_VOL_TOTAL_W - 8;
}

int  tray_volume_get(void)        { return audio_get_volume(); }
void tray_volume_set(int v)       { audio_set_volume(v); }
void tray_volume_mute(bool m)     { audio_set_muted(m); }
bool tray_volume_muted(void)      { return audio_is_muted(); }

/* ---- Volume widget --------------------------------------------------- */
/* Liquid Glass applet plaque: rounded frost tile with a light bevel —
 * replaces the old dark boxes with hard black borders. */
static void tray_glass_plaque(draw_target_t *t, int x, int y, int w, int h) {
    gfx_blend_round_rect(t, x, y, w, h, 4, 0x2CFFFFFFu);
    gfx_blend_round_rect(t, x + 1, y + 1, w - 2, h / 2 - 1, 4, 0x22FFFFFFu);
    gfx_draw_round_rect(t, x, y, w, h, 4, 0x52FFFFFFu);
}

static void draw_volume_slider(draw_target_t *t, int x, int y) {
    int vol = tray_volume_get();
    bool muted = tray_volume_muted();

    /* Plaque background.  Slight gradient feel with a dark base.  We
     * deliberately do NOT paint a giant rectangle that covers the
     * speaker glyph + slider as a single block - each sub-element
     * paints its own background so the panel gradient peeks through
     * between them. */
    int plaque_w = TRAY_VOL_TOTAL_W;
    int plaque_h = TRAY_VOL_SLIDER_H + 6;
    int plaque_y = y - 3;
    tray_glass_plaque(t, x - 2, plaque_y, plaque_w + 4, plaque_h);

    /* Speaker glyph (left of slider). */
    int sg_x = x;
    int sg_y = y - 1;
    uint32_t glyph_fg = muted ? 0xFFFF6060 : 0xFFBBD4F0;   /* glass blue */
    /* Stylised speaker cone. */
    gfx_fill_rect(t, sg_x,     sg_y + 4, 3, 4, glyph_fg);
    gfx_fill_rect(t, sg_x + 3, sg_y + 2, 2, 8, glyph_fg);
    gfx_fill_rect(t, sg_x + 5, sg_y + 1, 2, 10, glyph_fg);
    if (!muted) {
        /* Two sound arcs. */
        if (vol > 33) {
            gfx_putpixel(t, sg_x + 9,  sg_y + 4, glyph_fg);
            gfx_putpixel(t, sg_x + 9,  sg_y + 5, glyph_fg);
            gfx_putpixel(t, sg_x + 9,  sg_y + 6, glyph_fg);
            gfx_putpixel(t, sg_x + 9,  sg_y + 7, glyph_fg);
        }
        if (vol > 66) {
            gfx_putpixel(t, sg_x + 11, sg_y + 3, glyph_fg);
            gfx_putpixel(t, sg_x + 11, sg_y + 4, glyph_fg);
            gfx_putpixel(t, sg_x + 11, sg_y + 7, glyph_fg);
            gfx_putpixel(t, sg_x + 11, sg_y + 8, glyph_fg);
        }
    } else {
        /* Red diagonal slash. */
        for (int i = 0; i < 10; i++) {
            gfx_putpixel(t, sg_x + i + 1, sg_y + i,     0xFFC02020);
            gfx_putpixel(t, sg_x + i + 1, sg_y + i + 1, 0xFFC02020);
        }
    }

    /* Slider: shared Aero glass track + glossy round thumb (one look for every
     * slider in the system).  Geometry (tr_x / TRAY_VOL_SLIDER_W) is unchanged
     * so the drag hit-test in tray_handle_drag still maps correctly. */
    int tr_x = x + TRAY_VOL_GLYPH_W;
    int sl_h = TRAY_VOL_SLIDER_H + 4;
    int sl_y = y + 1 + (TRAY_VOL_SLIDER_H - 2 - sl_h) / 2;
    /* One consistent Liquid Glass accent (matches the Settings slider). */
    uint32_t accent = muted ? 0xFF8890A0u : GLASS_ACCENT;
    gfx_draw_slider_aero(t, tr_x, sl_y, TRAY_VOL_SLIDER_W, sl_h, vol * 10, accent);
}

/* ---- Network icon ---------------------------------------------------- */
static void draw_network(draw_target_t *t, int x, int y) {
    bool up = e1000_present();
    if (up) {
        net_config_t c;
        net_get_config(&c);
        if (c.ip == 0) up = false;
    }
    /* Glass plaque keeps the bars from sitting directly on the gradient. */
    tray_glass_plaque(t, x - 2, y - 2, TRAY_ICON_W + 4, TRAY_ICON_H + 4);

    /* Bars rising left to right; connected = glass blue ramp. */
    for (int b = 0; b < 4; b++) {
        int bh = 3 + b * 3;
        uint32_t fg = up ? (0xFF5F9BE0u + (uint32_t)(b * 0x000A0A08u))
                         : 0xFF7C8896u;
        gfx_fill_rect(t, x + 1 + b * 4, y + 14 - bh, 3, bh, fg);
        gfx_blend_rect(t, x + 1 + b * 4, y + 14 - bh, 3, 1, 0x60FFFFFFu);
    }
    if (!up) {
        for (int i = 0; i < 14; i++) {
            gfx_putpixel(t, x + 1 + i, y + 2 + i, 0xFFC02020);
        }
    }
}

/* ---- Notify icon ----------------------------------------------------- */
static void draw_notify(draw_target_t *t, int x, int y) {
    int n = notify_count();
    /* Glass plaque keeps the bell from sitting on the gradient. */
    tray_glass_plaque(t, x - 2, y - 2, TRAY_ICON_W + 4, TRAY_ICON_H + 4);

    uint32_t fg = n > 0 ? 0xFFEAF3FFu : 0xFFB9CDE6u;      /* glass blues */
    /* Bell: rounded cup + lip + clapper. */
    gfx_fill_round_rect(t, x + 4, y + 3, 8, 8, 3, fg);
    gfx_fill_round_rect(t, x + 2, y + 10, 12, 2, 1, fg);
    gfx_fill_circle(t, x + 8, y + 14, 1, fg);
    if (n > 0) {
        /* Red unread-count badge (round). */
        gfx_fill_circle(t, x + 13, y + 4, 4, 0xFFD03030);
        char buf[2] = { (char)('0' + (n > 9 ? 9 : n)), 0 };
        gfx_draw_string(t, x + 10, y + 1, buf, 0xFFFFFFFF, 0x00000000);
    }
}

/* ---- #11: network-info popup ----------------------------------------- */
static void draw_netinfo_popup(draw_target_t *t, int screen_w, int panel_top) {
    bool hu = (i18n_get_language() == LANG_HU);

    net_config_t c;
    net_get_config(&c);
    bool up = e1000_present() && c.ip != 0;

    char ssid[40];
    wifi_current_ssid(ssid, sizeof(ssid));
    bool on_wifi = (ssid[0] != 0);

    const int W = 236, H = 92;
    int x = screen_w - W - TRAY_MARGIN;
    if (x < 4) x = 4;
    int y = panel_top - H - 6;

    /* Dark frosted-glass popup: soft shadow + blur backdrop + frost + bevel. */
    gfx_drop_shadow_round(t, x, y, W, H, RADIUS_PANEL, 8);
    gfx_glass_panel(t, x, y, W, H, RADIUS_PANEL, GLASS_POPUP_DARK);
    gfx_blend_rect(t, x + 1, y + 1, W - 2, 22, 0xE01850C8u);   /* blue header */
    gfx_blend_rect(t, x + 2, y + 1, W - 4, 1, GLASS_GLOSS);
    gfx_glass_bevel(t, x, y, W, H, RADIUS_PANEL, GLASS_EDGE_LIGHT, GLASS_EDGE_DARK);

    const char *title = hu ? "Hálózat" : "Network";
    gfx_draw_string(t, x + 10, y + 7, title, 0xFFFFFFFF, 0x00000000u);

    /* Connection-type line. */
    char line[80];
    if (on_wifi) {
        ksnprintf(line, sizeof(line), "%s: %s", hu ? "Wi-Fi" : "Wi-Fi", ssid);
    } else if (up) {
        ksnprintf(line, sizeof(line), "%s: %s",
                  hu ? "Kapcsolat" : "Connection",
                  hu ? "Vezetékes" : "Wired");
    } else {
        ksnprintf(line, sizeof(line), "%s",
                  hu ? "Nincs kapcsolat" : "Not connected");
    }
    gfx_draw_string(t, x + 10, y + 34, line, 0xFFE0E0E8, 0x00000000u);

    /* IPv4 address line. */
    char ipbuf[20];
    net_ip_ntoa(c.ip, ipbuf, sizeof(ipbuf));
    ksnprintf(line, sizeof(line), "%s: %s",
              hu ? "IP-cím" : "IP address",
              c.ip ? ipbuf : (hu ? "—" : "—"));
    gfx_draw_string(t, x + 10, y + 56, line, 0xFFC8D0E0, 0x00000000u);
}

void tray_draw(draw_target_t *t, int taskbar_top, int screen_w) {
    /* Centre the applets in the full DESKTOP_PANEL_H (40 px) bar.  The
     * old maths divided by 30 — the legacy panel height — which pushed
     * every glyph below the taskbar and clipped the bottom edge. */
    int icon_y = taskbar_top + (DESKTOP_PANEL_H - TRAY_ICON_H) / 2;

    /* Volume slider on the far left of the cluster. */
    int vol_y = taskbar_top + (DESKTOP_PANEL_H - TRAY_VOL_SLIDER_H) / 2;
    draw_volume_slider(t, tray_volume_x(screen_w), vol_y);

    /* Icon cluster (right-anchored). */
    draw_network(t, slot_x(TR_NETWORK, screen_w), icon_y);
    draw_notify (t, slot_x(TR_NOTIFY,  screen_w), icon_y);

    if (g_netinfo_open) draw_netinfo_popup(t, screen_w, taskbar_top);
    (void)theme_is_dark;
}

/* ---- Click / drag dispatch ------------------------------------------ */
static bool point_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

bool tray_handle_click(int sx, int sy, bool right_btn) {
    (void)right_btn;
    extern uint32_t vga_width(void);
    extern uint32_t vga_height(void);
    int sw = (int)vga_width();
    int sh = (int)vga_height();
    int panel_top = sh - DESKTOP_PANEL_H;
    int icon_y = panel_top + (DESKTOP_PANEL_H - TRAY_ICON_H) / 2;

    /* Volume slider hit-test - clicking the track jumps to that
     * position (in addition to drag tracking via tray_handle_drag). */
    int vol_y = panel_top + (DESKTOP_PANEL_H - TRAY_VOL_SLIDER_H) / 2;
    int vol_x = tray_volume_x(sw);
    int tr_x  = vol_x + TRAY_VOL_GLYPH_W;
    int tr_w  = TRAY_VOL_SLIDER_W;
    int tr_h  = TRAY_VOL_SLIDER_H - 2;
    int tr_y  = vol_y + 1;
    if (point_in(sx, sy, vol_x, vol_y - 3, TRAY_VOL_TOTAL_W, TRAY_VOL_SLIDER_H + 6)) {
        if (point_in(sx, sy, tr_x, tr_y, tr_w, tr_h)) {
            /* Track click: set proportional volume. */
            int rel = sx - (tr_x + 1);
            if (rel < 0) rel = 0;
            if (rel > tr_w - 2) rel = tr_w - 2;
            int v = (rel * 100) / (tr_w - 2);
            tray_volume_set(v);
            tray_volume_mute(false);
            return true;
        }
        /* Clicked on the speaker glyph zone → toggle mute. */
        tray_volume_mute(!tray_volume_muted());
        return true;
    }

    /* Network icon click - toggle the network-info popup (#11). */
    int nx = slot_x(TR_NETWORK, sw);
    if (point_in(sx, sy, nx - 2, icon_y - 2, TRAY_ICON_W + 4, TRAY_ICON_H + 4)) {
        g_netinfo_open = !g_netinfo_open;
        return true;
    }
    /* A click anywhere else in the tray dismisses the popup. */
    g_netinfo_open = false;
    /* Notify bell - clears toasts. */
    int bx = slot_x(TR_NOTIFY, sw);
    if (point_in(sx, sy, bx - 2, icon_y - 2, TRAY_ICON_W + 4, TRAY_ICON_H + 4)) {
        notify_init();
        return true;
    }
    return false;
}

bool tray_handle_drag(int sx, int sy, bool left_btn) {
    if (!left_btn) return false;
    extern uint32_t vga_width(void);
    extern uint32_t vga_height(void);
    int sw = (int)vga_width();
    int sh = (int)vga_height();
    int panel_top = sh - DESKTOP_PANEL_H;
    int vol_y = panel_top + (DESKTOP_PANEL_H - TRAY_VOL_SLIDER_H) / 2;
    int vol_x = tray_volume_x(sw);
    int tr_x  = vol_x + TRAY_VOL_GLYPH_W;
    int tr_w  = TRAY_VOL_SLIDER_W;
    int tr_h  = TRAY_VOL_SLIDER_H - 2;
    int tr_y  = vol_y + 1;
    /* Generous vertical hitbox so the user can drag slightly off-axis
     * without losing the slider. */
    if (sy < tr_y - 6 || sy > tr_y + tr_h + 6) return false;
    if (sx < tr_x - 4 || sx > tr_x + tr_w + 4) return false;
    int rel = sx - (tr_x + 1);
    if (rel < 0) rel = 0;
    if (rel > tr_w - 2) rel = tr_w - 2;
    int v = (rel * 100) / (tr_w - 2);
    tray_volume_set(v);
    if (v > 0) tray_volume_mute(false);
    return true;
}

void tray_handle_scroll(int dz) {
    /* Scroll over the tray adjusts master volume. */
    int v = tray_volume_get();
    v += dz * 5;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    tray_volume_set(v);
}
