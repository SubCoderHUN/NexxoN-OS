/* ============================================================================
 * NexxoN OS - USB HID boot-protocol driver (keyboard + mouse)
 * ----------------------------------------------------------------------------
 * See usbhid.h for the rationale.  The driver:
 *   1. On attach: SET_PROTOCOL(boot) + SET_IDLE(0) on the HID interface.
 *   2. On poll:   reads the interrupt-IN report via the quiet/bounded-spin
 *      usb_intr_in() (so an idle, NAKing endpoint doesn't stall the system).
 *   3. Translates:
 *        - keyboard boot reports (8 bytes: mods, resv, 6 keycodes) into PS/2
 *          set-1 scancodes fed to keyboard_handle_byte(), reusing all of the
 *          existing modifier/layout/keymap logic;
 *        - mouse boot reports (buttons, dx, dy) into mouse_inject().
 *
 * Edge-triggered: it diffs each report against the previous one so a held
 * key produces a single make and one break (no host-side auto-repeat yet).
 * ============================================================================ */
#include "usbhid.h"
#include "usb.h"
#include "keyboard.h"
#include "mouse.h"
#include "string.h"
#include "debug.h"
#include "pit.h"

/* ---------- HID class requests (HID 1.11 §7.2) -------------------------- */
#define HID_REQ_SET_IDLE        0x0A
#define HID_REQ_SET_PROTOCOL    0x0B
#define HID_RT_CLASS_IFACE_OUT  0x21    /* host->dev | class | interface */

#define HID_PROTO_KEYBOARD      1
#define HID_PROTO_MOUSE         2

/* Poll cadence.  The PIT runs at 100 Hz so this rounds to ~10-20 ms, which
 * is well within the human input-latency budget while keeping the idle
 * NAK busy-wait (a few hundred microseconds per poll) cheap. */
#define HID_POLL_MS             10u
#define HID_SPIN_BUDGET         400000u

#define HID_MAX                 6

typedef struct {
    bool          in_use;
    usb_device_t *dev;
    bool          is_keyboard;
    bool          is_mouse;
    uint16_t      mps;            /* interrupt-IN max packet (<=8 for boot) */
    uint32_t      next_poll_ms;
    uint8_t       prev[8];        /* previous keyboard report (for diffing) */
    /* Software typematic.  A real PS/2 keyboard repeats held keys in
     * HARDWARE; a native-USB boot keyboard only reports state changes, so
     * without this a held Backspace deleted exactly one character.  The
     * most recent non-modifier key repeats after a short delay, system-
     * wide, because the repeats enter the same scancode pipeline. */
    uint8_t       held_key;       /* HID usage currently repeating (0=none) */
    uint32_t      next_repeat_ms;
} hid_dev_t;

#define HID_TYPEMATIC_DELAY_MS  400u   /* hold this long before repeating  */
#define HID_TYPEMATIC_RATE_MS    45u   /* then one repeat per this period  */

static hid_dev_t g_hids[HID_MAX];

/* ---------- HID usage -> PS/2 set-1 scancode ---------------------------- *
 * Encoding: 0 = unmapped; low byte = scancode; bit 8 (0x100) = extended
 * (emit a 0xE0 prefix first).  Covers the standard 101/102-key layout. */
static const uint16_t hid2set1[0x100] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, /* a b c d */
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23, /* e f g h */
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26, /* i j k l */
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19, /* m n o p */
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14, /* q r s t */
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D, /* u v w x */
    [0x1C] = 0x15, [0x1D] = 0x2C,                               /* y z     */
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, /* 1 2 3 4 */
    [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09, /* 5 6 7 8 */
    [0x26] = 0x0A, [0x27] = 0x0B,                               /* 9 0     */
    [0x28] = 0x1C, /* Enter */   [0x29] = 0x01, /* Esc */
    [0x2A] = 0x0E, /* Backspc*/  [0x2B] = 0x0F, /* Tab */
    [0x2C] = 0x39, /* Space */   [0x2D] = 0x0C, /* - */
    [0x2E] = 0x0D, /* = */       [0x2F] = 0x1A, /* [ */
    [0x30] = 0x1B, /* ] */       [0x31] = 0x2B, /* backslash */
    [0x32] = 0x2B, /* non-US # */[0x33] = 0x27, /* ; */
    [0x34] = 0x28, /* ' */       [0x35] = 0x29, /* ` */
    [0x36] = 0x33, /* , */       [0x37] = 0x34, /* . */
    [0x38] = 0x35, /* / */       [0x39] = 0x3A, /* CapsLock */
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, /* F1-F4  */
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42, /* F5-F8  */
    [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58, /* F9-F12 */
    [0x47] = 0x46, /* ScrollLock */
    [0x49] = 0x100 | 0x52, /* Insert   */
    [0x4A] = 0x100 | 0x47, /* Home     */
    [0x4B] = 0x100 | 0x49, /* PageUp   */
    [0x4C] = 0x100 | 0x53, /* Delete   */
    [0x4D] = 0x100 | 0x4F, /* End      */
    [0x4E] = 0x100 | 0x51, /* PageDown */
    [0x4F] = 0x100 | 0x4D, /* Right    */
    [0x50] = 0x100 | 0x4B, /* Left     */
    [0x51] = 0x100 | 0x50, /* Down     */
    [0x52] = 0x100 | 0x48, /* Up       */
    [0x53] = 0x45, /* NumLock */
    [0x54] = 0x100 | 0x35, /* KP /  */  [0x55] = 0x37, /* KP * */
    [0x56] = 0x4A, /* KP - */           [0x57] = 0x4E, /* KP + */
    [0x58] = 0x100 | 0x1C, /* KP Enter */
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, /* KP 1 2 3 */
    [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D, /* KP 4 5 6 */
    [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49, /* KP 7 8 9 */
    [0x62] = 0x52, /* KP 0 */ [0x63] = 0x53, /* KP . */
    [0x64] = 0x56, /* non-US backslash */
};

/* The 8 modifier bits in byte 0 of a boot keyboard report -> set-1 codes. */
static const uint16_t mod_sc[8] = {
    0x1D,          /* bit0 Left Ctrl  */
    0x2A,          /* bit1 Left Shift */
    0x38,          /* bit2 Left Alt   */
    0x100 | 0x5B,  /* bit3 Left GUI   */
    0x100 | 0x1D,  /* bit4 Right Ctrl */
    0x36,          /* bit5 Right Shift*/
    0x100 | 0x38,  /* bit6 Right Alt  */
    0x100 | 0x5C,  /* bit7 Right GUI  */
};

/* Feed a set-1 scancode (make or break) into the keyboard pipeline,
 * emitting the 0xE0 prefix first for extended keys. */
static void emit_sc(uint16_t v, bool brk) {
    if (!v) return;
    if (v & 0x100) keyboard_handle_byte(0xE0);
    uint8_t sc = (uint8_t)(v & 0xFF);
    keyboard_handle_byte(brk ? (uint8_t)(sc | 0x80) : sc);
}

static hid_dev_t *hid_alloc(void) {
    for (int i = 0; i < HID_MAX; i++)
        if (!g_hids[i].in_use) {
            memset(&g_hids[i], 0, sizeof(g_hids[i]));
            g_hids[i].in_use = true;
            return &g_hids[i];
        }
    return NULL;
}

/* ---------- Report processing ------------------------------------------- */
static bool key_in_report(const uint8_t *rpt, uint8_t key) {
    for (int i = 2; i < 8; i++) if (rpt[i] == key) return true;
    return false;
}

static void kbd_report(hid_dev_t *h, const uint8_t *r) {
    uint8_t mods = r[0], pmods = h->prev[0];
    for (int b = 0; b < 8; b++) {
        uint8_t m = (uint8_t)(1u << b);
        if ((mods & m) && !(pmods & m))      emit_sc(mod_sc[b], false);
        else if (!(mods & m) && (pmods & m)) emit_sc(mod_sc[b], true);
    }
    /* Releases: keys present last time but not now. */
    for (int i = 2; i < 8; i++) {
        uint8_t k = h->prev[i];
        if (k >= 4 && !key_in_report(r, k)) emit_sc(hid2set1[k], true);
    }
    /* Presses: keys present now but not last time.  (Codes 1-3 are
     * error/rollover indicators, not real keys.) */
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k >= 4 && !key_in_report(h->prev, k)) {
            emit_sc(hid2set1[k], false);
            /* Typematic re-targets to the newest press (PC behaviour). */
            h->held_key       = k;
            h->next_repeat_ms = pit_ms() + HID_TYPEMATIC_DELAY_MS;
        }
    }
    /* Stop repeating once the repeating key is no longer held. */
    if (h->held_key && !key_in_report(r, h->held_key))
        h->held_key = 0;
    memcpy(h->prev, r, 8);
}

static void mouse_report(hid_dev_t *h, const uint8_t *r) {
    (void)h;
    uint8_t b = r[0];
    int dx = (int)(int8_t)r[1];
    int dy = (int)(int8_t)r[2];
    uint8_t btns = 0;
    if (b & 0x01) btns |= MOUSE_BTN_LEFT;
    if (b & 0x02) btns |= MOUSE_BTN_RIGHT;
    if (b & 0x04) btns |= MOUSE_BTN_MIDDLE;
    /* Boot-protocol mice are 3 bytes (no wheel); movement + buttons only. */
    mouse_inject(dx, dy, 0, btns);
}

/* ---------- Public API -------------------------------------------------- */
bool usbhid_attach(usb_device_t *dev) {
    if (!dev || dev->iface_class != USB_CLASS_HID) return false;
    int proto = dev->iface_protocol;
    if (proto != HID_PROTO_KEYBOARD && proto != HID_PROTO_MOUSE) {
        debug_printf("[usbhid] addr=%d HID proto=%d not boot kbd/mouse - skipped\n",
                     dev->address, proto);
        return false;
    }
    if (!(dev->ep_in & 0x80)) {
        debug_printf("[usbhid] addr=%d has no interrupt-IN endpoint\n", dev->address);
        return false;
    }

    hid_dev_t *h = hid_alloc();
    if (!h) { debug_printf("[usbhid] table full\n"); return false; }
    h->dev         = dev;
    h->is_keyboard = (proto == HID_PROTO_KEYBOARD);
    h->is_mouse    = (proto == HID_PROTO_MOUSE);
    h->mps         = dev->ep_in_mps ? dev->ep_in_mps : 8;
    if (h->mps > 8) h->mps = 8;            /* boot reports never exceed 8 */
    h->next_poll_ms = 0;
    memset(h->prev, 0, sizeof(h->prev));
    dev->toggle_in  = 0;

    /* SET_PROTOCOL = Boot (0). */
    usb_setup_t sp = { HID_RT_CLASS_IFACE_OUT, HID_REQ_SET_PROTOCOL,
                       0x0000, dev->iface_num, 0 };
    usb_control_transfer(dev, &sp, NULL, 0);
    /* SET_IDLE = 0 -> report only on change (keeps idle polls quiet). */
    usb_setup_t si = { HID_RT_CLASS_IFACE_OUT, HID_REQ_SET_IDLE,
                       0x0000, dev->iface_num, 0 };
    usb_control_transfer(dev, &si, NULL, 0);

    debug_printf("[usbhid] %s attached: addr=%d ep_in=0x%02x mps=%u\n",
                 h->is_keyboard ? "keyboard" : "mouse",
                 dev->address, dev->ep_in, h->mps);
    return true;
}

void usbhid_detach(usb_device_t *dev) {
    for (int i = 0; i < HID_MAX; i++) {
        hid_dev_t *h = &g_hids[i];
        if (!h->in_use || h->dev != dev) continue;
        /* Release anything still held so a yanked keyboard can't leave a
         * modifier (Shift/Ctrl/Alt) latched down. */
        if (h->is_keyboard) {
            for (int b = 0; b < 8; b++)
                if (h->prev[0] & (1u << b)) emit_sc(mod_sc[b], true);
            for (int k = 2; k < 8; k++)
                if (h->prev[k] >= 4) emit_sc(hid2set1[h->prev[k]], true);
        }
        h->in_use = false;
        h->dev    = NULL;
        debug_printf("[usbhid] detached addr=%d\n", dev->address);
    }
}

void usbhid_counts(int *keyboards, int *mice) {
    int kb = 0, ms = 0;
    for (int i = 0; i < HID_MAX; i++) {
        hid_dev_t *h = &g_hids[i];
        if (!h->in_use || !h->dev || !h->dev->in_use) continue;
        if (h->is_keyboard) kb++;
        else if (h->is_mouse) ms++;
    }
    if (keyboards) *keyboards = kb;
    if (mice)      *mice      = ms;
}

void usbhid_poll(void) {
    uint32_t now = pit_ms();
    for (int i = 0; i < HID_MAX; i++) {
        hid_dev_t *h = &g_hids[i];
        if (!h->in_use) continue;
        /* Device vanished without a clean detach (defensive). */
        if (!h->dev || !h->dev->in_use) { h->in_use = false; continue; }
        if (now < h->next_poll_ms) continue;
        h->next_poll_ms = now + HID_POLL_MS;

        /* Software typematic pump: while a key stays held (no new report
         * arrives for it), re-emit its make code on schedule. */
        if (h->is_keyboard && h->held_key &&
            (int32_t)(now - h->next_repeat_ms) >= 0) {
            emit_sc(hid2set1[h->held_key], false);
            h->next_repeat_ms = now + HID_TYPEMATIC_RATE_MS;
        }

        uint8_t rpt[8] = {0};
        uint32_t got = 0;
        int rc = usb_intr_in(h->dev, rpt, h->mps, &got, HID_SPIN_BUDGET);
        if (rc != 0 || got == 0) continue;   /* -3 = no report ready */

        if (h->is_keyboard)   kbd_report(h, rpt);
        else if (h->is_mouse) mouse_report(h, rpt);
    }
}
