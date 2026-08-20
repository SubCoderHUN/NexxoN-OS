/* ============================================================================
 * NexxoN OS - Graphical Installer  (v1.0)
 * ----------------------------------------------------------------------------
 * A guided multi-step wizard that installs NexxoN OS from the live RAMFS
 * image to a SATA hard disk via nxfs_install_to_sata().
 *
 * Wizard steps:
 *   1. Welcome screen
 *   2. Disk information (AHCI target detected, size shown)
 *   3. Confirmation (erases the disk)
 *   4. Progress bar during installation
 *   5. Done / Error result
 * ============================================================================ */
#include "sysdisk.h"
#include "installer.h"
#include "window.h"
#include "icons.h"
#include "gfx.h"
#include "font.h"
#include "i18n.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "keyboard.h"
#include "nxfs.h"
#include "ahci.h"
#include "dialogs.h"
#include "notify.h"
#include "pit.h"
#include "acpi.h"
#include "auth.h"   /* #8: create the account chosen during installation */

/* ---------- Layout ------------------------------------------------------- */
#define INST_WIN_W    540
#define INST_WIN_H    380
#define INST_WIN_X    100
#define INST_WIN_Y     80

#define INST_PAD       16
#define INST_HDR_H     50
#define INST_BTN_W     90
#define INST_BTN_H     28
#define INST_PROG_H    16

/* ---------- Colours ------------------------------------------------------ */
#define INST_BG         0xFFF0F2F8
#define INST_HDR_BG     0xFF181860
#define INST_HDR_FG     0xFFFFFFFF
#define INST_TEXT_FG    0xFF202030
#define INST_TEXT_DIM   0xFF708090
#define INST_BTN_NEXT   0xFF1870D0
#define INST_BTN_BACK   0xFF606878
#define INST_BTN_INST   0xFFC04020
#define INST_BTN_REBOOT 0xFF208040
#define INST_BTN_FG     0xFFFFFFFF
#define INST_BTN_DIS    0xFFB0B8C0
#define INST_PROG_TRACK 0xFFCCCCCC
#define INST_PROG_FILL  0xFF1870D0
#define INST_PROG_BD    0xFF8090A8
#define INST_STEP_ACT   0xFF1870D0
#define INST_STEP_DONE  0xFF208040
#define INST_STEP_INACT 0xFFC0C8D8

/* ---------- Wizard state ------------------------------------------------- */
typedef enum {
    STEP_WELCOME = 0,
    STEP_DISK,
    STEP_CONFIRM,
    STEP_INSTALLING,
    STEP_DONE,
    STEP_COUNT,
} inst_step_t;

static struct {
    window_t   *win;
    inst_step_t step;
    bool        install_ok;
    uint32_t    install_start;
    uint32_t    disk_sectors;
    bool        disk_present;
    char        uname[AUTH_NAME_MAX];   /* #8: account for the installed OS */
    char        pwd  [32];
} g_inst;

static bool stale(void) {
    if (!g_inst.win) return true;
    if (!g_inst.win->in_use) { g_inst.win = NULL; return true; }
    return false;
}

/* ---------- Drawing helpers ---------------------------------------------- */
/* Aero glass push-button (shared widget) so the wizard matches the rest of
 * the system's dialogs. */
static void draw_btn(draw_target_t *t, int x, int y, int w, int h,
                     const char *label, uint32_t bg, bool disabled) {
    gfx_draw_button_aero(t, x, y, w, h, label,
                         disabled ? INST_BTN_DIS : bg, false);
}

static void draw_step_indicator(draw_target_t *t, int x, int y) {
    const char *labels[STEP_COUNT] = {
        L(STR_INST_STEP_WELCOME), L(STR_INST_STEP_DISK),
        L(STR_INST_STEP_CONFIRM), L(STR_INST_STEP_INSTALL),
        L(STR_INST_STEP_DONE),
    };
    int dot_r = 8;
    int dot_gap = 80;
    for (int i = 0; i < STEP_COUNT; i++) {
        bool done   = i < (int)g_inst.step;
        bool active = i == (int)g_inst.step;
        uint32_t col = done ? INST_STEP_DONE : active ? INST_STEP_ACT
                                                      : INST_STEP_INACT;
        int cx = x + i * dot_gap;
        /* Connector first so the pips sit on top; progress tint. */
        if (i < STEP_COUNT - 1)
            gfx_fill_rect(t, cx + dot_r, y - 1, dot_gap - dot_r * 2, 2,
                          done ? INST_STEP_DONE : INST_STEP_INACT);
        /* Glass pip: AA circle + gloss cap + soft rim. */
        gfx_fill_circle(t, cx, y, dot_r, col);
        gfx_fill_circle(t, cx - dot_r / 3, y - dot_r / 3, dot_r / 3,
                        active || done ? 0x66FFFFFFu : 0x50FFFFFFu);
        if (active)
            gfx_fill_circle(t, cx, y, dot_r / 3, 0xFFFFFFFFu);
        int lw = (int)strlen(labels[i]) * FONT_GLYPH_W;
        gfx_draw_string(t, cx - lw / 2, y + dot_r + 4,
                        labels[i], active ? INST_TEXT_FG : INST_TEXT_DIM,
                        0x00000000u);
    }
}

static void draw_progress_bar(draw_target_t *t, int x, int y, int w, int pct) {
    /* Rounded glass track + accent fill with a gloss sweep. */
    gfx_fill_round_rect(t, x, y, w, INST_PROG_H, INST_PROG_H / 2, 0xFFD6DCE6);
    gfx_draw_round_rect(t, x, y, w, INST_PROG_H, INST_PROG_H / 2, INST_PROG_BD);
    int fill = (pct * (w - 2)) / 100;
    if (fill > INST_PROG_H) {
        gfx_fill_round_rect(t, x + 1, y + 1, fill, INST_PROG_H - 2,
                            (INST_PROG_H - 2) / 2, INST_PROG_FILL);
        gfx_blend_round_rect(t, x + 2, y + 2, fill - 2, (INST_PROG_H - 2) / 2,
                             (INST_PROG_H - 2) / 2, 0x50FFFFFFu);
    }
    char buf[8];
    ksnprintf(buf, sizeof(buf), "%d%%", pct);
    gfx_draw_string(t, x + w / 2 - 16, y + (INST_PROG_H - 8) / 2,
                    buf, INST_TEXT_FG, 0x00000000u);
}

static void draw_wrapped(draw_target_t *t, int x, int y, int max_w,
                         const char *text, uint32_t fg, uint32_t bg) {
    /* Simple word-wrap: break on space when width exceeds max_w. */
    char line[128];
    int lw = max_w / 8;
    if (lw < 1) lw = 1;
    int n = 0, ty = y;
    for (const char *p = text; ; p++) {
        if (*p == 0 || *p == '\n' || n >= lw) {
            if (n > 0) {
                line[n] = 0;
                gfx_draw_string(t, x, ty, line, fg, bg);
                ty += 14;
                n = 0;
            }
            if (*p == 0) break;
            if (*p == '\n') continue;
            /* If mid-word, back up to last space. */
        }
        if (n < (int)sizeof(line) - 1) line[n++] = *p;
    }
}

static void redraw(void) {
    if (stale()) return;
    draw_target_t *t = &g_inst.win->content;
    int cw = (int)t->width;
    int ch = (int)t->height;
    (void)ch;

    gfx_fill_rect(t, 0, 0, cw, ch, INST_BG);

    /* Header: glass gradient band (deep navy -> blue) + gloss sweep. */
    for (int y = 0; y < INST_HDR_H; y++) {
        int num = y * 256 / (INST_HDR_H - 1);
        int r = 0x10 + (0x1E - 0x10) * num / 256;
        int g = 0x20 + (0x50 - 0x20) * num / 256;
        int b = 0x58 + (0xA8 - 0x58) * num / 256;
        gfx_fill_rect(t, 0, y, cw, 1,
                      0xFF000000u | ((uint32_t)r << 16) |
                      ((uint32_t)g << 8) | (uint32_t)b);
    }
    gfx_blend_rect(t, 0, 0, cw, INST_HDR_H / 2, 0x1EFFFFFFu);   /* gloss */
    gfx_blend_rect(t, 0, 0, cw, 1, 0x60FFFFFFu);
    gfx_blend_rect(t, 0, INST_HDR_H - 1, cw, 1, 0x40000000u);
    gfx_draw_string_aa_clipped(t, INST_PAD, 10,
                               cw - 2 * INST_PAD, L(STR_INST_TITLE),
                               INST_HDR_FG, 0x00000000u);
    gfx_draw_string(t, INST_PAD, 28, "NexxoN OS Installer  v1.0",
                    0xFFA9BCE8, 0x00000000u);

    /* Step indicator (centered) */
    int si_x = (cw - (STEP_COUNT - 1) * 80) / 2 + 8;
    draw_step_indicator(t, si_x, INST_HDR_H + 22);

    int content_y = INST_HDR_H + 52;
    int btn_y     = ch - INST_BTN_H - INST_PAD;

    /* Horizontal separator */
    gfx_draw_hline(t, INST_PAD, content_y - 4, cw - 2 * INST_PAD, INST_STEP_INACT);
    gfx_draw_hline(t, INST_PAD, btn_y - 8,      cw - 2 * INST_PAD, INST_STEP_INACT);

    switch (g_inst.step) {
    case STEP_WELCOME:
        gfx_draw_string(t, INST_PAD, content_y, L(STR_APP_INSTALLER),
                        INST_STEP_ACT, INST_BG);
        draw_wrapped(t, INST_PAD, content_y + 18, cw - 2 * INST_PAD,
                     L(STR_INST_WELCOME), INST_TEXT_FG, INST_BG);
        draw_btn(t, cw - INST_BTN_W - INST_PAD, btn_y,
                 INST_BTN_W, INST_BTN_H, L(STR_INST_NEXT), INST_BTN_NEXT, false);
        break;

    case STEP_DISK: {
        gfx_draw_string(t, INST_PAD, content_y, L(STR_INST_SELECT_DISK),
                        INST_STEP_ACT, INST_BG);
        int dy = content_y + 20;
        if (g_inst.disk_present) {
            char info[64];
            uint32_t gb = g_inst.disk_sectors / (1024 * 1024 * 2);
            ksnprintf(info, sizeof(info), "SATA disk  %u GB  (%u sectors)",
                      gb, g_inst.disk_sectors);
            gfx_fill_rect(t, INST_PAD, dy, cw - 2 * INST_PAD, 32, 0xFFDCECFF);
            gfx_draw_rect(t, INST_PAD, dy, cw - 2 * INST_PAD, 32, INST_STEP_ACT);
            gfx_draw_string(t, INST_PAD + 8, dy + 12, info, INST_TEXT_FG, 0xFFDCECFF);
        } else {
            gfx_draw_string(t, INST_PAD, dy + 12, L(STR_INST_NO_DISK),
                            0xFFC03030, INST_BG);
        }
        draw_btn(t, INST_PAD, btn_y, INST_BTN_W, INST_BTN_H,
                 L(STR_INST_BACK), INST_BTN_BACK, false);
        draw_btn(t, cw - INST_BTN_W - INST_PAD, btn_y, INST_BTN_W, INST_BTN_H,
                 L(STR_INST_NEXT), INST_BTN_NEXT, !g_inst.disk_present);
        break;
    }

    case STEP_CONFIRM:
        gfx_draw_string(t, INST_PAD, content_y, L(STR_INST_CONFIRM),
                        0xFFC04020, INST_BG);
        draw_wrapped(t, INST_PAD, content_y + 20, cw - 2 * INST_PAD,
                     L(STR_INST_CONFIRM), INST_TEXT_FG, INST_BG);
        draw_btn(t, INST_PAD, btn_y, INST_BTN_W, INST_BTN_H,
                 L(STR_INST_BACK), INST_BTN_BACK, false);
        draw_btn(t, cw - INST_BTN_W - INST_PAD, btn_y, INST_BTN_W, INST_BTN_H,
                 L(STR_INST_INSTALL), INST_BTN_INST, false);
        break;

    case STEP_INSTALLING: {
        gfx_draw_string(t, INST_PAD, content_y, L(STR_INST_PROGRESS),
                        INST_STEP_ACT, INST_BG);
        uint32_t elapsed = pit_ms() - g_inst.install_start;
        int pct = (int)(elapsed / 80);  /* rough ~8s estimate */
        if (pct > 95) pct = 95;
        draw_progress_bar(t, INST_PAD, content_y + 30, cw - 2 * INST_PAD, pct);
        break;
    }

    case STEP_DONE:
        gfx_draw_string(t, INST_PAD, content_y,
                        g_inst.install_ok ? L(STR_INST_DONE) : L(STR_INST_FAILED),
                        g_inst.install_ok ? (uint32_t)0xFF208040 : (uint32_t)0xFFC03030,
                        INST_BG);
        if (g_inst.install_ok) {
            draw_btn(t, cw - INST_BTN_W - INST_PAD, btn_y, INST_BTN_W, INST_BTN_H,
                     "Reboot", INST_BTN_REBOOT, false);
        }
        break;

    default:
        break;
    }

    wm_mark_dirty();
}

/* ---------- Action buttons ----------------------------------------------- */
static bool btn_hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void do_next(void) {
    switch (g_inst.step) {
    case STEP_WELCOME:   g_inst.step = STEP_DISK;  break;
    case STEP_DISK:
        /* #8: before confirming, ask what account the installed system
         * should use.  Cancelling/leaving blank keeps the default
         * admin/admin.  Reuse the localised login labels for the prompts. */
        g_inst.uname[0] = 0;
        g_inst.pwd[0]   = 0;
        if (dialog_input(L(STR_INST_TITLE), i18n("login.username"), "",
                         g_inst.uname, sizeof(g_inst.uname)) &&
            g_inst.uname[0]) {
            dialog_input(L(STR_INST_TITLE), i18n("login.password"), "",
                         g_inst.pwd, sizeof(g_inst.pwd));
        } else {
            g_inst.uname[0] = 0;   /* cancelled -> keep default account */
        }
        g_inst.step = STEP_CONFIRM;
        break;
    case STEP_CONFIRM:
        /* #8: apply the chosen account to the live auth store BEFORE the
         * image is copied to SATA, so the installed system boots with it.
         * Replaces the default admin so the new credentials are the only
         * way in. */
        if (g_inst.uname[0]) {
            if (auth_add_user(g_inst.uname, g_inst.pwd, ROLE_ADMIN)) {
                if (strcmp(g_inst.uname, "admin") != 0)
                    auth_remove_user("admin");
            }
            /* Don't leave the plaintext password lying around in BSS. */
            memset(g_inst.pwd, 0, sizeof(g_inst.pwd));
        }
        /* Start installation. */
        g_inst.step = STEP_INSTALLING;
        g_inst.install_start = pit_ms();
        redraw();
        /* Run the install synchronously. */
        int rc = nxfs_install_to_sata();
        g_inst.install_ok = (rc == 0);
        g_inst.step = STEP_DONE;
        if (g_inst.install_ok)
            notify_post(NOTIFY_SUCCESS, L(STR_INST_TITLE), L(STR_INST_DONE));
        else
            notify_post(NOTIFY_ERROR,   L(STR_INST_TITLE), L(STR_INST_FAILED));
        break;
    case STEP_DONE:
        acpi_reboot();
        break;
    default: break;
    }
    redraw();
}

static void do_back(void) {
    if (g_inst.step == STEP_DISK)    { g_inst.step = STEP_WELCOME; redraw(); }
    if (g_inst.step == STEP_CONFIRM) { g_inst.step = STEP_DISK;    redraw(); }
}

static bool on_click(window_t *w, int mx, int my,
                     uint8_t pressed, uint8_t btn) {
    (void)w; (void)btn;
    if (!pressed) return false;
    draw_target_t *t = &g_inst.win->content;
    int cw = (int)t->width;
    int ch = (int)t->height;
    int btn_y = ch - INST_BTN_H - INST_PAD;

    /* Back button (left side). */
    if (btn_hit(mx, my, INST_PAD, btn_y, INST_BTN_W, INST_BTN_H)) {
        do_back();
        return true;
    }
    /* Next / Install / Reboot button (right side). */
    if (btn_hit(mx, my, cw - INST_BTN_W - INST_PAD, btn_y, INST_BTN_W, INST_BTN_H)) {
        do_next();
        return true;
    }
    return false;
}

static void on_destroy(window_t *w) {
    (void)w;
    g_inst.win = NULL;
}

static void installer_resize_cb(window_t *w) { (void)w; redraw(); }

static void on_lang_change(lang_t l) {
    (void)l;
    redraw();
}

/* ---------- Public API --------------------------------------------------- */
bool installer_open(void) {
    if (!stale()) {
        wm_set_focus(g_inst.win);
        wm_mark_dirty();
        return true;
    }

    /* Probe disk. */
    g_inst.disk_present = sysdisk_present();
    g_inst.disk_sectors = g_inst.disk_present ? sysdisk_sector_count() : 0;
    g_inst.step         = STEP_WELCOME;
    g_inst.install_ok   = false;

    window_t *w = wm_create_window(INST_WIN_X, INST_WIN_Y, INST_WIN_W, INST_WIN_H,
                                   L(STR_APP_INSTALLER));
    if (!w) return false;
    g_inst.win = w;

    wm_set_content_click(w, on_click, NULL);
    wm_set_destroy_cb(w, on_destroy);
    /* Repaint on resize.  Without this the WM leaves the freshly-resized
     * content buffer filled with its neutral grey chrome and the installer
     * appeared to "grey out and stay grey".  redraw() already lays out against
     * t->width/height, so this also makes the installer reflow on resize. */
    wm_set_resize_cb(w, installer_resize_cb);
    wm_set_icon(w, ICON_INSTALLER);
    lang_register_cb(on_lang_change);

    redraw();
    wm_set_focus(w);
    debug_printf("[installer] opened, disk=%d sectors=%u\n",
                 g_inst.disk_present, g_inst.disk_sectors);
    return true;
}

bool installer_active(void) {
    return !stale();
}
