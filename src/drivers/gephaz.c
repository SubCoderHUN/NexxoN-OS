/* ============================================================================
 * NexxoN OS - Gephaz (Settings control panel)  (v2.0, overhauled)
 * ----------------------------------------------------------------------------
 * Four full tabs:
 *
 *   1. Eroforrasok / Performance  - live CPU + RAM line graphs sampled
 *                                   once per second from PIT idle counter
 *                                   and the WM framebuffer pool stats.
 *   2. Halozat / Network          - MAC + IPv4 + Netmask + Gateway + DNS,
 *                                   DHCP toggle, interactive editors for
 *                                   static IP, gateway, primary DNS,
 *                                   secondary DNS.
 *   3. Megjelenites / Themes      - wallpaper reload, theme accent
 *                                   cycler (5 colors), mouse sensitivity
 *                                   slider (1..5), keyboard repeat rate.
 *   4. Tarhely / Storage          - graphical bars for NXFS inode + block
 *                                   usage and (when an AHCI / USB disk
 *                                   shows up) for that volume too.
 *
 * Everything that changes persists via /sys/gephaz.cfg (created on first
 * write).  Existing /sys/desktop.cfg path is untouched.
 * ============================================================================ */
#include "sysdisk.h"
#include "apps.h"
#include "window.h"
#include "icons.h"
#include "gfx.h"
#include "font.h"
#include "vga.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "pit.h"
#include "nxfs.h"
#include "desktop.h"
#include "net.h"
#include "dialogs.h"
#include "ahci.h"
#include "speaker.h"
#include "i18n.h"
#include "auth.h"
#include "theme.h"
#include "vbe_table.h"
#include "notify.h"
#include "edid.h"

#define GH_W          760    /* wide enough for 7 tabs at a readable width */
#define GH_H          480
#define GH_MIN_W      500
#define GH_MIN_H      320
#define GH_BG         0xFFF0F0F4
#define GH_TAB_BG     0xFFD8D8E0
#define GH_TAB_HOT    0xFF1850C8
#define GH_TAB_FG     0xFF202020
#define GH_TAB_FG_H   0xFFFFFFFF
#define GH_HDR        0xFF002878
#define GH_LABEL      0xFF101010
#define GH_VAL_FG     0xFF003898
#define GH_BTN_BG     0xFF208030
#define GH_BTN_BG_H   0xFF30A040
#define GH_BTN_FG     0xFFFFFFFF
#define GH_GRAPH_BG   0xFF101030
#define GH_GRAPH_GRID 0xFF303060
#define GH_CPU_FG     0xFF40FF80
#define GH_RAM_FG     0xFFFFE040
#define GH_BAR_BG     0xFFE0E0E8
#define GH_BAR_FG     0xFF1850C8
#define GH_BAR_HOT    0xFFB02020

#define GH_TAB_H      28
#define GH_NUM_TABS   7
/* Tabs are sized DYNAMICALLY to the window width so they always fit (no
 * overflow past the edge, no overlap), shrinking/growing with a resize. */
static int gh_tab_w(int content_w) {
    int w = (content_w - 16) / GH_NUM_TABS;   /* 8px margin each side */
    if (w < 44)  w = 44;
    if (w > 150) w = 150;
    return w;
}
static int gh_tab_x(int content_w, int idx) { return 8 + idx * gh_tab_w(content_w); }

#define GH_GRAPH_N    96

typedef enum {
    GH_T_SYSTEM   = 0,
    GH_T_PERS     = 1,
    GH_T_SOUND    = 2,
    GH_T_NET      = 3,
    GH_T_SECURITY = 4,
    GH_T_DISPLAY  = 5,
    GH_T_MOUSE    = 6,
} gh_tab_t;

/* Mixer state lives in speaker.c so the taskbar volume widget shares
 * the same source of truth — see audio_get_volume / audio_set_volume. */

extern int  g_mouse_sensitivity;
extern int  g_kbd_repeat_rate;

static window_t *g_gh_win   = NULL;
static gh_tab_t  g_gh_tab   = GH_T_SYSTEM;
/* Kept around so the gephaz.cfg loader can still read/write this value
 * even though the Themes tab is gone. */
static int       g_theme_accent_idx = 0;

/* Per-second rolling history of CPU / RAM samples. */
static uint8_t  g_cpu_hist[GH_GRAPH_N];
static uint8_t  g_ram_hist[GH_GRAPH_N];
static uint32_t g_last_sample_ms = 0;
/* Coarse CPU-busy estimate: percentage of the last 1 s window the
 * shell idle loop *didn't* spend in hlt.  Updated by gephaz_tick(). */
static uint32_t g_busy_window_start = 0;
static uint32_t g_busy_ticks = 0;
static uint32_t g_busy_total = 0;

static void gh_redraw(void);

/* ---- Display tab state (forward-declared for gephaz_tick) ------------ */
/* 32: firmware enums return well past the old cap of 16 (SeaBIOS: 18). */
#define DISP_MAX_MODES  32
/* Keep/revert grace period.  Long enough for a slow monitor to re-sync AND for
 * the user to find + click "Keep" before it auto-reverts (5s was too short to
 * even react). */
#define ROLLBACK_TIMEOUT_MS  12000

typedef struct { uint16_t w, h; } disp_mode_t;

static disp_mode_t g_disp_modes[DISP_MAX_MODES];
static int         g_disp_mode_count  = 0;
static int         g_disp_selected    = 0;
/* Scrolling mode list: firmware enums routinely return 15+ modes (SeaBIOS
 * lists 18) and the old grow-down layout pushed the Apply button below the
 * window edge — unreachable.  The list now scrolls between a fixed top and
 * the bottom-anchored Apply button. */
static int         g_disp_scroll      = 0;     /* first visible row        */
static bool        g_disp_scroll_snap = true;  /* snap to selection once   */
static bool        g_disp_rollback_active = false;
static uint16_t    g_disp_prev_w = 0;
static uint16_t    g_disp_prev_h = 0;
static uint32_t    g_disp_rollback_start = 0;
/* One-shot: re-capture g_disp_rollback_start on the FIRST gephaz_tick after
 * the popup opened.  On bare metal the mode switch + the first full-desktop
 * repaint at the new resolution take real seconds; arming the timer only
 * once the idle loop is back guarantees the user gets the WHOLE grace
 * period, not "grace minus switch time". */
static bool        g_disp_rollback_rearm = false;
static window_t   *g_disp_confirm = NULL;   /* keep/revert popup (its own window) */

static void gh_build_mode_list(void);
static void gh_confirm_open(void);
static void gh_confirm_revert(void);
static void gh_confirm_draw(void);

/* ---- Deferred actions ------------------------------------------------- *
 * Clicks are dispatched from inside wm_tick().  Anything heavy or modal
 * (a blocking DHCP request, a dialog_input prompt, a VBE resolution switch)
 * must NOT run there: a modal dialog would re-enter wm_tick recursively and
 * the VBE real-mode mode-switch must not run nested in the compositor — both
 * hard-freeze the desktop.  Instead the click records WHAT to do here and
 * gephaz_tick() (called from the main idle loop, after wm_tick has returned)
 * performs it in a safe, non-re-entrant context. */
typedef enum {
    GH_PEND_NONE = 0,
    GH_PEND_TOGGLE_DHCP,
    GH_PEND_DHCP_RENEW,
    GH_PEND_SET_IP,
    GH_PEND_SET_GW,
    GH_PEND_SET_DNS,
    GH_PEND_SECDNS_INFO,
    GH_PEND_RES_APPLY,
    GH_PEND_RES_REVERT,
    GH_PEND_CHANGE_PW,
} gh_pending_t;
static gh_pending_t g_gh_pending = GH_PEND_NONE;
static void gh_process_pending(void);

/* ---- /sys/gephaz.cfg persistence ------------------------------------- */
static void gh_save(void) {
    if (!nxfs_is_mounted()) return;
    uint32_t sys_ino;
    if (nxfs_resolve(0, "sys", &sys_ino) != NXFS_OK) {
        if (nxfs_create_dir(0, "sys", &sys_ino) != NXFS_OK) return;
    }
    uint32_t cfg_ino;
    if (nxfs_resolve(sys_ino, "gephaz.cfg", &cfg_ino) != NXFS_OK) {
        if (nxfs_create_file(sys_ino, "gephaz.cfg", &cfg_ino) != NXFS_OK) return;
    }
    char buf[512];
    net_config_t nc;
    net_get_config(&nc);
    char ip[24], gw[24], mask[24], dns[24];
    net_ip_ntoa(nc.ip, ip, sizeof(ip));
    net_ip_ntoa(nc.gateway, gw, sizeof(gw));
    net_ip_ntoa(nc.netmask, mask, sizeof(mask));
    net_ip_ntoa(nc.dns, dns, sizeof(dns));
    int n = ksnprintf(buf, sizeof(buf),
        "theme=%d\nmouse=%d\nkbd=%d\ndhcp=%d\nip=%s\nmask=%s\ngw=%s\ndns=%s\n"
        "lang=%s\nvolume=%d\nmuted=%d\ndispw=%u\ndisph=%u\n",
        g_theme_accent_idx, g_mouse_sensitivity, g_kbd_repeat_rate,
        nc.dhcp ? 1 : 0, ip, mask, gw, dns,
        i18n_get_language() == LANG_HU ? "hu" : "en",
        audio_get_volume(), audio_is_muted() ? 1 : 0,
        vga_width(), vga_height());
    nxfs_write_file(cfg_ino, buf, (uint32_t)n);
    debug_printf("[gephaz] saved %d bytes to /sys/gephaz.cfg\n", n);
}

/* Public wrapper - persists the current settings to disk so subsystems
 * that mutate state outside the Gephaz window (e.g. taskbar volume
 * slider, i18n_set_language from shell/Start Menu) can request a save. */
void gephaz_save_settings(void) { gh_save(); }

/* Load all persisted settings from /sys/gephaz.cfg and apply them to
 * the live subsystems.  Called once at boot, BEFORE the login screen is
 * shown, so the language preference survives a reboot.  Failures are
 * silent — first boot, missing file, malformed entries all leave the
 * defaults intact. */
void gephaz_load_settings(void) {
    if (!nxfs_is_mounted()) return;
    uint32_t sys_ino;
    if (nxfs_resolve(0, "sys", &sys_ino) != NXFS_OK) return;
    uint32_t cfg_ino;
    if (nxfs_resolve(sys_ino, "gephaz.cfg", &cfg_ino) != NXFS_OK) return;

    char buf[1024];
    uint32_t got = 0;
    if (nxfs_read_file(cfg_ino, buf, sizeof(buf) - 1, &got) != NXFS_OK) return;
    if (got == 0) return;
    buf[got] = 0;

    /* #1: restore the saved display resolution after the parse loop. */
    int load_w = 0, load_h = 0;

    char *p = buf;
    while (*p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = 0;
        /* Split key=value. */
        char *eq = p;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = 0;
            const char *key = p;
            const char *val = eq + 1;
            if      (strcmp(key, "theme") == 0)  g_theme_accent_idx = val[0] - '0';
            else if (strcmp(key, "mouse") == 0) {
                int v = 0;
                for (const char *q = val; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0');
                mouse_set_sensitivity(v);   /* clamps to [MIN,MAX] */
            }
            else if (strcmp(key, "kbd")   == 0)  g_kbd_repeat_rate   = val[0] - '0';
            else if (strcmp(key, "lang")  == 0) {
                if (strcmp(val, "hu") == 0) i18n_set_language(LANG_HU);
                else                        i18n_set_language(LANG_EN);
            }
            else if (strcmp(key, "volume") == 0) {
                int v = 0;
                for (const char *q = val; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0');
                audio_set_volume(v);
            }
            else if (strcmp(key, "muted") == 0) {
                audio_set_muted(val[0] == '1');
            }
            else if (strcmp(key, "dispw") == 0 || strcmp(key, "disph") == 0) {
                int v = 0;
                for (const char *q = val; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0');
                if (key[4] == 'w') load_w = v; else load_h = v;
            }
            else if (strcmp(key, "ip") == 0 || strcmp(key, "gw") == 0 ||
                     strcmp(key, "mask") == 0 || strcmp(key, "dns") == 0 ||
                     strcmp(key, "dhcp") == 0) {
                /* Network config - apply if value parses. */
                net_config_t nc;
                net_get_config(&nc);
                if (strcmp(key, "ip") == 0)      nc.ip      = net_ip_aton(val);
                else if (strcmp(key, "gw") == 0) nc.gateway = net_ip_aton(val);
                else if (strcmp(key, "mask") == 0) nc.netmask = net_ip_aton(val);
                else if (strcmp(key, "dns") == 0)  nc.dns     = net_ip_aton(val);
                else if (strcmp(key, "dhcp") == 0) nc.dhcp    = (val[0] == '1');
                net_set_config(&nc);
            }
            if (g_theme_accent_idx < 0 || g_theme_accent_idx > 4) g_theme_accent_idx = 0;
            if (g_kbd_repeat_rate < 0 || g_kbd_repeat_rate > 2)     g_kbd_repeat_rate   = 1;
        }
        *eol = saved;
        if (*eol == '\n') eol++;
        p = eol;
    }

    /* Migrate a stale QEMU-default static config (10.0.2.15) to DHCP: on bare
     * metal that address is never a deliberate choice — only a leftover from
     * an emulator test — and the user expects automatic DHCP. */
    {
        net_config_t nc; net_get_config(&nc);
        if (!nc.dhcp && nc.ip == 0x0F02000Au && nc.gateway == 0x0202000Au) {
            nc.dhcp = true; net_set_config(&nc);
        }
    }

    /* #1: re-apply the persisted display resolution.  Runs at boot before the
     * first wm_present(), so the desktop comes up at the saved size.  Only
     * switch if it differs from the current mode and looks sane. */
    if (load_w >= 640 && load_h >= 480 &&
        ((uint32_t)load_w != vga_width() || (uint32_t)load_h != vga_height())) {
        if (vga_switch_mode((uint16_t)load_w, (uint16_t)load_h)) {
            wm_handle_resolution_change(vga_width(), vga_height());
            debug_printf("[gephaz] restored resolution %dx%d\n", load_w, load_h);
        }
    }
    debug_printf("[gephaz] loaded settings from /sys/gephaz.cfg\n");
}

/* ---- Hit-test helpers ------------------------------------------------- */
static bool point_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}


/* Execute a click-deferred action (see gh_pending_t).  Called from
 * gephaz_tick — i.e. the main idle loop, after wm_tick() has returned — so a
 * modal dialog or the VBE mode switch runs in a safe, non-re-entrant context
 * instead of nested inside the compositor's click dispatch (which froze the
 * desktop).  Blocking DHCP waits stay responsive via the net idle hook. */
static void gh_process_pending(void) {
    gh_pending_t act = g_gh_pending;
    g_gh_pending = GH_PEND_NONE;
    switch (act) {
    case GH_PEND_TOGGLE_DHCP: {
        net_config_t nc; net_get_config(&nc);
        nc.dhcp = !nc.dhcp;
        net_set_config(&nc);
        gh_save();
        if (nc.dhcp) {
            int r = dhcp_request(3000);
            const char *line = (r == 0) ? "DHCP lease acquired."
                : "DHCP request timed out - keeping previous config.";
            const char *info[1] = { line };
            if (r == 0) gh_save();           /* persist the leased address */
            dialog_info("Network", info, 1);
        }
        gh_redraw();
        break;
    }
    case GH_PEND_DHCP_RENEW: {
        int r = dhcp_request(3000);
        const char *line = (r == 0) ? "DHCP lease renewed."
            : "DHCP renewal failed (no server response).";
        const char *info[1] = { line };
        if (r == 0) gh_save();
        dialog_info("Network", info, 1);
        gh_redraw();
        break;
    }
    case GH_PEND_SET_IP: {
        char buf[24]; buf[0] = 0;
        if (dialog_input("Static IP", "Enter IPv4:", "", buf, sizeof(buf))) {
            uint32_t ip = net_ip_aton(buf);
            if (ip) { net_config_t nc; net_get_config(&nc);
                      nc.ip = ip; nc.dhcp = false; net_set_config(&nc); gh_save(); }
        }
        gh_redraw();
        break;
    }
    case GH_PEND_SET_GW: {
        char buf[24]; buf[0] = 0;
        if (dialog_input("Gateway", "Enter gateway IPv4:", "", buf, sizeof(buf))) {
            uint32_t gw = net_ip_aton(buf);
            if (gw) { net_config_t nc; net_get_config(&nc);
                      nc.gateway = gw; net_set_config(&nc); gh_save(); }
        }
        gh_redraw();
        break;
    }
    case GH_PEND_SET_DNS: {
        char buf[24]; buf[0] = 0;
        if (dialog_input("Primary DNS", "Enter DNS IPv4:", "", buf, sizeof(buf))) {
            uint32_t dns = net_ip_aton(buf);
            if (dns) { net_config_t nc; net_get_config(&nc);
                       nc.dns = dns; net_set_config(&nc); gh_save(); }
        }
        gh_redraw();
        break;
    }
    case GH_PEND_SECDNS_INFO: {
        const char *info[1] = {
            "Secondary DNS stored in user-side config only (no live failover yet)." };
        dialog_info("Network", info, 1);
        break;
    }
    case GH_PEND_RES_APPLY: {
        if (g_disp_selected >= 0 && g_disp_selected < g_disp_mode_count) {
            uint16_t nw = g_disp_modes[g_disp_selected].w;
            uint16_t nh = g_disp_modes[g_disp_selected].h;
            g_disp_prev_w = (uint16_t)vga_width();
            g_disp_prev_h = (uint16_t)vga_height();
            bool ok = vga_switch_mode(nw, nh);
            if (ok) {
                wm_handle_resolution_change(vga_width(), vga_height());
                g_disp_rollback_active = true;
                g_disp_rollback_start = pit_ms();
                g_disp_rollback_rearm = true;
                debug_printf("[gephaz] keep/revert armed at %u ms "
                             "(grace %u ms, re-arms on next tick)\n",
                             g_disp_rollback_start,
                             (unsigned)ROLLBACK_TIMEOUT_MS);
                gh_build_mode_list();
                gh_confirm_open();      /* WM popup with the keep/revert prompt */
            } else {
                notify_post(NOTIFY_ERROR, L(STR_DISP_TITLE), L(STR_DISP_SWITCH_FAIL));
            }
            gh_redraw();
        }
        break;
    }
    case GH_PEND_RES_REVERT: {
        g_disp_rollback_active = false;
        vga_switch_mode(g_disp_prev_w, g_disp_prev_h);
        wm_handle_resolution_change(vga_width(), vga_height());
        notify_post(NOTIFY_INFO, L(STR_DISP_TITLE), L(STR_DISP_REVERTED));
        gh_build_mode_list();
        gh_redraw();
        break;
    }
    case GH_PEND_CHANGE_PW: {
        char pwd[32]; pwd[0] = 0;
        if (dialog_input(L(STR_SET_SEC_CHANGE_PW), "Enter new password:", "",
                         pwd, sizeof(pwd))) {
            if (pwd[0]) {
                auth_change_password(auth_current_user(), pwd);
                const char *info[] = { "Password updated." };
                dialog_info(L(STR_SET_SEC_HDR), info, 1);
            }
        }
        break;
    }
    default: break;
    }
}

/* Background tick: called from the shell idle loop so the CPU/RAM
 * graphs animate smoothly even when no input is happening. */
void gephaz_tick(void) {
    uint32_t now = pit_ms();

    /* Run any action a click deferred to us (safe, non-re-entrant context). */
    if (g_gh_pending != GH_PEND_NONE) gh_process_pending();

    /* Automatic DHCP after boot, when configured for DHCP.  Kicked off ~4 s in
     * (after the desktop is up) and driven ASYNCHRONOUSLY: dhcp_async_start()
     * fires a DISCOVER and returns, and dhcp_async_tick() below advances the
     * handshake a little on every idle pass WITHOUT blocking — so the desktop
     * never freezes during the lease (the old synchronous retry stuttered the UI
     * once per round).  On success the leased address is persisted. */
    static bool s_boot_dhcp_started = false;
    if (!s_boot_dhcp_started && now > 4000) {
        s_boot_dhcp_started = true;
        net_config_t nc; net_get_config(&nc);
        if (nc.dhcp) dhcp_async_start();
    }
    if (dhcp_async_tick() == 1) gh_save();      /* lease just bound -> persist */

    if (g_disp_rollback_active) {
        if (g_disp_rollback_rearm) {
            /* First idle pass since the switch: the WM has composited the
             * new resolution at least once, so the grace period starts NOW
             * — switch/repaint time no longer eats into it.  Re-read the
             * clock: `now` above was sampled BEFORE gh_process_pending(),
             * i.e. before the (on bare metal: seconds-long) mode switch ran
             * in this very tick — using it here would backdate the timer by
             * the full switch time. */
            g_disp_rollback_rearm = false;
            now = pit_ms();
            g_disp_rollback_start = now;
            debug_printf("[gephaz] keep/revert re-armed at %u ms\n", now);
        }
        int32_t elapsed = (int32_t)(now - g_disp_rollback_start);
        if (elapsed < 0) {
            /* Clock anomaly (tick counter disturbed by a BIOS call): an
             * unsigned wrap here used to read as "huge elapsed" and revert
             * instantly.  Re-arm instead of reverting on garbage. */
            debug_printf("[gephaz] keep/revert clock went backwards "
                         "(now=%u start=%u) — re-arming\n",
                         now, g_disp_rollback_start);
            g_disp_rollback_start = now;
            elapsed = 0;
        }
        if (elapsed >= (int32_t)ROLLBACK_TIMEOUT_MS) {
            debug_printf("[gephaz] keep/revert grace elapsed "
                         "(now=%u start=%u) — auto-revert\n",
                         now, g_disp_rollback_start);
            gh_confirm_revert();        /* grace period elapsed -> auto-revert */
        } else {
            gh_confirm_draw();          /* refresh the live countdown */
        }
    }

    if (g_busy_window_start == 0) g_busy_window_start = now;
    g_busy_total++;
    if (g_busy_total > 1000) g_busy_ticks++;  /* trivial proxy            */
    if (now - g_last_sample_ms < 1000) return;
    g_last_sample_ms = now;
    /* Performance tab was removed; we still keep the lightweight history
     * sampling so other system surfaces (Task Manager) can re-use the
     * arrays in a future epoch. */
    g_busy_window_start = now;
    g_busy_total = 0;
    g_busy_ticks = 0;
    (void)g_cpu_hist; (void)g_ram_hist;
}

/* ---- Local colour interpolation --------------------------------------- */
static uint32_t gh_lerp_c(uint32_t a, uint32_t b, int n, int d) {
    if (d <= 0) return a;
    int ra = (int)((a >> 16) & 0xFF), rb = (int)((b >> 16) & 0xFF);
    int ga = (int)((a >>  8) & 0xFF), gb = (int)((b >>  8) & 0xFF);
    int ba = (int)( a        & 0xFF), bb = (int)( b        & 0xFF);
    return 0xFF000000u
         | ((uint32_t)(ra + (rb - ra) * n / d) << 16)
         | ((uint32_t)(ga + (gb - ga) * n / d) <<  8)
         |  (uint32_t)(ba + (bb - ba) * n / d);
}

/* Soft vertical frost gradient body — same family as the shared dialogs. */
static void gh_paint_body(draw_target_t *t) {
    int h = (int)t->height, w = (int)t->width;
    for (int y = 0; y < h; y++) {
        int num = (h > 1) ? y * 256 / (h - 1) : 0;
        int r = 0xF4 + (0xE4 - 0xF4) * num / 256;
        int g = 0xF6 + (0xE9 - 0xF6) * num / 256;
        int b = 0xFA + (0xF1 - 0xFA) * num / 256;
        gfx_fill_rect(t, 0, y, w, 1,
                      0xFF000000u | ((uint32_t)r << 16) |
                      ((uint32_t)g << 8) | (uint32_t)b);
    }
}

/* Settings card: a white rounded panel with a soft drop shadow and a bold
 * section title above it — the modern "settings card" look.  Geometry is
 * compatible with the old etched group box (same x/y/w/h envelope). */
static void gh_draw_group(draw_target_t *t, int x, int y, int w, int h,
                          const char *title) {
    gfx_draw_string(t, x + 2, y - 1, title, GH_HDR, 0x00000000u);
    int cy = y + FONT_GLYPH_H + 4;
    int ch = h - FONT_GLYPH_H - 4;
    gfx_fill_round_rect(t, x, cy, w, ch, 8, 0xFFFFFFFFu);
    gfx_blend_round_rect(t, x + 1, cy + 1, w - 2, ch / 3, 8, 0x2EF4F8FFu);
    gfx_draw_round_rect(t, x, cy, w, ch, 8, 0xFFD9DFE8u);
    /* Gentle lift: two soft shade lines under the card, nothing more. */
    gfx_blend_rect(t, x + 3, cy + ch,     w - 6, 1, 0x16000000u);
    gfx_blend_rect(t, x + 4, cy + ch + 1, w - 8, 1, 0x0A000000u);
}

/* Styled slider: the shared Aero glass slider (identical to the tray volume
 * control) so every slider in the system looks the same. */
static void gh_draw_styled_slider(draw_target_t *t, int x, int y, int w, int h,
                                  int min_v, int max_v, int cur_v) {
    int permil = (max_v > min_v)
               ? (cur_v - min_v) * 1000 / (max_v - min_v) : 0;
    int sh = 20;                        /* slider height = thumb diameter  */
    gfx_draw_slider_aero(t, x, y + (h - sh) / 2, w, sh, permil, GLASS_ACCENT);
}

/* ---- Drawing helpers -------------------------------------------------- */
static void gh_draw_button(draw_target_t *t, int x, int y, int w, int h,
                           const char *label, uint32_t bg) {
    /* Hover-test against the window this draw target belongs to.  The
     * keep/revert popup is its own WM window: testing the Settings window's
     * coordinates for it highlighted the wrong rectangle — and dereferenced
     * g_gh_win unconditionally. */
    window_t *win = g_gh_win;
    if (g_disp_confirm && t == &g_disp_confirm->content) win = g_disp_confirm;
    bool hot = false;
    if (win && win->in_use) {
        int mx = mouse_x(), my = mouse_y();
        int abs_x = win->x + WM_BORDER + x;
        int abs_y = win->y + WM_BORDER + WM_TITLE_H + 2 + y;
        hot = (mx >= abs_x && mx < abs_x + w &&
               my >= abs_y && my < abs_y + h);
    }
    gfx_draw_button_aero(t, x, y, w, h, label, bg, hot);
}

static void gh_draw_label_line(draw_target_t *t, int x, int y,
                               const char *label, const char *value) {
    gfx_draw_string(t, x, y, label, GH_LABEL, 0x00000000u);
    gfx_draw_string(t, x + 140, y, value, GH_VAL_FG, 0x00000000u);
}

static void gh_draw_tabs(draw_target_t *t) {
    const char *tabs[GH_NUM_TABS] = {
        i18n_or("tab.system",  "System"),
        L(STR_SETTINGS_TAB_THEME),      /* Personalization */
        L(STR_SETTINGS_TAB_AUDIO),      /* Sound           */
        L(STR_SETTINGS_TAB_NET),        /* Network         */
        L(STR_SETTINGS_TAB_SECURITY),   /* Security        */
        L(STR_SETTINGS_TAB_DISPLAY),    /* Display         */
        L(STR_SETTINGS_TAB_MOUSE),      /* Mouse & Keyboard */
    };
    int tw = gh_tab_w((int)t->width);
    for (int i = 0; i < GH_NUM_TABS; i++) {
        int tx = gh_tab_x((int)t->width, i);
        gfx_draw_tab_aero(t, tx, 4, tw, GH_TAB_H, tabs[i], GLASS_ACCENT,
                          i == (int)g_gh_tab, false);
    }
    gfx_blend_rect(t, 0, 4 + GH_TAB_H + 1, (int)t->width, 1, GLASS_EDGE_DARK);
}

/* ---- Tab 1: System --------------------------------------------------- */
static void gh_draw_system(draw_target_t *t) {
    gfx_draw_string_aa(t, 16, 50,
                       i18n_or("set.sys.hdr", "System Information"),
                       GH_HDR, 0x00000000u);

    gh_draw_group(t, 16, 66, (int)t->width - 32, 110,
                  i18n_or("set.sys.about", "About this computer"));

    int y = 86;
    char line[80];
    gh_draw_label_line(t, 28, y,
                       i18n_or("set.sys.os",   "OS:"),
                       "NexxoN OS");               y += 16;
    gh_draw_label_line(t, 28, y,
                       i18n_or("set.sys.ver",  "Version:"),
                       "2.0 (Aero Glass)");        y += 16;
    gh_draw_label_line(t, 28, y,
                       i18n_or("set.sys.arch", "Arch:"),
                       "x86_64 (64-bit)");       y += 16;
    ksnprintf(line, sizeof(line), "%u x %u x 32 bpp",
              vga_width(), vga_height());
    gh_draw_label_line(t, 28, y,
                       i18n_or("set.sys.disp", "Display:"), line); y += 16;
    gh_draw_label_line(t, 28, y,
                       i18n_or("set.sys.build", "Build:"),
                       __DATE__);
}

/* ---- Tab 2: Personalization ------------------------------------------ */
static void gh_draw_personalization(draw_target_t *t) {
    /* #9: this tab is now "Language / Nyelv" only -- the theme-accent and
     * wallpaper selectors were removed here (they live elsewhere / are not
     * exposed in this menu).  STR_SETTINGS_TAB_THEME is localised to
     * "Language"/"Nyelv". */
    gfx_draw_string_aa(t, 16, 50, L(STR_SETTINGS_TAB_THEME), GH_HDR, 0x00000000u);

    /* Language group */
    gh_draw_group(t, 16, 66, (int)t->width - 32, 72,
                  L(STR_SET_SEC_LANG));
    char lang_line[64];
    ksnprintf(lang_line, sizeof(lang_line), "%s:  %s",
              L(STR_SET_SEC_LANG),
              (i18n_get_language() == LANG_HU) ? "Magyar" : "English");
    gfx_draw_string(t, 28, 84, lang_line, GH_LABEL, 0x00000000u);
    gh_draw_button(t, 28,  104, 140, 26, L(STR_SET_SEC_SWITCH_EN), GH_BTN_BG);
    gh_draw_button(t, 184, 104, 140, 26, L(STR_SET_SEC_SWITCH_HU), GH_BTN_BG);
}

/* ---- Tab 7: Mouse & Keyboard ---------------------------------------- *
 * Pointer sensitivity slider.  Widths follow t->width so the group + slider
 * stretch with the window instead of staying a fixed size. */
static void gh_draw_mouse(draw_target_t *t) {
    gfx_draw_string_aa(t, 16, 50, L(STR_SETTINGS_TAB_MOUSE), GH_HDR, 0x00000000u);

    int gw  = (int)t->width - 32;
    int my0 = 66;
    gh_draw_group(t, 16, my0, gw, 86, L(STR_GH_MOUSE_SEC));
    char ms_line[64];
    ksnprintf(ms_line, sizeof(ms_line), "%s:  %d / %d",
              L(STR_GH_MOUSE_SENS), mouse_get_sensitivity(), MOUSE_SENS_MAX);
    gfx_draw_string(t, 28, my0 + 18, ms_line, GH_LABEL, 0x00000000u);
    int sx = 28, sw = gw - 24;
    gh_draw_styled_slider(t, sx, my0 + 40, sw, 24,
                          MOUSE_SENS_MIN, MOUSE_SENS_MAX, mouse_get_sensitivity());
    gfx_draw_string(t, sx, my0 + 66, L(STR_GH_SLOWER), GH_LABEL, 0x00000000u);
    int fw = gfx_string_pixel_width(L(STR_GH_FASTER));
    gfx_draw_string(t, sx + sw - fw, my0 + 66, L(STR_GH_FASTER), GH_LABEL, 0x00000000u);
}

/* ---- Tab 3: Network -------------------------------------------------- */
static void gh_draw_network(draw_target_t *t) {
    net_config_t nc;
    net_get_config(&nc);
    char ip[24], gw[24], mask[24], dns[24], mac[24];
    net_ip_ntoa(nc.ip, ip, sizeof(ip));
    net_ip_ntoa(nc.gateway, gw, sizeof(gw));
    net_ip_ntoa(nc.netmask, mask, sizeof(mask));
    net_ip_ntoa(nc.dns, dns, sizeof(dns));
    ksnprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
              nc.mac[0], nc.mac[1], nc.mac[2],
              nc.mac[3], nc.mac[4], nc.mac[5]);

    gfx_draw_string_aa(t, 16, 50, L(STR_SET_NET_HDR), GH_HDR, 0x00000000u);

    int y = 78;
    /* Label/value rows.  The colon is fixed punctuation so we keep it
     * in the format string; only the label noun is localised. */
    char lbl[24];
    ksnprintf(lbl, sizeof(lbl), "%-12s:", L(STR_SET_NET_MAC));
    gh_draw_label_line(t, 24, y, lbl, mac); y += 16;
    ksnprintf(lbl, sizeof(lbl), "%-12s:", L(STR_SET_NET_IPV4));
    gh_draw_label_line(t, 24, y, lbl, ip);  y += 16;
    ksnprintf(lbl, sizeof(lbl), "%-12s:", L(STR_SET_NET_NETMASK));
    gh_draw_label_line(t, 24, y, lbl, mask); y += 16;
    ksnprintf(lbl, sizeof(lbl), "%-12s:", L(STR_SET_NET_GATEWAY));
    gh_draw_label_line(t, 24, y, lbl, gw);  y += 16;
    ksnprintf(lbl, sizeof(lbl), "%-12s:", L(STR_SET_NET_DNS));
    gh_draw_label_line(t, 24, y, lbl, dns); y += 16;
    ksnprintf(lbl, sizeof(lbl), "%-12s:", L(STR_SET_NET_MODE));
    gh_draw_label_line(t, 24, y, lbl,
                       nc.dhcp ? L(STR_SET_NET_DHCP) : L(STR_SET_NET_STATIC));
    y += 24;

    gh_draw_button(t, 24,  y, 200, 26,
                   nc.dhcp ? L(STR_SET_NET_BTN_DHCP_OFF)
                           : L(STR_SET_NET_BTN_DHCP_ON), GH_BTN_BG);
    gh_draw_button(t, 240, y, 160, 26, L(STR_SET_NET_BTN_RENEW), GH_BTN_BG);
    y += 36;

    gh_draw_button(t, 24,  y, 200, 26, L(STR_SET_NET_BTN_SET_IP),  GH_BTN_BG);
    gh_draw_button(t, 240, y, 160, 26, L(STR_SET_NET_BTN_SET_GW),  GH_BTN_BG);
    y += 36;

    gh_draw_button(t, 24,  y, 200, 26, L(STR_SET_NET_BTN_SET_DNS),  GH_BTN_BG);
    gh_draw_button(t, 240, y, 200, 26, L(STR_SET_NET_BTN_SET_DNS2), GH_BTN_BG);
}

/* ---- Slider helper (still used by the Audio tab) -------------------- */
static void gh_draw_slider(draw_target_t *t, int x, int y, int w, int h,
                           int min_v, int max_v, int cur_v) {
    gfx_fill_rect(t, x, y, w, h, GH_BAR_BG);
    gfx_draw_rect(t, x, y, w, h, 0xFF000000);
    if (max_v == min_v) return;
    int fill_w = (cur_v - min_v) * w / (max_v - min_v);
    gfx_fill_rect(t, x + 1, y + 1, fill_w - 2, h - 2, GH_BAR_FG);
    /* Tick marks. */
    for (int v = min_v; v <= max_v; v++) {
        int tx = x + (v - min_v) * w / (max_v - min_v);
        gfx_draw_vline(t, tx, y + h, 4, 0xFF000000);
    }
}

/* ---- Tab 4: Storage -------------------------------------------------- */
static void gh_draw_bar(draw_target_t *t, int x, int y, int w, int h,
                        uint32_t used, uint32_t total,
                        const char *label) {
    gfx_draw_string(t, x, y, label, GH_LABEL, 0x00000000u);
    gfx_fill_rect(t, x, y + 14, w, h, GH_BAR_BG);
    gfx_draw_rect(t, x, y + 14, w, h, 0xFF000000);
    if (total) {
        /* Pure 32-bit arithmetic: avoid 64-bit divides that would
         * require __udivdi3 in this freestanding build.  Scale by
         * shifting whenever a 32-bit multiply would overflow. */
        uint32_t u_s = used, t_s = total;
        while (u_s > 0xFFFFFFu) { u_s >>= 1; t_s >>= 1; }
        if (t_s == 0) t_s = 1;
        uint32_t fw = (u_s * (uint32_t)(w - 2)) / t_s;
        if (fw > (uint32_t)(w - 2)) fw = (uint32_t)(w - 2);
        uint32_t pct = (uint32_t)((u_s * 100u) / t_s);
        uint32_t col = pct > 85 ? GH_BAR_HOT : GH_BAR_FG;
        gfx_fill_rect(t, x + 1, y + 15, (int)fw, h - 2, col);
        char num[64];
        ksnprintf(num, sizeof(num), "%u / %u  (%u%%)", used, total, pct);
        gfx_draw_string(t, x + w + 12, y + 14 + (h - FONT_GLYPH_H) / 2,
                        num, GH_LABEL, 0x00000000u);
    }
}

static void gh_draw_storage(draw_target_t *t) {
    gfx_draw_string_aa(t, 16, 50, L(STR_SET_STORE_HDR), GH_HDR, 0x00000000u);
    int y = 80;

    uint32_t iused, bused;
    nxfs_stats(&iused, &bused);
    gh_draw_bar(t, 24, y, 280, 18, iused, 1024, L(STR_SET_STORE_INODES));
    y += 50;
    gh_draw_bar(t, 24, y, 280, 18, bused, 4096, L(STR_SET_STORE_BLOCKS));
    y += 50;

    if (sysdisk_present()) {
        uint32_t sec = sysdisk_sector_count();
        char l[80];
        ksnprintf(l, sizeof(l), "%s  %u MiB total",
                  L(STR_SET_STORE_SATA), sec / 2048u);
        gfx_draw_string(t, 24, y, l, GH_LABEL, 0x00000000u);
        /* Real v3 usage: used blocks * 8 sectors + metadata head. */
        uint32_t ino_used = 0, blk_used = 0;
        nxfs_stats(&ino_used, &blk_used);
        uint32_t used_sec = nxfs_data_start() +
                            blk_used * NXFS_BLOCK_SECTORS;
        gh_draw_bar(t, 24, y + 14, 280, 18, used_sec, sec, "");
        y += 60;
    }

    gfx_draw_string(t, 16, (int)t->height - 20,
                    L(STR_SET_STORE_TIP), 0xFF606080, 0x00000000u);
}

/* ---- Tab 4: Sound ----------------------------------------------------- */
static void gh_draw_audio(draw_target_t *t) {
    gfx_draw_string_aa(t, 16, 50, L(STR_SET_AUDIO_HDR), GH_HDR, 0x00000000u);

    int vol_now = audio_get_volume();
    bool muted  = audio_is_muted();

    /* Volume group with styled slider */
    gh_draw_group(t, 16, 66, (int)t->width - 32, 80,
                  L(STR_SET_AUDIO_VOLUME));
    char vol[80];
    ksnprintf(vol, sizeof(vol), "%s:  %d%%%s%s",
              L(STR_SET_AUDIO_VOLUME), vol_now,
              muted ? "  " : "", muted ? L(STR_SET_AUDIO_MUTED) : "");
    gfx_draw_string(t, 28, 84, vol, GH_LABEL, 0x00000000u);
    /* Styled slider: 4 px groove, 12×20 glossy thumb */
    gh_draw_styled_slider(t, 28, 102, 360, 28, 0, 100, vol_now);

    /* Controls */
    gh_draw_button(t, 28,  158, 140, 26,
                   muted ? L(STR_BTN_UNMUTE) : L(STR_BTN_MUTE), GH_BTN_BG);
    gh_draw_button(t, 184, 158, 140, 26, L(STR_BTN_TEST),        GH_BTN_BG);
    gh_draw_button(t, 340, 158, 110, 26, L(STR_BTN_VOLUME_UP),   GH_BTN_BG);
    gh_draw_button(t, 28,  194, 140, 26, L(STR_BTN_VOLUME_DOWN), GH_BTN_BG);
}

/* ---- Tab 5: Security ------------------------------------------------- */
static void gh_draw_security(draw_target_t *t) {
    gfx_draw_string_aa(t, 16, 50, L(STR_SET_SEC_HDR), GH_HDR, 0x00000000u);

    gh_draw_group(t, 16, 66, (int)t->width - 32, 100,
                  L(STR_SET_SEC_HDR));

    const char *user = auth_current_user();
    auth_role_t role = auth_current_role();
    char line[96];
    int y = 86;
    ksnprintf(line, sizeof(line), "%s :  %s",
              L(STR_SET_SEC_USER),
              user && user[0] ? user : "(none)");
    gfx_draw_string(t, 28, y, line, GH_LABEL, 0x00000000u); y += 16;
    ksnprintf(line, sizeof(line), "%s :  %s",
              L(STR_SET_SEC_ROLE),
              role == ROLE_ADMIN ? L(STR_SET_SEC_ROLE_ADMIN)
                                 : L(STR_SET_SEC_ROLE_USER));
    gfx_draw_string(t, 28, y, line, GH_LABEL, 0x00000000u); y += 16;
    ksnprintf(line, sizeof(line), "%s :  %d",
              L(STR_SET_SEC_TOTAL), auth_user_count());
    gfx_draw_string(t, 28, y, line, GH_LABEL, 0x00000000u);

    int btn_y = 176;
    gh_draw_button(t, 28,  btn_y, 200, 26, L(STR_SET_SEC_OPEN_USERS), GH_BTN_BG);
    gh_draw_button(t, 244, btn_y, 200, 26, L(STR_SET_SEC_CHANGE_PW),  GH_BTN_BG);

    gfx_draw_string(t, 16, btn_y + 44,
                    i18n_or("set.sec.tip",
                            "Credentials are hashed in /sys/users.cfg."),
                    0xFF606080, 0x00000000u);
}

/* ---- Tab 6: Display -------------------------------------------------- */

static void gh_build_mode_list(void) {
    g_disp_mode_count = 0;
    int n = vbe_table_count();
    for (int i = 0; i < n && g_disp_mode_count < DISP_MAX_MODES; i++) {
        const vbe_mode_entry_t *e = vbe_table_get(i);
        if (!e || e->bpp != 32) continue;
        if (e->width < 800 || e->height < 600) continue;
        bool dup = false;
        for (int j = 0; j < g_disp_mode_count; j++) {
            if (g_disp_modes[j].w == e->width &&
                g_disp_modes[j].h == e->height) { dup = true; break; }
        }
        if (dup) continue;
        g_disp_modes[g_disp_mode_count].w = e->width;
        g_disp_modes[g_disp_mode_count].h = e->height;
        g_disp_mode_count++;
    }
    uint16_t cw = (uint16_t)vga_width();
    uint16_t ch = (uint16_t)vga_height();
    for (int i = 0; i < g_disp_mode_count; i++) {
        if (g_disp_modes[i].w == cw && g_disp_modes[i].h == ch)
            g_disp_selected = i;
    }
    g_disp_scroll_snap = true;     /* bring the selection into view */
}

/* Display-tab list geometry, shared by draw + click + wheel so they can
 * never disagree.  Everything keys off the live content size: the Apply
 * button anchors to the window bottom and the list scrolls in between. */
#define DISP_LIST_Y   106
#define DISP_ROW_H    22

static int gh_disp_apply_y(const draw_target_t *t) {
    int y = (int)t->height - 28 - 10;
    if (y < DISP_LIST_Y + DISP_ROW_H + 12) y = DISP_LIST_Y + DISP_ROW_H + 12;
    return y;
}
static int gh_disp_visible_rows(const draw_target_t *t) {
    int n = (gh_disp_apply_y(t) - 12 - DISP_LIST_Y) / DISP_ROW_H;
    if (n < 1) n = 1;
    return n;
}
static void gh_disp_clamp_scroll(const draw_target_t *t) {
    int max_scroll = g_disp_mode_count - gh_disp_visible_rows(t);
    if (max_scroll < 0) max_scroll = 0;
    if (g_disp_scroll > max_scroll) g_disp_scroll = max_scroll;
    if (g_disp_scroll < 0) g_disp_scroll = 0;
}

static void gh_draw_display(draw_target_t *t) {
    if (g_disp_mode_count == 0) gh_build_mode_list();

    gfx_draw_string_aa(t, 16, 50, L(STR_DISP_TITLE), GH_HDR, 0x00000000u);

    int apply_y = gh_disp_apply_y(t);
    int visible = gh_disp_visible_rows(t);
    if (g_disp_scroll_snap) {
        /* One-shot after a (re)build: bring the selected row into view. */
        g_disp_scroll_snap = false;
        if (g_disp_selected < g_disp_scroll)
            g_disp_scroll = g_disp_selected;
        else if (g_disp_selected >= g_disp_scroll + visible)
            g_disp_scroll = g_disp_selected - visible + 1;
    }
    gh_disp_clamp_scroll(t);

    gh_draw_group(t, 16, 66, (int)t->width - 32, apply_y - 12 - 66,
                  L(STR_DISP_RESOLUTION));

    char buf[64];
    ksnprintf(buf, sizeof(buf), "%s  %ux%u",
              L(STR_DISP_CURRENT), (unsigned)vga_width(), (unsigned)vga_height());
    gfx_draw_string(t, 28, 86, buf, GH_LABEL, 0x00000000u);

    if (g_disp_mode_count == 0) {
        gfx_draw_string(t, 28, DISP_LIST_Y, L(STR_DISP_NO_MODES), 0xFFC03030, 0x00000000u);
        return;
    }

    int y = DISP_LIST_Y;
    int last = g_disp_scroll + visible;
    if (last > g_disp_mode_count) last = g_disp_mode_count;
    for (int i = g_disp_scroll; i < last; i++) {
        bool sel = (i == g_disp_selected);
        uint32_t bg = sel ? 0xFF1870D0 : 0xFFE0E0E8;
        uint32_t fg = sel ? 0xFFFFFFFF : GH_LABEL;
        int row_x = 28, row_w = (int)t->width - 72;
        gfx_fill_rect(t, row_x, y, row_w, 20, bg);
        gfx_draw_rect(t, row_x, y, row_w, 20, 0xFFA0A8B0);

        bool current = (g_disp_modes[i].w == (uint16_t)vga_width() &&
                        g_disp_modes[i].h == (uint16_t)vga_height());
        ksnprintf(buf, sizeof(buf), "  %ux%u%s",
                  g_disp_modes[i].w, g_disp_modes[i].h,
                  current ? "  *" : "");
        gfx_draw_string(t, row_x + 4, y + 6, buf, fg, bg);
        y += DISP_ROW_H;
    }

    /* Clipped-list indicators: clickable one-row scroll arrows pinned to
     * the list's right edge (the wheel scrolls too).  Drawn from glyphs,
     * no localisable text. */
    if (g_disp_scroll > 0)
        gh_draw_button(t, (int)t->width - 40, DISP_LIST_Y, 16, 16, "^",
                       GH_TAB_BG);
    if (last < g_disp_mode_count)
        gh_draw_button(t, (int)t->width - 40,
                       DISP_LIST_Y + visible * DISP_ROW_H - 18, 16, 16, "v",
                       GH_TAB_BG);

    gh_draw_button(t, (int)t->width - 32 - 120, apply_y, 120, 28,
                   L(STR_DISP_APPLY), 0xFF1870D0);

    /* The keep/revert prompt is no longer painted into the Settings content —
     * it is its own WM window (gh_confirm_*), like every other system popup. */
}

/* ---- Resolution keep/revert popup (a real WM-managed window) --------- *
 * After a switch the user confirms within ROLLBACK_TIMEOUT_MS or it auto-
 * reverts (safety net for an unreadable mode).  Modal WM window so it looks +
 * behaves like the other dialogs; the countdown is ticked from gephaz_tick. */
static void gh_confirm_draw(void) {
    if (!g_disp_confirm || !g_disp_confirm->in_use) return;
    draw_target_t *t = &g_disp_confirm->content;
    gfx_clear(t, GH_BG);
    int32_t el = (int32_t)(pit_ms() - g_disp_rollback_start);
    if (el < 0) el = 0;                       /* clock anomaly: show full grace */
    if (el > (int32_t)ROLLBACK_TIMEOUT_MS) el = ROLLBACK_TIMEOUT_MS;
    int remain = (int)((ROLLBACK_TIMEOUT_MS - (uint32_t)el) / 1000);
    char b[96];
    gfx_draw_string_aa(t, 14, 12, L(STR_DISP_KEEP_TITLE), GH_HDR, 0x00000000u);
    ksnprintf(b, sizeof b, "%ux%u", (unsigned)vga_width(), (unsigned)vga_height());
    gfx_draw_string(t, 14, 38, b, GH_LABEL, 0x00000000u);
    ksnprintf(b, sizeof b, L(STR_DISP_KEEP_MSG), remain);
    gfx_draw_string(t, 14, 56, b, GH_LABEL, 0x00000000u);
    int bh = 28, by = (int)t->height - bh - 12;
    gh_draw_button(t, 14, by, 130, bh, L(STR_DISP_KEEP_BTN), 0xFF208040);
    gh_draw_button(t, (int)t->width - 14 - 110, by, 110, bh,
                   L(STR_DISP_REVERT_BTN), 0xFFC04020);
    wm_mark_dirty();
}

static void gh_confirm_close(void) {
    if (g_disp_confirm) {
        window_t *w = g_disp_confirm;
        g_disp_confirm = NULL;
        wm_pop_modal(w);
        if (w->in_use) wm_destroy_window(w);
    }
    g_disp_rollback_active = false;
    g_disp_rollback_rearm  = false;
}

static void gh_confirm_keep(void) {
    gh_confirm_close();
    gh_save();                          /* persist the confirmed resolution */
    notify_post(NOTIFY_SUCCESS, L(STR_DISP_TITLE), L(STR_DISP_SWITCH_OK));
    gh_redraw();
}

static void gh_confirm_revert(void) {
    gh_confirm_close();
    vga_switch_mode(g_disp_prev_w, g_disp_prev_h);
    wm_handle_resolution_change(vga_width(), vga_height());
    notify_post(NOTIFY_INFO, L(STR_DISP_TITLE), L(STR_DISP_REVERTED));
    gh_build_mode_list();
    gh_redraw();
}

static bool gh_confirm_click(window_t *w, int cx, int cy, uint8_t pressed, uint8_t btn) {
    (void)btn;
    if (w != g_disp_confirm) return false;
    if (!(pressed & MOUSE_BTN_LEFT)) return true;
    draw_target_t *t = &w->content;
    int bh = 28, by = (int)t->height - bh - 12;
    if (point_in(cx, cy, 14, by, 130, bh))                       { gh_confirm_keep();   return true; }
    if (point_in(cx, cy, (int)t->width - 14 - 110, by, 110, bh)) { gh_confirm_revert(); return true; }
    return true;
}

static void gh_confirm_on_destroy(window_t *w) {
    (void)w;
    /* Closed via the X — the screen is clearly usable, so keep the new mode. */
    if (g_disp_confirm) {
        g_disp_confirm = NULL;
        g_disp_rollback_active = false;
        g_disp_rollback_rearm  = false;
        gh_save();
    }
}

static void gh_confirm_open(void) {
    int sw = (int)vga_width(), sh = (int)vga_height();
    int w = 380, h = 168;
    int x = (sw - w) / 2, y = (sh - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_disp_confirm = wm_create_window(x, y, w, h, L(STR_DISP_KEEP_TITLE));
    if (!g_disp_confirm) { g_disp_rollback_active = false; return; }
    wm_set_resizable(g_disp_confirm, false, 0, 0);
    wm_set_content_click(g_disp_confirm, gh_confirm_click, NULL);
    wm_set_destroy_cb(g_disp_confirm, gh_confirm_on_destroy);
    wm_push_modal(g_disp_confirm);
    wm_set_focus(g_disp_confirm);
    gh_confirm_draw();
}

/* ---- Click dispatch -------------------------------------------------- */
static bool gh_click(window_t *w, int cx, int cy,
                     uint8_t pressed, uint8_t btn) {
    (void)btn;
    if (w != g_gh_win) return false;
    if (!(pressed & MOUSE_BTN_LEFT)) return true;

    if (cy >= 4 && cy < 4 + GH_TAB_H) {
        int cw = (int)w->content.width;
        int tw = gh_tab_w(cw);
        for (int i = 0; i < GH_NUM_TABS; i++) {
            int tx = gh_tab_x(cw, i);
            if (cx >= tx && cx < tx + tw) {
                g_gh_tab = (gh_tab_t)i;
                gh_redraw();
                return true;
            }
        }
    }

    if (g_gh_tab == GH_T_PERS) {
        /* #9: only the language switch remains on this tab; the accent +
         * wallpaper controls were removed. */
        if (point_in(cx, cy, 28, 104, 140, 26)) {
            i18n_set_language(LANG_EN);
            gh_save(); gh_redraw(); return true;
        }
        if (point_in(cx, cy, 184, 104, 140, 26)) {
            i18n_set_language(LANG_HU);
            gh_save(); gh_redraw(); return true;
        }
    }

    if (g_gh_tab == GH_T_MOUSE) {
        /* Pointer sensitivity slider — geometry MUST match gh_draw_mouse:
         * sx=28, sw=(content_w-32)-24, y=66+40=106, h=24. */
        int sx = 28, sw = (int)w->content.width - 32 - 24;
        if (sw > 8 && point_in(cx, cy, sx, 106, sw, 24)) {
            int range = MOUSE_SENS_MAX - MOUSE_SENS_MIN;
            int nv = MOUSE_SENS_MIN + ((cx - sx) * range + sw / 2) / sw;
            mouse_set_sensitivity(nv);
            gh_save(); gh_redraw(); return true;
        }
    }

    if (g_gh_tab == GH_T_NET) {
        /* Y positions match the gh_draw_network layout.  Each button only
         * RECORDS the intent; gephaz_tick() runs the blocking/modal work so
         * the desktop never freezes (see gh_process_pending). */
        int y0 = 78 + 6 * 16 + 24;        /* first button row */
        if (point_in(cx, cy, 24, y0, 200, 26)) {
            g_gh_pending = GH_PEND_TOGGLE_DHCP; gh_redraw(); return true;
        }
        if (point_in(cx, cy, 240, y0, 160, 26)) {
            g_gh_pending = GH_PEND_DHCP_RENEW; gh_redraw(); return true;
        }
        int y1 = y0 + 36;
        if (point_in(cx, cy, 24, y1, 200, 26)) {
            g_gh_pending = GH_PEND_SET_IP; gh_redraw(); return true;
        }
        if (point_in(cx, cy, 240, y1, 160, 26)) {
            g_gh_pending = GH_PEND_SET_GW; gh_redraw(); return true;
        }
        int y2 = y1 + 36;
        if (point_in(cx, cy, 24, y2, 200, 26)) {
            g_gh_pending = GH_PEND_SET_DNS; gh_redraw(); return true;
        }
        if (point_in(cx, cy, 240, y2, 200, 26)) {
            g_gh_pending = GH_PEND_SECDNS_INFO; return true;
        }
    }

    if (g_gh_tab == GH_T_SOUND) {
        /* Styled slider area: x=28..388, y=102..130 — click sets volume */
        if (point_in(cx, cy, 28, 102, 360, 28)) {
            int nv = (cx - 28) * 100 / 360;
            if (nv < 0)   nv = 0;
            if (nv > 100) nv = 100;
            audio_set_volume(nv);
            gh_save(); gh_redraw(); return true;
        }
        /* Buttons at y=158 */
        if (point_in(cx, cy, 28,  158, 140, 26)) {
            audio_set_muted(!audio_is_muted());
            gh_save(); gh_redraw(); return true;
        }
        if (point_in(cx, cy, 184, 158, 140, 26)) {
            speaker_beep(880, 120); return true;
        }
        if (point_in(cx, cy, 340, 158, 110, 26)) {
            audio_set_volume(audio_get_volume() + 10);
            gh_save(); gh_redraw(); return true;
        }
        if (point_in(cx, cy, 28,  194, 140, 26)) {
            audio_set_volume(audio_get_volume() - 10);
            gh_save(); gh_redraw(); return true;
        }
    }

    if (g_gh_tab == GH_T_SECURITY) {
        /* Buttons at btn_y=176 — mirrors gh_draw_security layout */
        int y_btn = 176;
        if (point_in(cx, cy, 28, y_btn, 200, 26)) {
            usermgr_open(); return true;
        }
        if (point_in(cx, cy, 244, y_btn, 200, 26)) {
            g_gh_pending = GH_PEND_CHANGE_PW; return true;
        }
    }

    if (g_gh_tab == GH_T_DISPLAY) {
        draw_target_t *dt = &g_gh_win->content;

        /* The keep/revert prompt is its own WM window now (gh_confirm_*); no
         * in-content hit-testing needed here. */

        int visible = gh_disp_visible_rows(dt);
        gh_disp_clamp_scroll(dt);
        int last = g_disp_scroll + visible;
        if (last > g_disp_mode_count) last = g_disp_mode_count;

        /* Scroll arrows (same geometry as gh_draw_display). */
        if (g_disp_scroll > 0 &&
            point_in(cx, cy, (int)dt->width - 40, DISP_LIST_Y, 16, 16)) {
            g_disp_scroll--; gh_redraw(); return true;
        }
        if (last < g_disp_mode_count &&
            point_in(cx, cy, (int)dt->width - 40,
                     DISP_LIST_Y + visible * DISP_ROW_H - 18, 16, 16)) {
            g_disp_scroll++; gh_redraw(); return true;
        }

        /* Mode list selection (visible window of the scrolled list). */
        int mode_y = DISP_LIST_Y;
        for (int i = g_disp_scroll; i < last; i++) {
            if (point_in(cx, cy, 28, mode_y, (int)dt->width - 72, 20)) {
                g_disp_selected = i;
                gh_redraw(); return true;
            }
            mode_y += DISP_ROW_H;
        }

        /* Apply button (anchored to the window bottom). */
        if (point_in(cx, cy, (int)dt->width - 32 - 120,
                     gh_disp_apply_y(dt), 120, 28)) {
            if (g_disp_selected >= 0 && g_disp_selected < g_disp_mode_count) {
                uint16_t nw = g_disp_modes[g_disp_selected].w;
                uint16_t nh = g_disp_modes[g_disp_selected].h;
                if (nw == (uint16_t)vga_width() && nh == (uint16_t)vga_height()) {
                    gh_redraw(); return true;
                }
                /* Defer the actual VBE mode switch to gephaz_tick(): running
                 * the real-mode trampoline nested inside the compositor's
                 * click dispatch hard-freezes bare metal. */
                g_gh_pending = GH_PEND_RES_APPLY;
                gh_redraw();
            }
            return true;
        }
    }

    return true;
}

static void gh_redraw(void) {
    if (!g_gh_win || !g_gh_win->in_use) { g_gh_win = NULL; return; }
    draw_target_t *t = &g_gh_win->content;
    gh_paint_body(t);
    gh_draw_tabs(t);

    switch (g_gh_tab) {
        case GH_T_SYSTEM:   gh_draw_system         (t); break;
        case GH_T_PERS:     gh_draw_personalization(t); break;
        case GH_T_SOUND:    gh_draw_audio          (t); break;
        case GH_T_NET:      gh_draw_network        (t); break;
        case GH_T_SECURITY: gh_draw_security       (t); break;
        case GH_T_DISPLAY:  gh_draw_display        (t); break;
        case GH_T_MOUSE:    gh_draw_mouse          (t); break;
    }
    wm_mark_dirty();
}

static void gh_resize_cb(window_t *w) { (void)w; gh_redraw(); }

/* Wheel scrolls the Display tab's mode list (dz > 0 = wheel up). */
static bool gh_scroll(window_t *w, int dz) {
    if (w != g_gh_win || g_gh_tab != GH_T_DISPLAY) return false;
    g_disp_scroll += (dz < 0) ? 1 : -1;
    gh_disp_clamp_scroll(&w->content);
    gh_redraw();
    return true;
}

/* ISSUE 1: WM is about to free this slot — drop our cached pointer so
 * a later wm_create_window for another app cannot trick us into
 * rendering Settings UI into its content framebuffer. */
static void gh_destroy_cb(window_t *w) {
    if (w == g_gh_win) g_gh_win = NULL;
}

/* TASK i18n v2: language-change listener.  Re-title the live window
 * and force a full repaint so tab labels + buttons + status text
 * appear in the new locale immediately. */
static void gh_on_language_change(lang_t new_lang) {
    (void)new_lang;
    if (g_gh_win && g_gh_win->in_use) {
        /* The window title is owned by the WM; just trigger a full
         * recompose to pick up the new STR_APP_SETTINGS value at the
         * next paint pass. */
        gh_redraw();
    }
}

bool gephaz_open(void) {
    if (g_gh_win && g_gh_win->in_use) {
        wm_set_focus(g_gh_win);
        gh_redraw();
        return true;
    }
    /* One-time listener registration on the first open.  Static guard
     * keeps the table from growing on repeated open/close cycles. */
    static bool listener_registered = false;
    if (!listener_registered) {
        lang_register_cb(gh_on_language_change);
        listener_registered = true;
    }
    g_gh_win = wm_create_window(80, 60, GH_W, GH_H, L(STR_APP_SETTINGS));
    if (!g_gh_win) return false;
    wm_set_content_click(g_gh_win, gh_click, NULL);
    wm_set_resizable(g_gh_win, true, GH_MIN_W, GH_MIN_H);
    wm_set_resize_cb(g_gh_win, gh_resize_cb);
    wm_set_scroll_handler(g_gh_win, gh_scroll);
    wm_set_destroy_cb(g_gh_win, gh_destroy_cb);
    wm_set_icon(g_gh_win, ICON_SETTINGS);
    gh_redraw();
    return true;
}
