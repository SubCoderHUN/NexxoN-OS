/* ============================================================================
 * NexxoN OS - Login + User Manager windows  (v3.0)
 * ----------------------------------------------------------------------------
 * v3.0 highlights:
 *
 *   * Cursor back-buffer compositor: when the mouse moves, we restore
 *     the 16x16 pixels we previously drew the cursor over (from a
 *     small per-frame snapshot), then save the new position's pixels
 *     and paint the cursor.  The modal panel + backdrop are repainted
 *     only on real state changes (keystroke, focus change, caret
 *     blink, error update) — high-frequency cursor motion no longer
 *     triggers full window repaints.
 *
 *   * Layout fix: footer hint text now lives BELOW the inputs and
 *     ABOVE the Sign In button (or in a reserved error row),
 *     eliminating the previous intersection.
 *
 *   * Footer hint text is 100% i18n-driven (no English literal in the
 *     render path).
 * ============================================================================ */
#include "apps.h"
#include "window.h"
#include "icons.h"
#include "gfx.h"
#include "font.h"
#include "vga.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "keyboard.h"
#include "pit.h"
#include "desktop.h"
#include "dialogs.h"
#include "i18n.h"
#include "auth.h"

/* ---------- Login window ---------------------------------------------- */
#define LOGIN_W      460
#define LOGIN_H      300

/* Background palette — Aero Glass: translucent panel with a subtle
 * white-alpha highlight border so the wallpaper dot grid + gradient
 * read through the login modal. */
#define LOGIN_BG_TOP     0xFF101830
#define LOGIN_BG_BOT     0xFF202848
/* Panel glass tint: cool white #F0F4FA at alpha 178.  Bright enough
 * for the labels to remain crisply legible, translucent enough to
 * unmistakably read as glass. */
#define LOGIN_PANEL_BG   0xB2F0F4FAu
/* Header gradient: blue with alpha so the panel tint blends through */
#define LOGIN_PANEL_HDR_TOP  0xD81850C8u
#define LOGIN_PANEL_HDR_BOT  0xD8002878u
#define LOGIN_PANEL_HDR_FG   0xFFFFFFFF
#define LOGIN_LBL        0xFF1A1A24
#define LOGIN_INP_BG     0xFFFFFFFF
#define LOGIN_INP_FG     0xFF101010
#define LOGIN_INP_BORDER 0xFF606080
#define LOGIN_INP_FOCUS  0xFF1850C8
#define LOGIN_BTN_BG     0xFF208030
#define LOGIN_BTN_BG_H   0xFF30A040
#define LOGIN_BTN_FG     0xFFFFFFFF
#define LOGIN_ERR        0xFFD03030
/* Solid colour used as the AA pre-fill on TEXT-FREE inner widgets
 * (input fields, signin button) where the underlying surface is
 * already a fully-painted opaque rect. */
#define LOGIN_PANEL_BG_OPAQUE 0xFFF0F4FAu

/* Cursor footprint - matches the 16x16 arrow shape drawn by
 * draw_cursor.  16x16 = 256 pixels * 4 bytes = 1 KiB snapshot. */
#define CURSOR_W 16
#define CURSOR_H 16
static uint32_t g_cursor_save[CURSOR_W * CURSOR_H];
static int      g_cursor_save_x = -1;
static int      g_cursor_save_y = -1;
static bool     g_cursor_saved  = false;

/* Render/present targets for the login modal.
 *   g_bb  = the WM's screen-sized RAM back buffer (cached write-back memory).
 *           The whole modal -- including every alpha-blended glass/shadow
 *           layer, which READS the destination per pixel -- is composed here.
 *   g_scr = the hardware VESA framebuffer.  We push the finished frame to it
 *           with one write-only blit (wm_blit_backbuffer), and composite the
 *           mouse cursor on top using clean pixels copied out of g_bb.
 * Drawing the blended panel straight onto g_scr (as the old code did) turned
 * every blended pixel into a slow uncached LFB read on real hardware, which
 * is exactly why the login screen crawled / "rendered in layers and never
 * finished" on bare metal while being instant under emulation. */
static draw_target_t *g_bb  = NULL;
static draw_target_t *g_scr = NULL;

static char       g_user[AUTH_NAME_MAX];
static char       g_pwd [32];
static int        g_user_caret;
static int        g_pwd_caret;
static int        g_field;
static const char *g_err;

typedef struct {
    int x, y, w, h;
} rect_t;

static rect_t g_user_rect, g_pwd_rect, g_signin_rect;
static int    g_modal_x, g_modal_y;

static bool pt_in(int px, int py, const rect_t *r) {
    return px >= r->x && px < r->x + r->w &&
           py >= r->y && py < r->y + r->h;
}

static uint32_t lerp_ch(uint32_t a, uint32_t b, int num, int den) {
    if (den <= 0) return a;
    return (uint32_t)((int32_t)a + (int32_t)(b - a) * num / den);
}
static uint32_t lerp_argb(uint32_t a, uint32_t b, int num, int den) {
    uint32_t aa = (a >> 24) & 0xFF, ab = (b >> 24) & 0xFF;
    uint32_t ra = (a >> 16) & 0xFF, rb = (b >> 16) & 0xFF;
    uint32_t ga = (a >>  8) & 0xFF, gb = (b >>  8) & 0xFF;
    uint32_t ba = (a      ) & 0xFF, bb = (b      ) & 0xFF;
    return (lerp_ch(aa, ab, num, den) << 24) |
           (lerp_ch(ra, rb, num, den) << 16) |
           (lerp_ch(ga, gb, num, den) <<  8) |
            lerp_ch(ba, bb, num, den);
}

static void fill_vgrad(draw_target_t *t, int x, int y, int w, int h,
                       uint32_t top, uint32_t bot) {
    if (h <= 0) return;
    for (int r = 0; r < h; r++) {
        uint32_t c = lerp_argb(top, bot, r, h - 1);
        gfx_draw_hline(t, x, y + r, w, c);
    }
}

static void draw_input(draw_target_t *t, const rect_t *r,
                       const char *text, bool password, int caret,
                       bool focused, bool show_caret) {
    gfx_fill_rect(t, r->x, r->y, r->w, r->h, LOGIN_INP_BG);
    uint32_t border = focused ? LOGIN_INP_FOCUS : LOGIN_INP_BORDER;
    gfx_draw_rect(t, r->x, r->y, r->w, r->h, border);
    if (focused) {
        gfx_draw_rect(t, r->x + 1, r->y + 1, r->w - 2, r->h - 2, border);
    }
    char buf[64];
    if (password) {
        int n = (int)strlen(text);
        if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
        for (int i = 0; i < n; i++) buf[i] = '*';
        buf[n] = 0;
    } else {
        strncpy(buf, text, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
    }
    int tx = r->x + 6;
    int ty = r->y + (r->h - FONT_GLYPH_H) / 2;
    gfx_draw_string(t, tx, ty, buf, LOGIN_INP_FG, LOGIN_INP_BG);
    if (focused && show_caret) {
        int cx = tx + caret * FONT_GLYPH_W;
        gfx_fill_rect(t, cx, r->y + 4, 1, r->h - 8, LOGIN_INP_FG);
    }
}

static void draw_button(draw_target_t *t, const rect_t *r, const char *label,
                        bool hot) {
    uint32_t top = hot ? LOGIN_BTN_BG_H : LOGIN_BTN_BG;
    uint32_t bot = hot ? LOGIN_BTN_BG   : 0xFF105818;
    fill_vgrad(t, r->x, r->y, r->w, r->h, top, bot);
    gfx_draw_rect(t, r->x, r->y, r->w, r->h, 0xFF000000);
    gfx_draw_hline(t, r->x + 1, r->y + 1, r->w - 2, 0xFFA0E0A8);
    int lw = gfx_string_pixel_width(label);
    int lx = r->x + (r->w - lw) / 2;
    int ly = r->y + (r->h - FONT_GLYPH_H) / 2;
    /* Transparent text bg so the glyph rim follows the live gradient
     * underneath instead of a flat per-row strip. */
    gfx_draw_string(t, lx + 1, ly + 1, label, 0xFF003000, 0x00000000u);
    gfx_draw_string(t, lx,     ly,     label, LOGIN_BTN_FG, 0x00000000u);
}

static const char *g_login_cursor[16] = {
    "X...............",
    "XX..............",
    "X#X.............",
    "X##X............",
    "X###X...........",
    "X####X..........",
    "X#####X.........",
    "X######X........",
    "X#######X.......",
    "X########X......",
    "X#####XXXX......",
    "X##X##X.........",
    "X#X.X##X........",
    "XX..X##X........",
    "X....X##X.......",
    ".....X##X.......",
};

/* Read a single pixel from the screen framebuffer.  Returns 0 if the
 * coordinates are out of bounds. */
static uint32_t pixel_at(draw_target_t *t, int x, int y) {
    if (!t || !t->fb) return 0;
    if (x < 0 || y < 0 || (uint32_t)x >= t->width || (uint32_t)y >= t->height) return 0;
    return *(uint32_t *)(t->fb + (uint32_t)y * t->pitch + (uint32_t)x * 4);
}

/* Snapshot the 16x16 pixel block under (mx, my) into g_cursor_save so
 * we can restore it on the next mouse move.  Skips pixels that the
 * cursor doesn't actually paint (the transparent dots in the mask). */
static void cursor_save(draw_target_t *t, int mx, int my) {
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            g_cursor_save[y * CURSOR_W + x] = pixel_at(t, mx + x, my + y);
        }
    }
    g_cursor_save_x = mx;
    g_cursor_save_y = my;
    g_cursor_saved  = true;
}

/* Restore the previously-saved 16x16 pixel block.  Only writes pixels
 * that the cursor would have drawn over, so adjacent UI elements
 * remain untouched. */
static void cursor_restore(draw_target_t *t) {
    if (!g_cursor_saved) return;
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            uint32_t v = g_cursor_save[y * CURSOR_W + x];
            int px = g_cursor_save_x + x;
            int py = g_cursor_save_y + y;
            if (px < 0 || py < 0) continue;
            if ((uint32_t)px >= t->width || (uint32_t)py >= t->height) continue;
            *(uint32_t *)(t->fb + (uint32_t)py * t->pitch + (uint32_t)px * 4) = v;
        }
    }
    g_cursor_saved = false;
}

static void cursor_paint(draw_target_t *t, int mx, int my) {
    for (int y = 0; y < 16; y++) {
        const char *row = g_login_cursor[y];
        for (int x = 0; x < 16; x++) {
            uint32_t c;
            switch (row[x]) {
                case 'X': c = 0xFFFFFFFFu; break;
                case '#': c = 0xFF000000u; break;
                default : continue;
            }
            gfx_putpixel(t, mx + x, my + y, c);
        }
    }
}

/* Repaint the static modal contents.  Does NOT paint the cursor.
 * Called only when the visible UI state actually changes - mouse
 * motion alone does NOT trigger this. */
static void redraw_modal(int sw, int sh, bool show_caret, bool signin_hot) {
    /* Compose into the RAM back buffer; login_show() presents it to the
     * hardware framebuffer afterwards with a single write-only blit. */
    draw_target_t *t = g_bb ? g_bb : gfx_screen();
    int x = (sw - LOGIN_W) / 2;
    int y = (sh - LOGIN_H) / 2;
    g_modal_x = x; g_modal_y = y;

    fill_vgrad(t, 0, 0, sw, sh, LOGIN_BG_TOP, LOGIN_BG_BOT);
    for (int dy = 12; dy < sh; dy += 18) {
        for (int dx = 12; dx < sw; dx += 18) {
            gfx_putpixel(t, dx, dy, 0xFF303860);
        }
    }

    for (int s = 1; s <= 6; s++) {
        gfx_blend_rect(t, x + s, y + s, LOGIN_W, LOGIN_H,
                       ((uint32_t)(60 - s * 8) << 24));
    }

    /* Glass panel: alpha-blended fill so the wallpaper bleeds through,
     * with a 1 px white-alpha highlight border for the Aero edge. */
    gfx_blend_rect(t, x, y, LOGIN_W, LOGIN_H, LOGIN_PANEL_BG);
    /* Subtle outer frame: dark hairline + white-alpha glass highlight */
    gfx_blend_rect(t, x,             y,             LOGIN_W, 1,      0x80202030u);
    gfx_blend_rect(t, x,             y + LOGIN_H-1, LOGIN_W, 1,      0x80202030u);
    gfx_blend_rect(t, x,             y,             1, LOGIN_H,      0x80202030u);
    gfx_blend_rect(t, x + LOGIN_W-1, y,             1, LOGIN_H,      0x80202030u);
    gfx_blend_rect(t, x + 1,         y + 1,         LOGIN_W - 2, 1,  0xA0FFFFFFu);
    gfx_blend_rect(t, x + 1,         y + 1,         1, LOGIN_H - 2,  0x80FFFFFFu);
    gfx_blend_rect(t, x + LOGIN_W-2, y + 1,         1, LOGIN_H - 2,  0x80FFFFFFu);

    int hdr_h = 44;
    /* Header gradient — alpha-blended so the panel tint shows through */
    for (int rr = 0; rr < hdr_h; rr++) {
        uint32_t c = lerp_argb(LOGIN_PANEL_HDR_TOP, LOGIN_PANEL_HDR_BOT, rr, hdr_h - 1);
        gfx_blend_rect(t, x + 2, y + 2 + rr, LOGIN_W - 4, 1, c);
    }
    const char *welcome = i18n("login.welcome");
    int welcome_w = gfx_string_pixel_width(welcome);
    int welcome_x = x + (LOGIN_W - welcome_w) / 2;
    /* Transparent glyph backgrounds: text reads cleanly over the glass */
    gfx_draw_string_aa(t, welcome_x + 1, y + 2 + (hdr_h - FONT_GLYPH_H) / 2 + 1,
                       welcome, 0xFF000000, 0x00000000u);
    gfx_draw_string_aa(t, welcome_x, y + 2 + (hdr_h - FONT_GLYPH_H) / 2,
                       welcome, LOGIN_PANEL_HDR_FG, 0x00000000u);

    int inp_x = x + 130;
    int inp_w = LOGIN_W - 130 - 24;
    int row1_y = y + hdr_h + 28;
    int row2_y = row1_y + 50;
    gfx_draw_string(t, x + 24, row1_y + 6, i18n("login.username"),
                    LOGIN_LBL, 0x00000000u);
    gfx_draw_string(t, x + 24, row2_y + 6, i18n("login.password"),
                    LOGIN_LBL, 0x00000000u);
    g_user_rect = (rect_t){ inp_x, row1_y, inp_w, 24 };
    g_pwd_rect  = (rect_t){ inp_x, row2_y, inp_w, 24 };
    draw_input(t, &g_user_rect, g_user, false, g_user_caret,
               g_field == 0, show_caret);
    draw_input(t, &g_pwd_rect,  g_pwd,  true,  g_pwd_caret,
               g_field == 1, show_caret);

    /* Footer hint row sits BETWEEN the password input and the button,
     * not on top of the button.  Previously the helper text and the
     * green button were sharing y-coordinates which produced a clear
     * visual intersection. */
    int hint_y  = row2_y + 24 + 20;
    /* Strict i18n: footer hint comes from the localised table.  Falls
     * back to the legacy key + an EN literal only if the lookup
     * misses, which is a development-time safety net rather than a
     * shipping behaviour. */
    const char *hint = i18n_or("login.hint",
                               (i18n_get_language() == LANG_HU)
                               ? "Tab / egér: mezővált, Enter: belépés"
                               : "Tab / mouse to switch fields, Enter to sign in");
    if (g_err) {
        gfx_draw_string(t, x + 24, hint_y, g_err,
                        LOGIN_ERR, 0x00000000u);
    } else {
        gfx_draw_string(t, x + 24, hint_y, hint, 0xFF606080, 0x00000000u);
    }

    /* Sign In button moved DOWN to its own row so it never intersects
     * the hint line.  Padding restored to give the modal a balanced
     * vertical rhythm. */
    int btn_w = 140, btn_h = 34;
    int btn_y = y + LOGIN_H - btn_h - 22;
    g_signin_rect = (rect_t){ x + LOGIN_W - btn_w - 24, btn_y, btn_w, btn_h };
    draw_button(t, &g_signin_rect, i18n("login.signin"), signin_hot);

    /* After a full modal repaint we have no valid cursor snapshot. */
    g_cursor_saved = false;
}

/* Restore cursor saved pixels, then save+paint at the new position.
 * This is the dirty-rectangle path: it touches at most 2*16*16
 * pixels per mouse move, no matter how the modal beneath is sized. */
static void cursor_step(int mx, int my) {
    /* Erase the previous cursor by writing back the clean pixels we snapshot
     * from the RAM frame, snapshot the clean pixels under the NEW position
     * (read from g_bb -- RAM, never the LFB), then paint the cursor.  Every
     * framebuffer access here is a WRITE; the only reads come from cached
     * RAM, so high-frequency mouse motion stays cheap even on bare metal. */
    cursor_restore(g_scr);
    cursor_save(g_bb, mx, my);
    cursor_paint(g_scr, mx, my);
}

void login_show(void) {
    int sw = (int)vga_width();
    int sh = (int)vga_height();

    memset(g_user, 0, sizeof(g_user));
    memset(g_pwd,  0, sizeof(g_pwd));
    g_user_caret = 0;
    g_pwd_caret  = 0;
    g_field      = 0;
    g_err        = NULL;
    g_cursor_saved = false;

    keyboard_drain();
    keyboard_clear_abort();

    /* Bind the render/present targets: compose into the WM's RAM back
     * buffer, present to the hardware framebuffer.  wm_init() ran during
     * boot (before login_show), so the back buffer is allocated and sized
     * to the screen by now. */
    g_bb  = wm_backbuffer();
    g_scr = gfx_screen();

    /* Initial paint: full modal once into RAM, blit it to the screen, then
     * snapshot the cursor footprint (from RAM) and paint the cursor. */
    int mx = mouse_x(), my = mouse_y();
    uint8_t btn = mouse_btn();
    bool signin_hot = pt_in(mx, my, &g_signin_rect);  /* false on first frame */
    redraw_modal(sw, sh, true, signin_hot);
    wm_blit_backbuffer();
    cursor_save(g_bb, mx, my);
    cursor_paint(g_scr, mx, my);

    uint32_t   last_blink_ms = pit_ms();
    bool       caret_on      = true;
    uint8_t    last_btn      = btn;
    int        last_mx       = mx;
    int        last_my       = my;
    bool       last_hot      = signin_hot;
    bool       full_dirty    = false;

    /* Bare-metal keyboards/mice are USB, not PS/2.  The shell idle loop
     * (which normally pumps the USB stack via pnp_tick) isn't running yet
     * during login, so poll the hot-plug + HID report path here too —
     * otherwise the sign-in screen would be dead on real hardware. */
    extern int  usb_poll(void);
    extern void usbhid_poll(void);

    for (;;) {
        usb_poll();
        usbhid_poll();

        int cur_mx, cur_my;
        uint8_t cur_btn;
        bool moved = mouse_poll(&cur_mx, &cur_my, &cur_btn);

        /* Detect signin-hot edge so the button hover state still
         * triggers a repaint (its colour changes on hover).  But the
         * vast majority of mouse moves do NOT touch the button so the
         * fast cursor-only path stays hot. */
        bool cur_hot = pt_in(cur_mx, cur_my, &g_signin_rect);
        if (cur_hot != last_hot) {
            full_dirty = true;
            last_hot   = cur_hot;
        }

        uint32_t now = pit_ms();
        if (now - last_blink_ms >= 500) {
            caret_on      = !caret_on;
            last_blink_ms = now;
            full_dirty    = true;
        }

        uint8_t pressed = (uint8_t)(cur_btn & ~last_btn);
        last_btn = cur_btn;
        last_mx  = cur_mx;
        last_my  = cur_my;

        bool do_signin = false;
        if (pressed & MOUSE_BTN_LEFT) {
            if (pt_in(cur_mx, cur_my, &g_user_rect)) {
                g_field = 0;
                full_dirty = true;
            } else if (pt_in(cur_mx, cur_my, &g_pwd_rect)) {
                g_field = 1;
                full_dirty = true;
            } else if (pt_in(cur_mx, cur_my, &g_signin_rect)) {
                do_signin = true;
            }
        }

        /* Order matters: recompose the modal into RAM and blit it to the
         * screen first (which clobbers the cursor area), then snapshot the
         * fresh footprint from RAM and paint the cursor on top. */
        if (full_dirty) {
            redraw_modal(sw, sh, caret_on, cur_hot);
            wm_blit_backbuffer();
            /* The blit overwrote the whole screen including the cursor
             * region, so the old snapshot is stale: take a fresh one from
             * RAM and draw the cursor on top. */
            cursor_save(g_bb, cur_mx, cur_my);
            cursor_paint(g_scr, cur_mx, cur_my);
            full_dirty = false;
        } else if (moved) {
            cursor_step(cur_mx, cur_my);
        }

        if (!do_signin) {
            if (!keyboard_has_data()) {
                __asm__ volatile ("sti; hlt");
                continue;
            }
        }

        int c = do_signin ? '\n' : keyboard_wait_getc();
        if (!do_signin && c == 0) continue;

        if (c == '\t') {
            g_field = 1 - g_field;
            full_dirty = true;
            continue;
        }
        if (c == '\n' || c == '\r') {
            auth_role_t role;
            if (auth_verify(g_user, g_pwd, &role)) {
                auth_set_current_user(g_user);
                {
                    extern int vault_unlock(const char *p);
                    vault_unlock(g_pwd);
                }
                debug_printf("[login] '%s' authenticated (role=%d)\n",
                             g_user, (int)role);
                memset(g_pwd, 0, sizeof(g_pwd));
                return;
            }
            g_err = i18n("login.bad");
            memset(g_pwd, 0, sizeof(g_pwd));
            g_pwd_caret = 0;
            full_dirty = true;
            continue;
        }
        if (c == '\b') {
            if (g_field == 0 && g_user_caret > 0) {
                g_user[--g_user_caret] = 0;
                full_dirty = true;
            } else if (g_field == 1 && g_pwd_caret > 0) {
                g_pwd[--g_pwd_caret] = 0;
                full_dirty = true;
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {
            if (g_field == 0 && g_user_caret < AUTH_NAME_MAX - 1) {
                g_user[g_user_caret++] = (char)c;
                g_user[g_user_caret]   = 0;
                full_dirty = true;
            } else if (g_field == 1 && g_pwd_caret < (int)sizeof(g_pwd) - 1) {
                g_pwd[g_pwd_caret++] = (char)c;
                g_pwd[g_pwd_caret]   = 0;
                full_dirty = true;
            }
        }
        (void)last_mx; (void)last_my;
    }
}

/* ---------- User Manager GUI ----------------------------------------- */
#define UM_W      460
#define UM_H      300
#define UM_BG     0xFFF0F0F4
#define UM_HDR    0xFF002878
#define UM_ROW    18

static window_t *g_um_win = NULL;
static int       g_um_sel = -1;

static void um_redraw(void);

static bool um_click(window_t *w, int cx, int cy,
                     uint8_t pressed, uint8_t btn) {
    (void)btn;
    if (w != g_um_win || !(pressed & MOUSE_BTN_LEFT)) return true;

    int list_top = 60;
    if (cy >= list_top && cy < list_top + AUTH_MAX_USERS * UM_ROW) {
        int idx = (cy - list_top) / UM_ROW;
        if (idx >= 0 && idx < AUTH_MAX_USERS) {
            auth_user_t u;
            if (auth_get_user(idx, &u)) {
                g_um_sel = idx;
                um_redraw();
            }
        }
        return true;
    }
    int by = (int)w->content.height - 40;
    if (cy >= by && cy < by + 28) {
        if (cx >= 16 && cx < 16 + 100) {
            char name[AUTH_NAME_MAX], pwd[32];
            name[0] = 0;
            if (!dialog_input("New user", "Username:", "",
                              name, sizeof(name))) return true;
            pwd[0] = 0;
            if (!dialog_input("New user", "Password:", "",
                              pwd, sizeof(pwd))) return true;
            if (!auth_add_user(name, pwd, ROLE_USER)) {
                const char *info[] = { "Could not add user (duplicate or limit)" };
                dialog_info("User Manager", info, 1);
            }
            um_redraw();
            return true;
        }
        if (cx >= 130 && cx < 130 + 120) {
            auth_user_t u;
            if (g_um_sel >= 0 && auth_get_user(g_um_sel, &u)) {
                char pwd[32];
                pwd[0] = 0;
                if (dialog_input("Change password",
                                 "New password:", "", pwd, sizeof(pwd))) {
                    auth_change_password(u.name, pwd);
                }
            }
            um_redraw();
            return true;
        }
        if (cx >= 260 && cx < 260 + 100) {
            auth_user_t u;
            if (g_um_sel >= 0 && auth_get_user(g_um_sel, &u)) {
                char ln[80];
                ksnprintf(ln, sizeof(ln), "Remove user '%s'?", u.name);
                if (dialog_yes_no("Confirm", ln, NULL)) {
                    auth_remove_user(u.name);
                    g_um_sel = -1;
                }
            }
            um_redraw();
            return true;
        }
    }
    return true;
}

static void um_redraw(void) {
    if (!g_um_win || !g_um_win->in_use) { g_um_win = NULL; return; }
    draw_target_t *t = &g_um_win->content;
    gfx_clear(t, UM_BG);
    gfx_draw_string_aa(t, 12, 14, L(STR_APP_USERMGR), UM_HDR, UM_BG);
    gfx_draw_string(t, 12, 38,
                    "Name                    Role     ", 0xFF606060, UM_BG);
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        auth_user_t u;
        int row_y = 60 + i * UM_ROW;
        if (!auth_get_user(i, &u)) continue;
        bool sel = (i == g_um_sel);
        if (sel) gfx_fill_rect(t, 0, row_y, (int)t->width, UM_ROW, 0xFF002878);
        char line[80];
        ksnprintf(line, sizeof(line), "%-24s %s",
                  u.name, u.role == ROLE_ADMIN ?
                            L(STR_SET_SEC_ROLE_ADMIN) :
                            L(STR_SET_SEC_ROLE_USER));
        gfx_draw_string(t, 14, row_y + (UM_ROW - 8) / 2, line,
                        sel ? 0xFFFFFFFF : 0xFF101010,
                        sel ? 0xFF002878 : UM_BG);
    }
    int by = (int)t->height - 40;
    const char *labels[3] = {
        i18n_or("usermgr.btn.add",
                i18n_get_language() == LANG_HU ? "Hozzáad" : "Add user"),
        i18n_or("usermgr.btn.pw",
                i18n_get_language() == LANG_HU ? "Jelszó csere" : "Change pwd"),
        L(STR_BTN_DELETE),
    };
    int xs[3] = { 16, 130, 260 };
    int ws[3] = { 100, 120, 100 };
    for (int b = 0; b < 3; b++) {
        gfx_fill_rect(t, xs[b], by, ws[b], 28, 0xFF1850C8);
        gfx_draw_rect(t, xs[b], by, ws[b], 28, 0xFF000000);
        int lw = gfx_string_pixel_width(labels[b]);
        gfx_draw_string(t, xs[b] + (ws[b] - lw) / 2,
                        by + (28 - 8) / 2,
                        labels[b], 0xFFFFFFFF, 0xFF1850C8);
    }
    wm_mark_dirty();
}

static void um_resize_cb(window_t *w) { (void)w; um_redraw(); }
static void um_destroy_cb(window_t *w) { if (w == g_um_win) g_um_win = NULL; }

bool usermgr_open(void) {
    if (g_um_win && g_um_win->in_use) {
        wm_set_focus(g_um_win);
        um_redraw();
        return true;
    }
    g_um_win = wm_create_window(140, 80, UM_W, UM_H, L(STR_APP_USERMGR));
    if (!g_um_win) return false;
    wm_set_content_click(g_um_win, um_click, NULL);
    wm_set_resizable(g_um_win, true, 360, 240);
    wm_set_resize_cb(g_um_win, um_resize_cb);
    wm_set_destroy_cb(g_um_win, um_destroy_cb);
    wm_set_icon(g_um_win, ICON_USERS);
    um_redraw();
    return true;
}
