/* ============================================================================
 * NexxoN OS - Context menu implementation  (v1.1, facelift)
 * ----------------------------------------------------------------------------
 * See ctxmenu.h for the API contract.  Implementation notes:
 *
 *   - The menu sits at fixed coordinates after open(); it does NOT follow
 *     the cursor.  Keeping the geometry static avoids re-clipping the menu
 *     every frame and matches every desktop OS user expectation.
 *   - Drawing happens AFTER the panel pass (so the menu is on top of the
 *     taskbar too, which matters when the user right-clicks near the bottom
 *     of the screen and the menu would otherwise be hidden behind it).
 *   - Click handling: clicks on an item fire the callback and close the
 *     menu.  Clicks outside close it without firing.  Either way the
 *     click is CONSUMED so the underlying window doesn't also see it.
 *
 * v1.1 (BUG 7): sharper 3D bevel + drop shadow + cleaner hover state.
 * ============================================================================ */
#include "ctxmenu.h"
#include "gfx.h"
#include "font.h"
#include "vga.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "theme.h"
#include "pit.h"
#include "window.h"   /* wm_mark_dirty() to drive the open animation */

/* ---------- Geometry --------------------------------------------------- */
#define CTX_ITEM_H      24
#define CTX_SEP_H       8
#define CTX_PAD_X       8
#define CTX_PAD_Y       6
#define CTX_W           172

/* ---------- Colour palette -------------------------------------------- */
#define CTX_BG          0xFFF2F2F4
#define CTX_HI          0xFFFFFFFF
#define CTX_HI2         0xFFE0E0E0
#define CTX_LO          0xFF202020
#define CTX_LO2         0xFF606060
#define CTX_SHADOW      0xFF000000
#define CTX_ITEM_FG     0xFF101010
#define CTX_ITEM_HOT_BG 0xFF1850C8
#define CTX_ITEM_HOT_FG 0xFFFFFFFF
#define CTX_ITEM_DEAD   0xFFA0A0A0
#define CTX_DANGER_FG   0xFFB00000
#define CTX_SEP_LINE_LO 0xFF808080
#define CTX_SEP_LINE_HI 0xFFFFFFFF

/* ---------- Static state ---------------------------------------------- */
static bool             g_active   = false;
static int              g_x        = 0;
static int              g_y        = 0;
static int              g_w        = CTX_W;
static int              g_h        = 0;
static int              g_n        = 0;
static ctxmenu_item_t   g_items[CTXMENU_MAX_ITEMS];
static ctxmenu_cb_t     g_cb       = NULL;
static void            *g_user     = NULL;
static uint32_t         g_open_ms  = 0;     /* open-animation start (PIT ms) */

/* ---------- Helpers --------------------------------------------------- */
static int item_height(const ctxmenu_item_t *it) {
    return it->separator ? CTX_SEP_H : CTX_ITEM_H;
}

static int total_height(void) {
    int h = CTX_PAD_Y * 2;
    for (int i = 0; i < g_n; i++) h += item_height(&g_items[i]);
    return h;
}

static bool point_in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static int hit_test_item(int mx, int my) {
    if (!point_in_rect(mx, my, g_x, g_y, g_w, g_h)) return -1;
    int y = g_y + CTX_PAD_Y;
    for (int i = 0; i < g_n; i++) {
        int h = item_height(&g_items[i]);
        if (my >= y && my < y + h) return i;
        y += h;
    }
    return -1;
}

/* ---------- Public API ------------------------------------------------ */
void ctxmenu_open(int x, int y,
                  const ctxmenu_item_t *items, int n_items,
                  ctxmenu_cb_t cb, void *user) {
    if (n_items <= 0 || !items) {
        debug_printf("[ctxmenu] open with empty item list - ignored\n");
        return;
    }
    if (n_items > CTXMENU_MAX_ITEMS) n_items = CTXMENU_MAX_ITEMS;

    memset(g_items, 0, sizeof(g_items));
    for (int i = 0; i < n_items; i++) {
        g_items[i] = items[i];
        g_items[i].label[CTXMENU_LABEL_MAX - 1] = 0;
    }
    g_n    = n_items;
    g_w    = CTX_W;
    g_h    = total_height();
    g_cb   = cb;
    g_user = user;

    /* Clip into the screen so the menu is always fully visible (account
     * for the drop shadow so it doesn't bleed off the edge). */
    int sw = (int)vga_width();
    int sh = (int)vga_height();
    if (x + g_w + 3 > sw) x = sw - g_w - 3;
    if (y + g_h + 3 > sh) y = sh - g_h - 3;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_x = x;
    g_y = y;

    g_active  = true;
    g_open_ms = pit_ms();
    debug_printf("[ctxmenu] opened at (%d,%d) %d items %dx%d\n",
                 g_x, g_y, g_n, g_w, g_h);
}

void ctxmenu_close(void) {
    if (!g_active) return;
    g_active = false;
    g_cb     = NULL;
    g_user   = NULL;
    g_n      = 0;
    debug_printf("[ctxmenu] closed\n");
}

bool ctxmenu_active(void) { return g_active; }

/* ---------- Render ---------------------------------------------------- */
static void draw_separator(draw_target_t *t, int x, int y, int w) {
    int cy = y + CTX_SEP_H / 2;
    gfx_blend_rect(t, x + 8, cy,     w - 16, 1, 0x30000000u);
    gfx_blend_rect(t, x + 8, cy + 1, w - 16, 1, 0x50FFFFFFu);
}

void ctxmenu_draw(draw_target_t *t) {
    if (!g_active || !t || !t->fb) return;

    /* Liquid open: the menu expands downward + fades in with ease-out cubic.
     * Keep the compositor dirty until the animation settles. */
    uint32_t dt   = pit_ms() - g_open_ms;
    int      prog = (int)anim_ease_out_cubic(dt, ANIM_MENU_MS);   /* 0..256 */
    if (dt < ANIM_MENU_MS) wm_mark_dirty();
    int vis_h = g_h * prog / 256;
    if (vis_h < CTX_ITEM_H) vis_h = CTX_ITEM_H;
    if (vis_h > g_h)        vis_h = g_h;

    /* Soft drop shadow + frosted-glass body (blur backdrop -> frost -> bevel). */
    gfx_drop_shadow_round(t, g_x, g_y, g_w, vis_h, RADIUS_HOVER, 7);
    gfx_glass_panel(t, g_x, g_y, g_w, vis_h, RADIUS_HOVER, GLASS_POPUP);

    int hover = hit_test_item(mouse_x(), mouse_y());

    int y = g_y + CTX_PAD_Y;
    for (int i = 0; i < g_n; i++) {
        const ctxmenu_item_t *it = &g_items[i];
        int h = item_height(it);
        if (y + h > g_y + vis_h - CTX_PAD_Y) break;   /* not yet revealed */

        if (it->separator) {
            draw_separator(t, g_x, y, g_w);
            y += h;
            continue;
        }

        bool hot = (hover == i) && !it->disabled;
        uint32_t fg = it->disabled  ? CTX_ITEM_DEAD
                    : hot           ? 0xFFFFFFFFu
                    : it->dangerous ? CTX_DANGER_FG
                    :                 GLASS_TEXT_DARK;

        if (hot) {
            /* Glossy accent capsule, 4 px rounded. */
            gfx_blend_round_rect(t, g_x + 3, y, g_w - 6, h, RADIUS_HOVER,
                                 (GLASS_ACCENT & 0x00FFFFFFu) | 0xE0000000u);
            gfx_blend_round_rect(t, g_x + 4, y + 1, g_w - 8, h / 2, RADIUS_HOVER,
                                 0x2EFFFFFFu);
        }
        gfx_draw_string_aa(t, g_x + CTX_PAD_X + 4,
                           y + (h - FONT_GLYPH_H) / 2,
                           it->label, fg, 0x00000000u);
        y += h;
    }
}

/* ---------- Click handling ------------------------------------------- */
bool ctxmenu_handle_click(int mx, int my, uint8_t pressed_btn) {
    if (!g_active) return false;
    if (!(pressed_btn & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT))) return false;

    int idx = hit_test_item(mx, my);

    ctxmenu_cb_t  cb     = g_cb;
    void         *user   = g_user;
    bool          fire   = (idx >= 0)
                           && !g_items[idx].separator
                           && !g_items[idx].disabled;
    int           id     = fire ? g_items[idx].id : 0;

    ctxmenu_close();

    if (fire && cb) cb(id, user);
    return true;
}
