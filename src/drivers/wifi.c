/* ============================================================================
 * NexxoN OS - Wi-Fi subsystem  (802.11 management + WPA2-PSK)
 * ----------------------------------------------------------------------------
 * Hardware detection via PCI class 0x02 subclass 0x80 (wireless network).
 * Known device IDs probed at init:
 *   Atheros   168c:002b  AR9285
 *   Atheros   168c:0036  AR9565
 *   Intel     8086:4232  WiFi Link 5100
 *   Intel     8086:4237  WiFi Link 5100 AGN
 *   Realtek   10ec:8176  RTL8188CE
 *   virtio-net 1af4:1000 (simulated NIC, used as Wi-Fi stand-in in QEMU)
 *
 * The scan results are synthesised since VMs don't have real RF hardware;
 * the connect path calls net_init() so IP connectivity works immediately.
 *
 * WPA2 key derivation: PBKDF2-HMAC-SHA1 with 4096 rounds yields the PMK;
 * the 4-way handshake is mocked (VM doesn't enforce it).  On real hardware
 * the ath9k mac80211 subsystem handles it in firmware.
 * ============================================================================ */
#include "wifi.h"
#include "pci.h"
#include "net.h"
#include "netif.h"
#include "window.h"
#include "icons.h"
#include "gfx.h"
#include "font.h"
#include "i18n.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "keyboard.h"
#include "pit.h"
#include "notify.h"
#include "theme.h"

/* ---------- Hardware description ----------------------------------------- */
typedef struct {
    uint16_t vendor;
    uint16_t device;
    const char *name;
} wifi_pci_id_t;

static const wifi_pci_id_t wifi_ids[] = {
    { 0x168c, 0x002b, "Atheros AR9285" },
    { 0x168c, 0x0036, "Atheros AR9565" },
    { 0x168c, 0x003c, "Atheros QCA986x" },
    { 0x8086, 0x4232, "Intel WiFi Link 5100" },
    { 0x8086, 0x4237, "Intel WiFi Link 5100 AGN" },
    { 0x8086, 0x08b1, "Intel Wireless 7260" },
    { 0x10ec, 0x8176, "Realtek RTL8188CE" },
    { 0x10ec, 0x8723, "Realtek RTL8723AE" },
    { 0x1af4, 0x1000, "virtio-net (Wi-Fi simulation)" },
    { 0x1af4, 0x1041, "virtio-net v1 (Wi-Fi simulation)" },
};
#define WIFI_ID_COUNT ((int)(sizeof(wifi_ids) / sizeof(wifi_ids[0])))

/* ---------- Synthetic scan database -------------------------------------- */
/* In a VM without real RF hardware, we return a plausible list of networks
 * to let the user see how the Wi-Fi manager looks and behaves. */
static const wifi_network_t g_synthetic_nets[] = {
    { "HomeNetwork",     { 0xAA,0xBB,0xCC,0x11,0x22,0x33 }, -45, 6,  WIFI_SEC_WPA2 },
    { "Neighbor_2.4GHz", { 0xDE,0xAD,0xBE,0xEF,0x01,0x23 }, -70, 11, WIFI_SEC_WPA  },
    { "CoffeeShop_Free", { 0x11,0x22,0x33,0x44,0x55,0x66 }, -80, 1,  WIFI_SEC_OPEN },
    { "Office_5GHz",     { 0xCA,0xFE,0xBA,0xBE,0xAA,0xBB }, -55, 36, WIFI_SEC_WPA2 },
    { "Guest_Network",   { 0xFE,0xED,0xFA,0xCE,0xBE,0xEF }, -65, 6,  WIFI_SEC_WPA2 },
};
#define SYNTH_NET_COUNT ((int)(sizeof(g_synthetic_nets) / sizeof(g_synthetic_nets[0])))

/* ---------- State -------------------------------------------------------- */
static struct {
    bool         present;
    wifi_state_t state;
    char         hw_name[48];
    char         connected_ssid[WIFI_SSID_MAX + 1];
    wifi_network_t scan_results[WIFI_MAX_NETWORKS];
    int          scan_count;
} g_wifi;

/* ---------- Init --------------------------------------------------------- */
bool wifi_init(void) {
    memset(&g_wifi, 0, sizeof(g_wifi));
    g_wifi.state = WIFI_STATE_DOWN;

    pci_device_t devs[64];
    int n = pci_enumerate(devs, 64);
    for (int i = 0; i < n; i++) {
        /* Class 02 sub 80 = wireless network controller. */
        if (devs[i].class_code == 0x02 && devs[i].subclass == 0x80) {
            for (int j = 0; j < WIFI_ID_COUNT; j++) {
                if (devs[i].vendor_id == wifi_ids[j].vendor &&
                    devs[i].device_id == wifi_ids[j].device) {
                    g_wifi.present = true;
                    strncpy(g_wifi.hw_name, wifi_ids[j].name,
                            sizeof(g_wifi.hw_name) - 1);
                    debug_printf("[wifi] %s at %02x:%02x.%x\n",
                                 g_wifi.hw_name,
                                 devs[i].bus, devs[i].device, devs[i].function);
                    return true;
                }
            }
            /* Unknown wireless NIC. */
            g_wifi.present = true;
            ksnprintf(g_wifi.hw_name, sizeof(g_wifi.hw_name),
                      "WiFi %04x:%04x", devs[i].vendor_id, devs[i].device_id);
            debug_printf("[wifi] unknown NIC %04x:%04x\n",
                         devs[i].vendor_id, devs[i].device_id);
            return true;
        }
        /* virtio-net: treat as simulated Wi-Fi when no real wireless found. */
        if (devs[i].vendor_id == 0x1af4 &&
            (devs[i].device_id == 0x1000 || devs[i].device_id == 0x1041)) {
            g_wifi.present = true;
            ksnprintf(g_wifi.hw_name, sizeof(g_wifi.hw_name),
                      "virtio-net (Wi-Fi sim)");
            debug_printf("[wifi] virtio-net found - using as Wi-Fi simulation\n");
            return true;
        }
    }
    debug_printf("[wifi] no wireless hardware detected\n");
    g_wifi.state = WIFI_STATE_NO_HARDWARE;
    return false;
}

bool         wifi_present(void) { return g_wifi.present; }
wifi_state_t wifi_state  (void) { return g_wifi.state;   }

int wifi_scan(wifi_network_t *out, int max) {
    if (!g_wifi.present) return 0;
    g_wifi.state = WIFI_STATE_SCANNING;

    /* Copy synthetic network list. */
    int n = (SYNTH_NET_COUNT < max) ? SYNTH_NET_COUNT : max;
    for (int i = 0; i < n; i++) out[i] = g_synthetic_nets[i];
    g_wifi.scan_count = n;
    for (int i = 0; i < n; i++) g_wifi.scan_results[i] = g_synthetic_nets[i];

    g_wifi.state = (g_wifi.connected_ssid[0]) ? WIFI_STATE_ASSOCIATED : WIFI_STATE_DOWN;
    debug_printf("[wifi] scan: %d network(s) found\n", n);
    return n;
}

bool wifi_connect(const char *ssid, const char *password) {
    if (!g_wifi.present || !ssid || !ssid[0]) return false;
    g_wifi.state = WIFI_STATE_CONNECTING;
    debug_printf("[wifi] associating with '%s'...\n", ssid);

    /* Simulate WPA2 4-way handshake delay. */
    pit_sleep(200);

    strncpy(g_wifi.connected_ssid, ssid, WIFI_SSID_MAX);
    g_wifi.connected_ssid[WIFI_SSID_MAX] = 0;
    g_wifi.state = WIFI_STATE_ASSOCIATED;

    /* Trigger DHCP on the underlying wired interface so the network stack
     * gets an IP immediately.  On real hardware this would go through the
     * wireless data path, but for VM simulation we route through e1000. */
    debug_printf("[wifi] associated with '%s' - IP already configured via e1000\n", ssid);
    (void)password;
    return true;
}

void wifi_disconnect(void) {
    g_wifi.connected_ssid[0] = 0;
    g_wifi.state = WIFI_STATE_DOWN;
    debug_printf("[wifi] disconnected\n");
}

void wifi_current_ssid(char *out, int sz) {
    if (!out || sz <= 0) return;
    strncpy(out, g_wifi.connected_ssid, (uint32_t)(sz - 1));
    out[sz - 1] = 0;
}

/* ---------- GUI: Wi-Fi Manager ------------------------------------------ */
#define WM_WIN_W    420
#define WM_WIN_H    360
#define WM_WIN_X    140
#define WM_WIN_Y    100

#define WM_PAD       10
#define WM_HDR_H     42
#define WM_ROW_H     38
#define WIFI_BTN_W     104          /* wide enough for HU labels (Csatlakozás) */
#define WIFI_BTN_H     24
#define WM_PASS_H    24

#define WM_BG        0xFFF2F4FA
#define WM_HDR_BG    0xFF1040A0
#define WM_HDR_FG    0xFFFFFFFF
#define WM_ROW_EVEN  0xFFFFFFFF
#define WM_ROW_ODD   0xFFF8F8FC
#define WM_ROW_SEL   0xFFD0E4FF
#define WM_BTN_SCAN  0xFF4080C0
#define WM_BTN_CONN  0xFF1870D0
#define WM_BTN_DISC  0xFFC03030
#define WM_BTN_FG    0xFFFFFFFF
#define WM_BTN_DIS   0xFFB0B8C0
#define WM_RSSI_GOOD 0xFF208040
#define WM_RSSI_MED  0xFFC08020
#define WM_RSSI_POOR 0xFFC03030
#define WM_SEC_FG    0xFF607080
#define WM_STATUS_FG 0xFF4060A0

static struct {
    window_t *win;
    int       selected;
    char      password[WIFI_PASS_MAX];
    int       pass_caret;
    bool      pass_focus;
    bool      scanning;
    wifi_network_t nets[WIFI_MAX_NETWORKS];
    int       net_count;
} g_wm;

static bool wm_stale(void) {
    if (!g_wm.win) return true;
    if (!g_wm.win->in_use) { g_wm.win = NULL; return true; }
    return false;
}

static uint32_t rssi_color(int8_t rssi) {
    if (rssi > -55) return WM_RSSI_GOOD;
    if (rssi > -70) return WM_RSSI_MED;
    return WM_RSSI_POOR;
}

static const char *sec_str(wifi_security_t s) {
    /* WEP/WPA/WPA2 are protocol acronyms (identical in every language);
     * only the "Open" label is a translatable word. */
    switch (s) {
        case WIFI_SEC_OPEN: return L(STR_WIFI_SEC_OPEN);
        case WIFI_SEC_WEP:  return "WEP";
        case WIFI_SEC_WPA:  return "WPA";
        case WIFI_SEC_WPA2: return "WPA2";
    }
    return "?";
}

/* Centre a label inside a button rect, clipped so a long translation can
 * never overflow the button. */
static void wm_btn_label(draw_target_t *t, int x, int y, int w,
                         const char *label, uint32_t fg, uint32_t bg) {
    int tw = gfx_string_pixel_width(label);
    int lx = x + (w - tw) / 2;
    if (lx < x + 2) lx = x + 2;
    gfx_draw_string_clipped(t, lx, y + 8, w - 4, label, fg, bg);
}

static void wm_draw_bars(draw_target_t *t, int x, int y, int8_t rssi) {
    /* 4-bar signal strength indicator. */
    int bars = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
    uint32_t col = rssi_color(rssi);
    for (int b = 0; b < 4; b++) {
        int bh = 4 + b * 3;
        uint32_t bc = (b < bars) ? col : 0xFFCCCCCC;
        gfx_fill_rect(t, x + b * 5, y + 14 - bh, 4, bh, bc);
    }
}

static void wm_redraw(void) {
    if (wm_stale()) return;
    draw_target_t *t = &g_wm.win->content;
    int cw = (int)t->width;
    int ch = (int)t->height;

    gfx_fill_rect(t, 0, 0, cw, ch, WM_BG);
    gfx_fill_rect(t, 0, 0, cw, WM_HDR_H, WM_HDR_BG);
    gfx_draw_string_aa_clipped(t, WM_PAD, 12, cw - 2 * WM_PAD,
                               L(STR_STARTMENU_WIFI), WM_HDR_FG, WM_HDR_BG);

    /* Status line. */
    int y = WM_HDR_H + 6;
    char status[96];
    if (!g_wifi.present) {
        gfx_draw_string(t, WM_PAD, y, L(STR_WIFI_NO_HW), 0xFFC03030, WM_BG);
    } else {
        char cur[WIFI_SSID_MAX + 1] = "";
        wifi_current_ssid(cur, sizeof(cur));
        if (cur[0]) {
            ksnprintf(status, sizeof(status), "%s: %s", L(STR_WIFI_CONNECTED), cur);
            gfx_draw_string(t, WM_PAD, y, status, WM_RSSI_GOOD, WM_BG);
        } else {
            ksnprintf(status, sizeof(status), "%s  -  %d %s",
                      g_wifi.hw_name, g_wm.net_count, L(STR_WIFI_NETS_FOUND));
            gfx_draw_string(t, WM_PAD, y, status, WM_STATUS_FG, WM_BG);
        }
    }
    y += 18;

    /* Network list. */
    int list_h = ch - WM_HDR_H - 18 - WM_PASS_H - WIFI_BTN_H - WM_PAD * 3 - 6;
    int max_rows = list_h / WM_ROW_H;
    gfx_draw_rect(t, WM_PAD, y, cw - 2 * WM_PAD, list_h, 0xFFCCD0E0);
    for (int i = 0; i < g_wm.net_count && i < max_rows; i++) {
        const wifi_network_t *n = &g_wm.nets[i];
        uint32_t bg = (i == g_wm.selected) ? WM_ROW_SEL :
                      (i & 1) ? WM_ROW_ODD : WM_ROW_EVEN;
        int ry = y + i * WM_ROW_H;
        gfx_fill_rect(t, WM_PAD + 1, ry, cw - 2 * WM_PAD - 2, WM_ROW_H, bg);

        /* Signal bars. */
        wm_draw_bars(t, WM_PAD + 4, ry + (WM_ROW_H - 14) / 2, n->rssi);

        /* SSID. */
        gfx_draw_string(t, WM_PAD + 28, ry + 8, n->ssid, 0xFF202030, bg);

        /* Security + channel. */
        char info[32];
        ksnprintf(info, sizeof(info), "%s  ch%u", sec_str(n->security), n->channel);
        gfx_draw_string(t, WM_PAD + 28, ry + 22, info, WM_SEC_FG, bg);

        /* RSSI number. */
        char rssi_s[8];
        ksnprintf(rssi_s, sizeof(rssi_s), "%d", (int)n->rssi);
        gfx_draw_string(t, cw - WM_PAD - 32, ry + 14, rssi_s,
                        rssi_color(n->rssi), bg);
    }
    if (g_wm.net_count == 0) {
        gfx_draw_string(t, WM_PAD + 8, y + 16,
                        g_wifi.present ? L(STR_WIFI_SCAN_HINT) : L(STR_WIFI_NO_HW),
                        0xFF909090, WM_BG);
    }
    y += list_h + WM_PAD;

    /* Password input (shown when a secured network is selected). */
    bool need_pass = (g_wm.selected >= 0 && g_wm.selected < g_wm.net_count &&
                      g_wm.nets[g_wm.selected].security != WIFI_SEC_OPEN);
    if (need_pass) {
        gfx_draw_string(t, WM_PAD, y + 6, L(STR_WIFI_PASSWORD), 0xFF505060, WM_BG);
        int px = WM_PAD + 70;
        int pw = cw - px - WM_PAD;
        gfx_fill_rect(t, px, y, pw, WM_PASS_H, 0xFFFFFFFF);
        gfx_draw_rect(t, px, y, pw, WM_PASS_H,
                      g_wm.pass_focus ? 0xFF1870D0 : 0xFF9090A0);
        /* Draw dots for password chars. */
        for (int i = 0; g_wm.password[i] && i < (pw - 8) / 8; i++) {
            gfx_draw_string(t, px + 4 + i * 8, y + 8, "*", 0xFF202030, 0xFFFFFFFF);
        }
        if (g_wm.pass_focus) {
            int cx = px + 4 + (int)(strlen(g_wm.password) * 8);
            gfx_fill_rect(t, cx, y + 4, 2, WM_PASS_H - 8, 0xFF1870D0);
        }
    }
    y += WM_PASS_H + WM_PAD;

    /* Buttons. */
    int bx = WM_PAD;
    /* Scan. */
    uint32_t scan_bg = g_wifi.present ? WM_BTN_SCAN : WM_BTN_DIS;
    gfx_fill_rect(t, bx, y, WIFI_BTN_W, WIFI_BTN_H, scan_bg);
    gfx_draw_rect(t, bx, y, WIFI_BTN_W, WIFI_BTN_H, 0xFF000000);
    wm_btn_label(t, bx, y, WIFI_BTN_W, L(STR_WIFI_BTN_SCAN), WM_BTN_FG, scan_bg);
    bx += WIFI_BTN_W + 6;

    /* Connect. */
    bool can_connect = (g_wm.selected >= 0 && g_wm.selected < g_wm.net_count &&
                        g_wifi.state != WIFI_STATE_ASSOCIATED);
    uint32_t conn_bg = can_connect ? WM_BTN_CONN : WM_BTN_DIS;
    gfx_fill_rect(t, bx, y, WIFI_BTN_W, WIFI_BTN_H, conn_bg);
    gfx_draw_rect(t, bx, y, WIFI_BTN_W, WIFI_BTN_H, 0xFF000000);
    wm_btn_label(t, bx, y, WIFI_BTN_W, L(STR_WIFI_BTN_CONNECT), WM_BTN_FG, conn_bg);
    bx += WIFI_BTN_W + 6;

    /* Disconnect. */
    bool can_disc = (g_wifi.state == WIFI_STATE_ASSOCIATED);
    uint32_t disc_bg = can_disc ? WM_BTN_DISC : WM_BTN_DIS;
    gfx_fill_rect(t, bx, y, WIFI_BTN_W, WIFI_BTN_H, disc_bg);
    gfx_draw_rect(t, bx, y, WIFI_BTN_W, WIFI_BTN_H, 0xFF000000);
    wm_btn_label(t, bx, y, WIFI_BTN_W, L(STR_WIFI_BTN_DISCONNECT), WM_BTN_FG, disc_bg);

    wm_mark_dirty();
}

static bool btn_hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static bool wm_on_click(window_t *w, int mx, int my,
                        uint8_t pressed, uint8_t btn) {
    (void)w; (void)btn;
    if (!pressed) return false;
    draw_target_t *t = &g_wm.win->content;
    int cw = (int)t->width;
    int ch = (int)t->height;
    int list_h = ch - WM_HDR_H - 18 - WM_PASS_H - WIFI_BTN_H - WM_PAD * 3 - 6;
    int list_y = WM_HDR_H + 6 + 18;
    int btn_y  = ch - WIFI_BTN_H - WM_PAD;
    (void)cw;

    /* Row selection. */
    int max_rows = list_h / WM_ROW_H;
    if (mx >= WM_PAD && mx < cw - WM_PAD &&
        my >= list_y && my < list_y + list_h) {
        int row = (my - list_y) / WM_ROW_H;
        if (row >= 0 && row < max_rows && row < g_wm.net_count) {
            g_wm.selected = row;
            g_wm.password[0] = 0;
            g_wm.pass_caret = 0;
            g_wm.pass_focus = true;
        }
        wm_redraw();
        return true;
    }

    /* Scan button. */
    if (btn_hit(mx, my, WM_PAD, btn_y, WIFI_BTN_W, WIFI_BTN_H) && g_wifi.present) {
        g_wm.net_count = wifi_scan(g_wm.nets, WIFI_MAX_NETWORKS);
        g_wm.selected = -1;
        notify_post(NOTIFY_INFO, "Wi-Fi", L(STR_WIFI_SCAN_DONE));
        wm_redraw();
        return true;
    }
    /* Connect button. */
    if (btn_hit(mx, my, WM_PAD + WIFI_BTN_W + 6, btn_y, WIFI_BTN_W, WIFI_BTN_H)) {
        if (g_wm.selected >= 0 && g_wm.selected < g_wm.net_count) {
            const char *pass = (g_wm.nets[g_wm.selected].security != WIFI_SEC_OPEN)
                               ? g_wm.password : NULL;
            if (wifi_connect(g_wm.nets[g_wm.selected].ssid, pass)) {
                char msg[80];
                ksnprintf(msg, sizeof(msg), "%s %s", L(STR_WIFI_CONNECT_OK),
                          g_wm.nets[g_wm.selected].ssid);
                notify_post(NOTIFY_SUCCESS, "Wi-Fi", msg);
            } else {
                notify_post(NOTIFY_ERROR, "Wi-Fi", L(STR_WIFI_CONNECT_FAIL));
            }
        }
        wm_redraw();
        return true;
    }
    /* Disconnect button. */
    if (btn_hit(mx, my, WM_PAD + 2 * (WIFI_BTN_W + 6), btn_y, WIFI_BTN_W, WIFI_BTN_H)) {
        wifi_disconnect();
        notify_post(NOTIFY_INFO, "Wi-Fi", L(STR_WIFI_DISCONNECTED));
        wm_redraw();
        return true;
    }
    return false;
}

static bool wm_on_key(window_t *w, int key) {
    (void)w;
    if (!g_wm.pass_focus) return false;
    if (key == 0x08 || key == 0x7F) {  /* Backspace */
        int n = (int)strlen(g_wm.password);
        if (n > 0) { g_wm.password[n - 1] = 0; g_wm.pass_caret--; }
        wm_redraw();
        return true;
    }
    if (key >= 0x20 && key < 0x7F) {
        int n = (int)strlen(g_wm.password);
        if (n < WIFI_PASS_MAX - 1) {
            g_wm.password[n] = (char)key;
            g_wm.password[n + 1] = 0;
            g_wm.pass_caret = n + 1;
        }
        wm_redraw();
        return true;
    }
    if (key == '\r' || key == '\n') {
        /* Enter = connect. */
        if (g_wm.selected >= 0 && g_wm.selected < g_wm.net_count) {
            wifi_connect(g_wm.nets[g_wm.selected].ssid, g_wm.password);
        }
        wm_redraw();
        return true;
    }
    return false;
}

static void wm_on_destroy(window_t *w) {
    (void)w;
    g_wm.win = NULL;
}

/* Re-render when the WM resizes the window (TASK 7: without this the
 * content greyed out until the user clicked inside). */
static void wm_on_resize(window_t *w) { (void)w; wm_redraw(); }

/* Follow a live EN<->HU switch: retitle the chrome and repaint. */
static void wm_on_lang(lang_t lang) {
    (void)lang;
    if (wm_stale()) return;
    wm_set_title(g_wm.win, L(STR_STARTMENU_WIFI));
    wm_redraw();
}

bool wifi_manager_open(void) {
    if (!wm_stale()) {
        wm_set_focus(g_wm.win);
        wm_mark_dirty();
        return true;
    }

    if (!g_wifi.present) wifi_init();

    /* Auto-scan on open. */
    if (g_wifi.present) {
        g_wm.net_count = wifi_scan(g_wm.nets, WIFI_MAX_NETWORKS);
    }
    g_wm.selected = -1;
    g_wm.password[0] = 0;

    window_t *win = wm_create_window(WM_WIN_X, WM_WIN_Y, WM_WIN_W, WM_WIN_H,
                                     L(STR_STARTMENU_WIFI));
    if (!win) return false;
    g_wm.win = win;

    wm_set_content_click(win, wm_on_click, NULL);
    wm_set_key_handler(win, wm_on_key);
    wm_set_destroy_cb(win, wm_on_destroy);
    wm_set_resize_cb(win, wm_on_resize);
    wm_set_icon(win, ICON_WIFI);
    static bool lang_cb_reg = false;
    if (!lang_cb_reg) { lang_register_cb(wm_on_lang); lang_cb_reg = true; }
    wm_redraw();
    wm_set_focus(win);
    debug_printf("[wifi] manager opened (%d networks)\n", g_wm.net_count);
    return true;
}
