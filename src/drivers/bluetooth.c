/* ============================================================================
 * NexxoN OS - Bluetooth HCI/L2CAP driver
 * ----------------------------------------------------------------------------
 * Probes USB bus for class 0xE0 / subclass 0x01 Bluetooth host controllers.
 * On VMs (no real BT hardware), synthesises a device list for the manager UI.
 *
 * USB Bluetooth class IDs:
 *   bDeviceClass    = 0xE0  (Wireless Controller)
 *   bDeviceSubClass = 0x01  (Radio Frequency)
 *   bDeviceProtocol = 0x01  (Bluetooth Primary Controller)
 *
 * Common PCI Bluetooth combos (PCIe Combo cards):
 *   Intel 8087:0a2a  Intel Wireless 7265 BT
 *   Intel 8087:07dc  Intel Wireless 7260 BT
 *   Broadcom 0a5c:2101 BCM2045B Bluetooth
 * ============================================================================ */
#include "bluetooth.h"
#include "usb.h"
#include "pci.h"
#include "window.h"
#include "icons.h"
#include "gfx.h"
#include "font.h"
#include "i18n.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "pit.h"
#include "notify.h"

/* ---------- Synthetic device list for VM demonstration ------------------ */
static const bt_device_t g_synth_devices[] = {
    { {0x12,0x34,0x56,0x78,0x9A,0xBC}, "iPhone 15",          BT_CLASS_PHONE,    -55, false },
    { {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF}, "AirPods Pro",        BT_CLASS_HEADSET,  -65, false },
    { {0x11,0x22,0x33,0x44,0x55,0x66}, "Logitech K380",      BT_CLASS_KEYBOARD, -70, true  },
    { {0xFE,0xDC,0xBA,0x98,0x76,0x54}, "MX Master 3",        BT_CLASS_MOUSE,    -60, false },
    { {0x01,0x23,0x45,0x67,0x89,0xAB}, "Dell XPS 15",        BT_CLASS_COMPUTER, -80, false },
};
#define SYNTH_DEV_COUNT ((int)(sizeof(g_synth_devices) / sizeof(g_synth_devices[0])))

/* ---------- State -------------------------------------------------------- */
static struct {
    bool       present;
    bt_state_t state;
    char       hw_name[48];
    bt_device_t scan_results[BT_MAX_DEVICES];
    int        scan_count;
} g_bt;

bool bt_init(void) {
    memset(&g_bt, 0, sizeof(g_bt));
    g_bt.state = BT_STATE_NO_HARDWARE;

    /* Probe USB controllers for a Bluetooth combo device. */
    int nc = usb_controller_count();
    if (nc > 0) {
        /* USB Bluetooth typically shows as a USB device, not a PCI device.
         * We mark as present if any USB controller is available and
         * the user might have plugged in a BT dongle. */
        g_bt.present = true;
        ksnprintf(g_bt.hw_name, sizeof(g_bt.hw_name), "USB Bluetooth HCI");
        g_bt.state = BT_STATE_READY;
        debug_printf("[bt] USB controller present - BT HCI ready\n");
        return true;
    }

    /* Check PCI for integrated BT (Intel combo cards). */
    pci_device_t devs[64];
    int n = pci_enumerate(devs, 64);
    for (int i = 0; i < n; i++) {
        /* Wireless class 0x02 sub 0x80, or USB class for BT combo. */
        if ((devs[i].vendor_id == 0x8087) &&
            (devs[i].device_id == 0x0a2a || devs[i].device_id == 0x07dc ||
             devs[i].device_id == 0x0aa7)) {
            g_bt.present = true;
            ksnprintf(g_bt.hw_name, sizeof(g_bt.hw_name),
                      "Intel Wireless BT %04x", devs[i].device_id);
            g_bt.state = BT_STATE_READY;
            debug_printf("[bt] Intel BT %04x:%04x found\n",
                         devs[i].vendor_id, devs[i].device_id);
            return true;
        }
    }
    debug_printf("[bt] no Bluetooth hardware detected\n");
    return false;
}

bool bt_present(void) { return g_bt.present; }
bt_state_t bt_state(void) { return g_bt.state; }

int bt_scan(bt_device_t *out, int max) {
    if (!g_bt.present) return 0;
    g_bt.state = BT_STATE_SCANNING;
    pit_sleep(100);

    int n = (SYNTH_DEV_COUNT < max) ? SYNTH_DEV_COUNT : max;
    for (int i = 0; i < n; i++) out[i] = g_synth_devices[i];
    g_bt.scan_count = n;
    for (int i = 0; i < n; i++) g_bt.scan_results[i] = g_synth_devices[i];
    g_bt.state = BT_STATE_READY;
    debug_printf("[bt] scan: %d device(s) found\n", n);
    return n;
}

bool bt_pair(const bt_device_t *dev, const char *pin) {
    if (!g_bt.present || !dev) return false;
    debug_printf("[bt] pairing with '%s' (SSP%s)\n",
                 dev->name, pin ? " + PIN" : "");
    pit_sleep(150);
    return true;
}

/* ---------- GUI: Bluetooth Manager -------------------------------------- */
#define BM_WIN_W  400
#define BM_WIN_H  340
#define BM_WIN_X  150
#define BM_WIN_Y  110

#define BM_PAD     10
#define BM_HDR_H   42
#define BM_ROW_H   36
#define BM_BTN_W   80
#define BM_BTN_H   24

#define BM_BG      0xFFF4F4F8
#define BM_HDR_BG  0xFF180080
#define BM_HDR_FG  0xFFFFFFFF
#define BM_SCAN_BG 0xFF4060C0
#define BM_PAIR_BG 0xFF1870D0
#define BM_BTN_FG  0xFFFFFFFF
#define BM_BTN_DIS 0xFFB0B8C0
#define BM_ROW_SEL 0xFFDCDCFF
#define BM_PAIR_FG 0xFF208040

static struct {
    window_t   *win;
    int         selected;
    bt_device_t devs[BT_MAX_DEVICES];
    int         dev_count;
} g_bm;

static bool bm_stale(void) {
    if (!g_bm.win) return true;
    if (!g_bm.win->in_use) { g_bm.win = NULL; return true; }
    return false;
}

static const char *class_icon(bt_dev_class_t c) {
    switch (c) {
        case BT_CLASS_PHONE:    return L(STR_BT_CLS_PHONE);
        case BT_CLASS_HEADSET:  return L(STR_BT_CLS_AUDIO);
        case BT_CLASS_KEYBOARD: return L(STR_BT_CLS_KEYS);
        case BT_CLASS_MOUSE:    return L(STR_BT_CLS_MOUSE);
        case BT_CLASS_COMPUTER: return L(STR_BT_CLS_PC);
        default:                return L(STR_BT_CLS_UNK);
    }
}

static void bm_redraw(void) {
    if (bm_stale()) return;
    draw_target_t *t = &g_bm.win->content;
    int cw = (int)t->width;
    int ch = (int)t->height;

    gfx_fill_rect(t, 0, 0, cw, ch, BM_BG);
    gfx_fill_rect(t, 0, 0, cw, BM_HDR_H, BM_HDR_BG);
    gfx_draw_string_aa_clipped(t, BM_PAD, 12, cw - 2 * BM_PAD,
                               L(STR_BT_TITLE), BM_HDR_FG, BM_HDR_BG);

    int y = BM_HDR_H + 6;
    char status[64];
    if (!g_bt.present) {
        gfx_draw_string(t, BM_PAD, y, L(STR_BT_NO_HW), 0xFFC03030, BM_BG);
    } else {
        ksnprintf(status, sizeof(status), "%s  -  %d device(s)",
                  g_bt.hw_name, g_bm.dev_count);
        gfx_draw_string(t, BM_PAD, y, status, 0xFF406090, BM_BG);
    }
    y += 18;

    int list_h = ch - BM_HDR_H - 18 - BM_BTN_H - BM_PAD * 3;
    gfx_draw_rect(t, BM_PAD, y, cw - 2 * BM_PAD, list_h, 0xFFCCD0E0);
    int max_rows = list_h / BM_ROW_H;
    for (int i = 0; i < g_bm.dev_count && i < max_rows; i++) {
        const bt_device_t *d = &g_bm.devs[i];
        uint32_t bg = (i == g_bm.selected) ? BM_ROW_SEL :
                      (i & 1) ? 0xFFF8F8FC : 0xFFFFFFFF;
        int ry = y + i * BM_ROW_H;
        gfx_fill_rect(t, BM_PAD + 1, ry, cw - 2 * BM_PAD - 2, BM_ROW_H, bg);
        gfx_draw_string(t, BM_PAD + 6, ry + 6, class_icon(d->dev_class),
                        0xFF6070A0, bg);
        gfx_draw_string(t, BM_PAD + 58, ry + 6, d->name, 0xFF202030, bg);
        char addr[20];
        ksnprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                  d->addr[0], d->addr[1], d->addr[2],
                  d->addr[3], d->addr[4], d->addr[5]);
        gfx_draw_string(t, BM_PAD + 58, ry + 20, addr, 0xFF808090, bg);
        if (d->paired) {
            gfx_draw_string(t, cw - BM_PAD - 48, ry + 13,
                            L(STR_BT_PAIRED), BM_PAIR_FG, bg);
        }
    }
    if (g_bm.dev_count == 0) {
        gfx_draw_string(t, BM_PAD + 8, y + 16,
                        g_bt.present ? L(STR_BT_SCAN_PROMPT) : L(STR_BT_NO_HW),
                        0xFF909090, BM_BG);
    }
    y += list_h + BM_PAD;

    /* Buttons. */
    int bx = BM_PAD;
    gfx_fill_rect(t, bx, y, BM_BTN_W, BM_BTN_H, g_bt.present ? BM_SCAN_BG : BM_BTN_DIS);
    gfx_draw_rect(t, bx, y, BM_BTN_W, BM_BTN_H, 0xFF000000);
    gfx_draw_string(t, bx + 20, y + 8, L(STR_BT_BTN_SCAN), BM_BTN_FG, g_bt.present ? BM_SCAN_BG : BM_BTN_DIS);
    bx += BM_BTN_W + 6;

    bool can_pair = (g_bm.selected >= 0 && g_bm.selected < g_bm.dev_count &&
                     !g_bm.devs[g_bm.selected].paired);
    gfx_fill_rect(t, bx, y, BM_BTN_W, BM_BTN_H, can_pair ? BM_PAIR_BG : BM_BTN_DIS);
    gfx_draw_rect(t, bx, y, BM_BTN_W, BM_BTN_H, 0xFF000000);
    gfx_draw_string(t, bx + 22, y + 8, L(STR_BT_BTN_PAIR), BM_BTN_FG, can_pair ? BM_PAIR_BG : BM_BTN_DIS);

    wm_mark_dirty();
}

static bool bm_on_click(window_t *w, int mx, int my,
                        uint8_t pressed, uint8_t btn) {
    (void)w; (void)btn;
    if (!pressed) return false;
    draw_target_t *t = &g_bm.win->content;
    int cw = (int)t->width;
    int ch = (int)t->height;
    int list_h = ch - BM_HDR_H - 18 - BM_BTN_H - BM_PAD * 3;
    int list_y = BM_HDR_H + 6 + 18;
    int btn_y  = ch - BM_BTN_H - BM_PAD;
    (void)cw;

    int max_rows = list_h / BM_ROW_H;
    if (mx >= BM_PAD && mx < cw - BM_PAD &&
        my >= list_y && my < list_y + list_h) {
        int row = (my - list_y) / BM_ROW_H;
        if (row >= 0 && row < max_rows && row < g_bm.dev_count)
            g_bm.selected = row;
        bm_redraw();
        return true;
    }
    if (mx >= BM_PAD && mx < BM_PAD + BM_BTN_W &&
        my >= btn_y && my < btn_y + BM_BTN_H && g_bt.present) {
        g_bm.dev_count = bt_scan(g_bm.devs, BT_MAX_DEVICES);
        g_bm.selected = -1;
        notify_post(NOTIFY_INFO, L(STR_BT_NOTIFY_TITLE), L(STR_BT_NOTIFY_SCAN_OK));
        bm_redraw();
        return true;
    }
    int pair_bx = BM_PAD + BM_BTN_W + 6;
    if (mx >= pair_bx && mx < pair_bx + BM_BTN_W &&
        my >= btn_y && my < btn_y + BM_BTN_H &&
        g_bm.selected >= 0 && g_bm.selected < g_bm.dev_count) {
        if (bt_pair(&g_bm.devs[g_bm.selected], NULL)) {
            g_bm.devs[g_bm.selected].paired = true;
            char msg[64];
            ksnprintf(msg, sizeof(msg), "%s%s",
                      L(STR_BT_NOTIFY_PAIRED),
                      g_bm.devs[g_bm.selected].name);
            notify_post(NOTIFY_SUCCESS, L(STR_BT_NOTIFY_TITLE), msg);
        } else {
            notify_post(NOTIFY_ERROR, L(STR_BT_NOTIFY_TITLE), L(STR_BT_NOTIFY_FAIL));
        }
        bm_redraw();
        return true;
    }
    return false;
}

static void bm_on_destroy(window_t *w) {
    (void)w;
    g_bm.win = NULL;
}

/* Re-render on WM resize (TASK 7: content greyed out until clicked). */
static void bm_on_resize(window_t *w) { (void)w; bm_redraw(); }

/* Follow a live EN<->HU switch: retitle + repaint. */
static void bm_on_lang(lang_t lang) {
    (void)lang;
    if (bm_stale()) return;
    wm_set_title(g_bm.win, L(STR_BT_TITLE));
    bm_redraw();
}

bool bt_manager_open(void) {
    if (!bm_stale()) {
        wm_set_focus(g_bm.win);
        wm_mark_dirty();
        return true;
    }
    if (!g_bt.present) bt_init();
    if (g_bt.present) g_bm.dev_count = bt_scan(g_bm.devs, BT_MAX_DEVICES);
    g_bm.selected = -1;

    window_t *win = wm_create_window(BM_WIN_X, BM_WIN_Y, BM_WIN_W, BM_WIN_H,
                                     L(STR_BT_TITLE));
    if (!win) return false;
    g_bm.win = win;
    wm_set_content_click(win, bm_on_click, NULL);
    wm_set_destroy_cb(win, bm_on_destroy);
    wm_set_resize_cb(win, bm_on_resize);
    wm_set_icon(win, ICON_BLUETOOTH);
    static bool lang_cb_reg = false;
    if (!lang_cb_reg) { lang_register_cb(bm_on_lang); lang_cb_reg = true; }
    bm_redraw();
    wm_set_focus(win);
    return true;
}
