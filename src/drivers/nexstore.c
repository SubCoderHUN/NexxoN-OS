/* ============================================================================
 * NexxoN OS - NexxStore (graphical package manager front-end)
 * ----------------------------------------------------------------------------
 * A WM-hosted window that wraps the nxpkg package manager in a storefront
 * UI.  Shows available and installed packages, with Install / Uninstall
 * buttons.  Network-dependent: fetches the package index from the nxpkg
 * registry via HTTP when opened.
 * ============================================================================ */
#include "nexstore.h"
#include "nxpkg.h"
#include "window.h"
#include "icons.h"
#include "gfx.h"
#include "theme.h"
#include "i18n.h"
#include "font.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "pit.h"
#include "dialogs.h"
#include "net.h"
#include "keyboard.h"

/* ---------- Layout ------------------------------------------------------- */
#define NS_WIN_X      120
#define NS_WIN_Y       60
#define NS_WIN_W      540
#define NS_WIN_H      400
#define NS_MIN_W      400
#define NS_MIN_H      300

#define NS_HEADER_H    40
#define NS_SEARCH_H    28
#define NS_STATUS_H    22
#define NS_ROW_H       44
#define NS_BTN_W       80
#define NS_BTN_H       22
#define NS_PAD          8

/* ---------- Colours ------------------------------------------------------ */
#define NS_BG          0xFFF0F4FA
#define NS_HDR_BG      0xFF1860C8
#define NS_HDR_FG      0xFFFFFFFF
#define NS_SEARCH_BG   0xFFFFFFFF
#define NS_SEARCH_FG   0xFF202020
#define NS_SEARCH_BD   0xFF6080A0
#define NS_ROW_BG      0xFFFFFFFF
#define NS_ROW_HOT     0xFFD0E4F8
#define NS_ROW_FG      0xFF202020
#define NS_ROW_SUB     0xFF606870
#define NS_ROW_DIV     0xFFD8DCE0
#define NS_BTN_INST_BG 0xFF1880E0
#define NS_BTN_INST_FG 0xFFFFFFFF
#define NS_BTN_UNI_BG  0xFFD03030
#define NS_BTN_UNI_FG  0xFFFFFFFF
#define NS_BTN_DIS_BG  0xFFB0B8C0
#define NS_BTN_DIS_FG  0xFFFFFFFF
#define NS_STATUS_BG   0xFFE0E4EA
#define NS_STATUS_FG   0xFF404048

/* ---------- Package list ------------------------------------------------- */
#define NS_MAX_PKGS    32

typedef struct {
    char     name[NXPKG_NAME_MAX];
    char     version[16];
    char     publisher[32];
    bool     installed;
} ns_pkg_t;

static struct {
    window_t *win;
    ns_pkg_t  pkgs[NS_MAX_PKGS];
    int       pkg_count;
    int       scroll_top;
    int       selected;
    bool      loaded;
    bool      no_net;            /* true = skipped fetch, no link present  */
    char      search[64];
    int       search_caret;
    bool      search_focus;
} g_ns = { 0 };

/* ---------- Helpers ------------------------------------------------------ */
static bool stale(void) {
    if (!g_ns.win) return true;
    if (!g_ns.win->in_use) { g_ns.win = NULL; return true; }
    return false;
}

static void parse_index(const char *raw, int len) {
    g_ns.pkg_count = 0;
    if (len <= 0 || !raw) return;

    const char *p = raw;
    const char *end = raw + len;
    while (p < end && g_ns.pkg_count < NS_MAX_PKGS) {
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (p >= end) break;

        ns_pkg_t *pk = &g_ns.pkgs[g_ns.pkg_count];
        memset(pk, 0, sizeof(*pk));

        int ni = 0;
        while (p < end && *p != '|' && *p != '\n' && ni < NXPKG_NAME_MAX - 1)
            pk->name[ni++] = *p++;
        pk->name[ni] = 0;
        if (p < end && *p == '|') p++;

        int vi = 0;
        while (p < end && *p != '|' && *p != '\n' && vi < 15)
            pk->version[vi++] = *p++;
        pk->version[vi] = 0;
        if (p < end && *p == '|') p++;

        int pi = 0;
        while (p < end && *p != '|' && *p != '\n' && pi < 31)
            pk->publisher[pi++] = *p++;
        pk->publisher[pi] = 0;

        while (p < end && *p != '\n') p++;
        if (pk->name[0]) g_ns.pkg_count++;
    }
    debug_printf("[nexstore] parsed %d packages from index\n", g_ns.pkg_count);
}

static void refresh_list(void) {
    g_ns.pkg_count = 0;
    /* No carrier => do NOT touch the network.  Synchronous fetches freeze
     * the foreground until they time out, so when there is demonstrably no
     * link we skip straight to the "no network" state instead of blocking
     * (this was the "opening NexxStore halts the system" bug). */
    if (!net_link_up()) {
        g_ns.no_net = true;
        g_ns.loaded = true;
        return;
    }
    g_ns.no_net = false;

    char buf[4096];
    int n = nxpkg_search("", buf, sizeof(buf));
    if (n > 0) {
        parse_index(buf, n);
        g_ns.loaded = true;
    } else {
        g_ns.pkg_count = 0;
        g_ns.loaded = true;
    }

    char installed[2048];
    int ilen = nxpkg_list_installed(installed, sizeof(installed));
    if (ilen > 0) {
        for (int i = 0; i < g_ns.pkg_count; i++) {
            g_ns.pkgs[i].installed = false;
            const char *s = installed;
            while (*s) {
                while (*s == '\n' || *s == '\r') s++;
                if (!*s) break;
                const char *line = s;
                while (*s && *s != '\n') s++;
                int llen = (int)(s - line);
                if (llen > 0 && (int)strlen(g_ns.pkgs[i].name) == llen
                    && memcmp(g_ns.pkgs[i].name, line, llen) == 0) {
                    g_ns.pkgs[i].installed = true;
                }
            }
        }
    }
}

static bool contains(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    int nlen = (int)strlen(needle);
    int hlen = (int)strlen(hay);
    for (int i = 0; i <= hlen - nlen; i++) {
        bool ok = true;
        for (int j = 0; j < nlen && ok; j++) {
            char a = hay[i+j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) ok = false;
        }
        if (ok) return true;
    }
    return false;
}

static bool matches_search(const ns_pkg_t *pk) {
    if (!g_ns.search[0]) return true;
    if (contains(pk->name, g_ns.search)) return true;
    if (contains(pk->publisher, g_ns.search)) return true;
    return false;
}

/* ---------- Drawing ------------------------------------------------------ */
static void draw_header(draw_target_t *t) {
    gfx_fill_rect(t, 0, 0, (int)t->width, NS_HEADER_H, NS_HDR_BG);
    gfx_blend_rect(t, 0, 0, (int)t->width, NS_HEADER_H / 2, 0x20FFFFFFu);
    gfx_blend_rect(t, 0, NS_HEADER_H - 1, (int)t->width, 1, GLASS_EDGE_DARK);
    gfx_draw_string(t, NS_PAD, (NS_HEADER_H - FONT_GLYPH_H) / 2,
                    L(STR_APP_NEXSTORE), NS_HDR_FG, NS_HDR_BG);

    int bw = NS_BTN_W;
    int bx = (int)t->width - bw - NS_PAD;
    int by = (NS_HEADER_H - NS_BTN_H) / 2;
    gfx_draw_button_aero(t, bx, by, bw, NS_BTN_H, L(STR_BTN_REFRESH),
                         0xFF2070D0u, false);
}

static void draw_search(draw_target_t *t) {
    int sy = NS_HEADER_H + 4;
    int sw = (int)t->width - NS_PAD * 2;
    gfx_fill_rect(t, NS_PAD, sy, sw, NS_SEARCH_H, NS_SEARCH_BG);
    gfx_draw_rect(t, NS_PAD, sy, sw, NS_SEARCH_H,
                  g_ns.search_focus ? NS_HDR_BG : NS_SEARCH_BD);

    const char *txt = g_ns.search[0] ? g_ns.search : L(STR_NXST_SEARCH);
    uint32_t fg = g_ns.search[0] ? NS_SEARCH_FG : 0xFF909098;
    gfx_draw_string_clipped(t, NS_PAD + 6,
                            sy + (NS_SEARCH_H - FONT_GLYPH_H) / 2,
                            sw - 12, txt, fg, NS_SEARCH_BG);
    if (g_ns.search_focus) {
        int cx = NS_PAD + 6 + g_ns.search_caret * FONT_GLYPH_W;
        gfx_fill_rect(t, cx, sy + 4, 1, NS_SEARCH_H - 8, NS_SEARCH_FG);
    }
}

static void draw_pkg_row(draw_target_t *t, int x, int y, int w,
                         const ns_pkg_t *pk, bool hot, bool sel) {
    uint32_t bg = sel ? 0xFF90C0F0 : (hot ? NS_ROW_HOT : NS_ROW_BG);
    gfx_fill_rect(t, x, y, w, NS_ROW_H, bg);
    gfx_draw_hline(t, x, y + NS_ROW_H - 1, w, NS_ROW_DIV);

    /* Icon plaque */
    gfx_fill_rect(t, x + 6, y + 8, 28, 28, NS_HDR_BG);
    gfx_draw_rect(t, x + 6, y + 8, 28, 28, 0xFF102060);
    char ini[2] = { pk->name[0], 0 };
    if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] -= 32;
    gfx_draw_string(t, x + 16, y + 16, ini, 0xFFFFFFFF, NS_HDR_BG);

    /* Name + version */
    char title[80];
    ksnprintf(title, sizeof(title), "%s  v%s", pk->name, pk->version);
    gfx_draw_string_clipped(t, x + 42, y + 8, w - 42 - NS_BTN_W - 16,
                            title, NS_ROW_FG, bg);

    /* Publisher */
    gfx_draw_string_clipped(t, x + 42, y + 24, w - 42 - NS_BTN_W - 16,
                            pk->publisher, NS_ROW_SUB, bg);

    /* Install / Uninstall button */
    int bx = x + w - NS_BTN_W - NS_PAD;
    int by = y + (NS_ROW_H - NS_BTN_H) / 2;
    uint32_t bbg, bfg;
    const char *blbl;
    if (pk->installed) {
        bbg = NS_BTN_UNI_BG; bfg = NS_BTN_UNI_FG;
        blbl = L(STR_NXST_UNINSTALL);
    } else {
        bbg = NS_BTN_INST_BG; bfg = NS_BTN_INST_FG;
        blbl = L(STR_NXST_INSTALL);
    }
    gfx_fill_rect(t, bx, by, NS_BTN_W, NS_BTN_H, bbg);
    gfx_draw_rect(t, bx, by, NS_BTN_W, NS_BTN_H, 0xFF102040);
    int tw = gfx_string_pixel_width(blbl);
    gfx_draw_string(t, bx + (NS_BTN_W - tw)/2,
                    by + (NS_BTN_H - FONT_GLYPH_H)/2, blbl, bfg, bbg);
}

static int list_top_y(void) {
    return NS_HEADER_H + 4 + NS_SEARCH_H + 6;
}

static int visible_count(const draw_target_t *t) {
    int avail = (int)t->height - list_top_y() - NS_STATUS_H;
    return avail / NS_ROW_H;
}

static void draw_list(draw_target_t *t) {
    int ty = list_top_y();
    int avail = (int)t->height - ty - NS_STATUS_H;
    int vis = avail / NS_ROW_H;
    int lw = (int)t->width;

    gfx_fill_rect(t, 0, ty, lw, avail, NS_BG);

    int shown = 0, idx = 0;
    int mx = mouse_x(), my = mouse_y();
    int abs_x = g_ns.win->x + WM_BORDER;
    int abs_y = g_ns.win->y + WM_BORDER + WM_TITLE_H + 2;

    for (int i = 0; i < g_ns.pkg_count && shown < vis; i++) {
        if (!matches_search(&g_ns.pkgs[i])) continue;
        if (idx < g_ns.scroll_top) { idx++; continue; }
        int ry = ty + shown * NS_ROW_H;
        bool hot = (mx >= abs_x && mx < abs_x + lw &&
                    my >= abs_y + ry && my < abs_y + ry + NS_ROW_H);
        draw_pkg_row(t, 0, ry, lw, &g_ns.pkgs[i], hot, i == g_ns.selected);
        shown++;
        idx++;
    }

    if (shown == 0) {
        const char *msg = !g_ns.loaded ? L(STR_MSG_LOADING)
                        :  g_ns.no_net  ? L(STR_NXST_NO_NET)
                        :                 L(STR_NXST_EMPTY);
        gfx_draw_string(t, NS_PAD, ty + 20, msg, NS_ROW_SUB, NS_BG);
    }
}

static void draw_status(draw_target_t *t) {
    int sy = (int)t->height - NS_STATUS_H;
    gfx_fill_rect(t, 0, sy, (int)t->width, NS_STATUS_H, NS_STATUS_BG);
    gfx_draw_hline(t, 0, sy, (int)t->width, NS_ROW_DIV);
    char buf[80];
    int total = 0, inst = 0;
    for (int i = 0; i < g_ns.pkg_count; i++) {
        if (!matches_search(&g_ns.pkgs[i])) continue;
        total++;
        if (g_ns.pkgs[i].installed) inst++;
    }
    ksnprintf(buf, sizeof(buf), "%d %s, %d %s",
              total, L(STR_NXST_AVAILABLE), inst, L(STR_NXST_INSTALLED));
    gfx_draw_string(t, NS_PAD, sy + (NS_STATUS_H - FONT_GLYPH_H)/2,
                    buf, NS_STATUS_FG, NS_STATUS_BG);
}

static void ns_redraw(void) {
    if (stale()) return;
    draw_target_t *t = &g_ns.win->content;
    gfx_clear(t, NS_BG);
    draw_header(t);
    draw_search(t);
    draw_list(t);
    draw_status(t);
    wm_mark_dirty();
}

/* ---------- Event handlers ----------------------------------------------- */
static int hit_pkg(int cy) {
    int ty = list_top_y();
    if (cy < ty) return -1;
    int row = (cy - ty) / NS_ROW_H + g_ns.scroll_top;
    int vis_idx = 0;
    for (int i = 0; i < g_ns.pkg_count; i++) {
        if (!matches_search(&g_ns.pkgs[i])) continue;
        if (vis_idx == row) return i;
        vis_idx++;
    }
    return -1;
}

static bool hit_btn(int cx, int cy, int win_w) {
    int ty = list_top_y();
    int row = (cy - ty) / NS_ROW_H;
    int ry = ty + row * NS_ROW_H;
    int bx = win_w - NS_BTN_W - NS_PAD;
    int by = ry + (NS_ROW_H - NS_BTN_H) / 2;
    return cx >= bx && cx < bx + NS_BTN_W && cy >= by && cy < by + NS_BTN_H;
}

static bool hit_refresh(int cx, int cy, int win_w) {
    int bw = NS_BTN_W;
    int bx = win_w - bw - NS_PAD;
    int by = (NS_HEADER_H - NS_BTN_H) / 2;
    return cx >= bx && cx < bx + bw && cy >= by && cy < by + NS_BTN_H;
}

static bool ns_click(window_t *w, int cx, int cy, uint8_t pressed, uint8_t btn) {
    (void)btn;
    if (stale() || w != g_ns.win) return false;
    if (!(pressed & MOUSE_BTN_LEFT)) return false;

    /* Refresh button */
    if (cy < NS_HEADER_H && hit_refresh(cx, cy, (int)w->content.width)) {
        refresh_list();
        ns_redraw();
        return true;
    }

    /* Search bar */
    int sy = NS_HEADER_H + 4;
    if (cy >= sy && cy < sy + NS_SEARCH_H) {
        g_ns.search_focus = true;
        ns_redraw();
        return true;
    }
    g_ns.search_focus = false;

    /* Package list */
    int pidx = hit_pkg(cy);
    if (pidx < 0) { ns_redraw(); return true; }

    if (hit_btn(cx, cy, (int)w->content.width)) {
        ns_pkg_t *pk = &g_ns.pkgs[pidx];
        if (pk->installed) {
            if (dialog_yes_no(L(STR_DLG_CONFIRM_TITLE), L(STR_NXST_CONFIRM_UNINST), pk->name)) {
                int r = nxpkg_uninstall(pk->name);
                if (r == 0) pk->installed = false;
            }
        } else {
            if (dialog_yes_no(L(STR_DLG_CONFIRM_TITLE), L(STR_NXST_CONFIRM_INST), pk->name)) {
                int r = nxpkg_install(pk->name);
                if (r == 0) pk->installed = true;
            }
        }
        ns_redraw();
        return true;
    }

    g_ns.selected = pidx;
    ns_redraw();
    return true;
}

static bool ns_key(window_t *w, int c) {
    if (w != g_ns.win || !g_ns.search_focus) return false;
    if (c == KEY_ESCAPE) { g_ns.search_focus = false; ns_redraw(); return true; }
    if (c == '\n' || c == '\r') {
        g_ns.scroll_top = 0;
        ns_redraw();
        return true;
    }
    if (c == '\b') {
        int n = (int)strlen(g_ns.search);
        if (g_ns.search_caret > 0 && n > 0) {
            memmove(g_ns.search + g_ns.search_caret - 1,
                    g_ns.search + g_ns.search_caret,
                    (size_t)(n - g_ns.search_caret + 1));
            g_ns.search_caret--;
        }
        ns_redraw();
        return true;
    }
    if (c >= 0x20 && c < 0x7F) {
        int n = (int)strlen(g_ns.search);
        if (n + 1 < (int)sizeof(g_ns.search)) {
            memmove(g_ns.search + g_ns.search_caret + 1,
                    g_ns.search + g_ns.search_caret,
                    (size_t)(n - g_ns.search_caret + 1));
            g_ns.search[g_ns.search_caret] = (char)c;
            g_ns.search_caret++;
        }
        ns_redraw();
        return true;
    }
    return true;
}

static void ns_resize_cb(window_t *w) { (void)w; ns_redraw(); }
static void ns_destroy_cb(window_t *w) {
    if (w == g_ns.win) g_ns.win = NULL;
}

static void ns_on_lang_change(lang_t lang) {
    (void)lang;
    ns_redraw();
}

/* ---------- Public API --------------------------------------------------- */
bool nexstore_active(void) { return !stale(); }

bool nexstore_open(void) {
    if (!stale()) {
        wm_set_focus(g_ns.win);
        ns_redraw();
        return true;
    }

    g_ns.win = wm_create_window(NS_WIN_X, NS_WIN_Y,
                                NS_WIN_W, NS_WIN_H,
                                L(STR_APP_NEXSTORE));
    if (!g_ns.win) return false;

    g_ns.selected     = -1;
    g_ns.scroll_top   = 0;
    g_ns.loaded       = false;
    g_ns.no_net       = false;
    g_ns.pkg_count    = 0;
    g_ns.search[0]    = 0;
    g_ns.search_caret = 0;
    g_ns.search_focus = false;

    wm_set_content_click(g_ns.win, ns_click, NULL);
    wm_set_key_handler  (g_ns.win, ns_key);
    wm_set_resizable    (g_ns.win, true, NS_MIN_W, NS_MIN_H);
    wm_set_resize_cb    (g_ns.win, ns_resize_cb);
    wm_set_destroy_cb   (g_ns.win, ns_destroy_cb);
    wm_set_icon         (g_ns.win, ICON_STORE);

    static bool lang_cb_reg = false;
    if (!lang_cb_reg) {
        lang_register_cb(ns_on_lang_change);
        lang_cb_reg = true;
    }

    /* Present the window in its "Loading…" state BEFORE doing any
     * (synchronous) network I/O so the store always appears instantly.
     * Previously refresh_list() blocked here and the window never got a
     * chance to composite, making the whole desktop look frozen. */
    wm_set_focus(g_ns.win);
    ns_redraw();
    wm_present();

    refresh_list();
    ns_redraw();
    return true;
}
