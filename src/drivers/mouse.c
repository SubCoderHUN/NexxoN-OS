/* ============================================================================
 * NexxoN OS - PS/2 auxiliary-device (mouse) driver  (v3.0, bare-metal hardened)
 * ----------------------------------------------------------------------------
 * v3.0 rewrite focuses on bare-metal reliability and boot-time latency:
 *
 *   * Every 8042 port-poll uses millisecond-bounded waits via pit_ms()
 *     instead of uncalibrated `for (i=0; i<100000; ...)` busy loops.
 *     On a 3 GHz Xeon those loops finish in ~30 µs; on a 333 MHz Atom
 *     they take ~300 µs.  Worse, when the controller never asserts
 *     OBF/IBF the loop completes and we silently miss a hand-shake.
 *     The new timeouts always reflect real time, not CPU speed.
 *
 *   * Reset (0xFF) + Basic Assurance Test (BAT, expects 0xAA, 0x00).
 *     Some PCH-based BIOSes leave the aux device in an unknown state
 *     post-hand-off; only a full reset reliably brings it back to
 *     reporting.
 *
 *   * IRQ12 is explicitly unmasked at the PIC level after the handler
 *     is installed.  The new pic_remap() leaves every line masked, so
 *     without this call the mouse ISR never fires on bare metal.
 *
 *   * IntelliMouse magic knock (200/100/80 Hz sample rate, then GET_ID)
 *     uses bounded byte reads instead of fixed iteration loops.
 *
 *   * Hard-fail path: if the controller never ACKs anything we still
 *     install the IRQ handler and unmask IRQ12 so that, if the device
 *     starts streaming later, the cursor still updates.
 * ============================================================================ */
#include "mouse.h"
#include "keyboard.h"
#include "io.h"
#include "irq.h"
#include "isr.h"
#include "debug.h"
#include "apps.h"
#include "pit.h"
#include "pic.h"

#define PS2_DATA      0x60
#define PS2_STATUS    0x64
#define PS2_CMD       0x64

/* 8042 controller commands */
#define CMD_DISABLE_PORT1   0xAD
#define CMD_ENABLE_PORT1    0xAE
#define CMD_DISABLE_PORT2   0xA7
#define CMD_ENABLE_PORT2    0xA8
#define CMD_READ_CONFIG     0x20
#define CMD_WRITE_CONFIG    0x60
#define CMD_TO_MOUSE        0xD4

/* Mouse-side commands */
#define MOUSE_RESET         0xFF
#define MOUSE_SET_DEFAULTS  0xF6
#define MOUSE_ENABLE        0xF4
#define MOUSE_SAMPLE_RATE   0xF3
#define MOUSE_GET_ID        0xF2
#define MOUSE_ACK           0xFA
#define MOUSE_RESEND        0xFE
#define MOUSE_BAT_OK        0xAA

/* Per-operation timeout caps.  10 ms is comfortably longer than the
 * 1-2 ms a healthy 8042 takes to ACK a command, and short enough that
 * a broken controller can't stall the boot for more than a few hundred
 * ms total. */
#define PS2_WAIT_MS         20
#define PS2_LONG_WAIT_MS    200

/* ---------- Driver state (writer = IRQ12, reader = compositor) ---------- */
static volatile int      g_x = 0;
static volatile int      g_y = 0;
static volatile uint8_t  g_btn = 0;
/* Press-edge latch: a click shorter than one compositor frame (heavy JPEG
 * decode in browser_tick, modal repaint...) used to flip g_btn down+up
 * entirely BETWEEN two mouse_poll() calls — the WM never saw the press and
 * the click was silently swallowed.  Press transitions are OR-ed in here
 * by the IRQ writers and folded into exactly one subsequent poll. */
static volatile uint8_t  g_btn_latch = 0;
static volatile bool     g_dirty = false;

static volatile int      g_bound_x = 1023;
static volatile int      g_bound_y = 767;

int g_mouse_sensitivity = MOUSE_SENS_DEFAULT;

/* Clamp + apply a new pointer sensitivity.  Single source of truth so the
 * PS/2 path, the USB-HID inject path and the Settings UI all agree. */
void mouse_set_sensitivity(int s) {
    if (s < MOUSE_SENS_MIN) s = MOUSE_SENS_MIN;
    if (s > MOUSE_SENS_MAX) s = MOUSE_SENS_MAX;
    g_mouse_sensitivity = s;
}
int mouse_get_sensitivity(void) {
    if (g_mouse_sensitivity < MOUSE_SENS_MIN) return MOUSE_SENS_MIN;
    if (g_mouse_sensitivity > MOUSE_SENS_MAX) return MOUSE_SENS_MAX;
    return g_mouse_sensitivity;
}

/* ---------- Pointer transfer function (fixed-point, sub-pixel) ----------- *
 * The cursor moves by a fixed-point gain applied to the raw delta, with the
 * fractional remainder carried across reports (Q8: 256 == 1.0x).  Two design
 * points the bare-metal user asked for, in order:
 *   1. No "jumping between pixels": the sub-pixel carry means even a gain below
 *      1.0 reaches every pixel smoothly instead of quantising to multiples.
 *   2. PREDICTABLE, i.e. LINEAR — the cursor distance is strictly proportional
 *      to how far the mouse moved, NOT how fast.  An earlier speed-based
 *      acceleration made fast flicks jump unpredictably; it is removed.  The
 *      sensitivity slider is a plain linear scale: each step adds
 *      MOUSE_GAIN_PER_STEP of gain, so the cursor speed is the same ratio at any
 *      pointer speed.  The base is tuned brisk — even the old maximum felt slow
 *      — so the DEFAULT setting is already ~3x and the slider reaches ~7.5x. */
#define MOUSE_FP_SHIFT       8
#define MOUSE_FP_ONE         (1 << MOUSE_FP_SHIFT)   /* 256 == 1.0x            */
#define MOUSE_GAIN_PER_STEP  192                     /* Q8 gain per slider step */

/* ---- Motion smoothing: pay reports out BETWEEN frames ------------------ *
 * Reports arrive every ~10 ms (the USB HID poll cadence and the PS/2 100 Hz
 * sample rate alike) and the old code moved the cursor by the WHOLE
 * gain-scaled delta in the frame the report landed in: at the default 3.0x
 * gain a modest move stepped 15-30 px once per report — the "pixely"
 * cursor, worse the higher the sensitivity.  Now a report only BANKS its
 * gain-scaled motion (Q8) and mouse_poll() — the compositor's per-frame
 * read — pays it out LINEARLY over one report period, so the same distance
 * is drawn as several small per-frame steps instead of one jump.
 *   - Linear, NOT accelerated: the payout reproduces the banked distance
 *     exactly (the Q8 carry survives in g_acc_*), so physical distance ->
 *     cursor distance stays strictly proportional at any hand speed.
 *   - Worst-case added latency is one report period (~10 ms): the
 *     theoretical minimum for de-quantising a 10 ms input stream.
 *   - Pacing needs sub-tick time (PIT ticks are the same 10 ms as the
 *     reports): pit_now_us() reads the live PIT channel-0 counter. */
#define MOUSE_SMOOTH_FAST_US   11000   /* payout window for fast motion    */
#define MOUSE_SMOOTH_SLOW_US   45000   /* ...and for tiny, precise motion  */
#define MOUSE_SMOOTH_TAIL_US    6000   /* graceful tail instead of a flush */
#define MOUSE_SMOOTH_MAX_DT_US 20000   /* clamp frame stalls               */

static int      g_pend_x = 0, g_pend_y = 0;   /* banked Q8 motion (IRQ adds) */
static int      g_acc_x  = 0, g_acc_y  = 0;   /* sub-pixel emission carry    */
static uint32_t g_pay_deadline_us = 0;        /* when the bank should empty  */
static uint32_t g_last_drain_us   = 0;

/* Translate a raw relative delta (already in screen orientation: +x right,
 * +y down) into banked cursor motion.  Shared by the PS/2 IRQ path and the
 * native USB-HID inject path so they behave identically.  May run in IRQ
 * context — it only adds to the bank; the drain below runs at task level.
 *
 * The payout window ADAPTS to how much motion is banked.  Audit result of
 * the "small moves visibly hop" report: at the default sensitivity one
 * count is a 3 px quantum, and spreading 3 px over a fixed 11 ms is less
 * than ONE 60 Hz refresh — invisible.  Tiny motions now spread over
 * ~45 ms (= 3 refreshes -> three 1 px steps: reads as movement), while
 * large banks keep the short window so flicks stay immediate.  Lag is
 * bounded by velocity x window: at slow speeds that is under 2 px. */
static void mouse_apply_motion(int dx, int dy) {
    int s    = mouse_get_sensitivity();                /* 1..MOUSE_SENS_MAX */
    int gain = s * MOUSE_GAIN_PER_STEP;                /* linear, Q8        */
    g_pend_x += dx * gain;
    g_pend_y += dy * gain;
    int ax = g_pend_x < 0 ? -g_pend_x : g_pend_x;
    int ay = g_pend_y < 0 ? -g_pend_y : g_pend_y;
    int mag = (ax > ay ? ax : ay) >> MOUSE_FP_SHIFT;   /* banked px         */
    uint32_t window;
    if (mag <= 4)        window = MOUSE_SMOOTH_SLOW_US;
    else if (mag >= 16)  window = MOUSE_SMOOTH_FAST_US;
    else window = MOUSE_SMOOTH_SLOW_US -
                  (uint32_t)(mag - 4) *
                  ((MOUSE_SMOOTH_SLOW_US - MOUSE_SMOOTH_FAST_US) / 12u);
    g_pay_deadline_us = pit_now_us() + window;
}

/* Per-frame payout — called from mouse_poll(), i.e. once per wm_tick()
 * composite (modal dialog loops pump wm_tick too, so motion never stalls). */
static void mouse_motion_drain(void) {
    uint32_t now = pit_now_us();
    uint32_t dt  = now - g_last_drain_us;
    if (dt == 0) return;
    if (dt > MOUSE_SMOOTH_MAX_DT_US) dt = MOUSE_SMOOTH_MAX_DT_US;
    g_last_drain_us = now;

    /* The IRQ12 / PIT-drain writers only ADD to the bank: snapshot and
     * write back with interrupts off (same pattern as mouse_scroll_delta). */
    __asm__ volatile ("cli");
    int px = g_pend_x, py = g_pend_y;
    if (px == 0 && py == 0) { __asm__ volatile ("sti"); return; }
    int32_t remain = (int32_t)(g_pay_deadline_us - now);
    /* Past-deadline residue glides out over a short tail instead of
     * flushing in one visible hop (a real stall still flushes: the
     * clamped dt exceeds the tail). */
    if (remain < (int32_t)MOUSE_SMOOTH_TAIL_US)
        remain = (int32_t)MOUSE_SMOOTH_TAIL_US;
    int tx, ty;
    if ((int32_t)dt >= remain) {
        tx = px; ty = py;               /* stall catch-up: flush the rest */
    } else {
        /* Linear share of the remaining window: pend * dt / remain, split
         * so every intermediate fits in 32 bits (the freestanding kernel
         * links no libgcc, so no 64-bit division). */
        tx = (px / remain) * (int)dt + ((px % remain) * (int)dt) / remain;
        ty = (py / remain) * (int)dt + ((py % remain) * (int)dt) / remain;
    }
    g_pend_x = px - tx;
    g_pend_y = py - ty;
    __asm__ volatile ("sti");

    int fx = g_acc_x + tx;
    int fy = g_acc_y + ty;
    int mvx = fx >> MOUSE_FP_SHIFT;                /* arithmetic shift: floors */
    int mvy = fy >> MOUSE_FP_SHIFT;
    g_acc_x = fx - (mvx << MOUSE_FP_SHIFT);        /* remainder in [0,255] */
    g_acc_y = fy - (mvy << MOUSE_FP_SHIFT);
    if (!mvx && !mvy) return;

    int nx = g_x + mvx;
    int ny = g_y + mvy;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > g_bound_x) nx = g_bound_x;
    if (ny > g_bound_y) ny = g_bound_y;
    g_x = nx;
    g_y = ny;
    g_dirty = true;
}

static volatile uint8_t  g_packet[4];
static volatile int      g_packet_idx = 0;
static volatile bool     g_has_wheel  = false;
static volatile int      g_scroll_acc = 0;

static volatile uint32_t g_last_byte_ms = 0;

/* Diagnostic counters (bare metal: tells us whether firmware USB-legacy is
 * actually STREAMING aux/mouse bytes, vs only answering the init handshake).
 * Surfaced by mouse_diag() in the shell `inf` output. */
static volatile uint32_t g_aux_bytes   = 0;   /* bytes routed to the mouse  */
static volatile uint32_t g_aux_packets = 0;   /* complete packets assembled */
static volatile uint32_t g_inject_reports = 0;/* native USB-HID reports injected */

/* ---------- Low-level 8042 plumbing (timeout-bounded) ------------------- */
/* Wait until status_bit clears (mask in, expect == 0).  Returns 0 on
 * success, -1 on timeout.  Bounded by both pit_ms wall time AND a hard
 * iteration cap so we work even before pit_init() has fired its first
 * tick (in which case pit_ms() returns 0 forever). */
static int ps2_wait_status(uint8_t mask, bool expect_set, uint32_t timeout_ms) {
    uint32_t start = pit_ms();
    uint32_t spin  = 0;
    while (1) {
        uint8_t s = inb(PS2_STATUS);
        bool   is_set = (s & mask) != 0;
        if (is_set == expect_set) return 0;
        /* Time-based exit (preferred, accurate). */
        if (timeout_ms > 0 && (pit_ms() - start) > timeout_ms) return -1;
        /* Spin-count exit (only kicks in when pit_ms is stuck at zero).
         * 1e6 iterations is ~100 ms on a Pentium-3, ~30 ms on a modern
         * CPU.  Always finite. */
        if (++spin > 1000000u) return -1;
    }
}

static int ps2_wait_input_empty(uint32_t timeout_ms)  {
    return ps2_wait_status(0x02, false, timeout_ms);
}
static int ps2_wait_output_full(uint32_t timeout_ms)  {
    return ps2_wait_status(0x01, true, timeout_ms);
}

/* Issue an 8042-side command (0x64 port write). */
static int ps2_cmd(uint8_t c) {
    if (ps2_wait_input_empty(PS2_WAIT_MS) < 0) return -1;
    outb(PS2_CMD, c);
    return 0;
}

/* Write to the 0x60 data port (used both as keyboard data and as the
 * byte that follows a multi-byte 8042 command sequence). */
static int ps2_data_write(uint8_t b) {
    if (ps2_wait_input_empty(PS2_WAIT_MS) < 0) return -1;
    outb(PS2_DATA, b);
    return 0;
}

/* Read a single byte from the data port, with timeout.  Returns -1 on
 * timeout, the byte value (0..255) on success. */
static int ps2_data_read(uint32_t timeout_ms) {
    if (ps2_wait_output_full(timeout_ms) < 0) return -1;
    return (int)inb(PS2_DATA);
}

/* Send a single byte to the mouse and consume the ACK.  Returns 0 on
 * ACK, -1 on timeout / NAK / Resend. */
static int mouse_write(uint8_t b) {
    if (ps2_cmd(CMD_TO_MOUSE)   < 0) return -1;
    if (ps2_data_write(b)       < 0) return -1;
    int r = ps2_data_read(PS2_WAIT_MS);
    if (r < 0) return -1;
    if (r == MOUSE_RESEND) {
        /* One-shot resend on Resend response. */
        if (ps2_cmd(CMD_TO_MOUSE)   < 0) return -1;
        if (ps2_data_write(b)       < 0) return -1;
        r = ps2_data_read(PS2_WAIT_MS);
    }
    return (r == MOUSE_ACK) ? 0 : -1;
}

/* ---------- Packet state machine (called by ps2_dispatch) --------------- */
/* Process ONE byte that ps2_dispatch() already read from the i8042 output
 * buffer with the AUX (mouse) status bit set.  The read + AUX routing now
 * live in ps2_dispatch() (keyboard.c) so that the byte gets consumed -- and
 * the cursor updated -- whether it arrived via IRQ12 or via the IRQ0/PIT
 * safety drain when IRQ12 isn't delivered (VirtualBox / strict 8042s). */
void mouse_handle_byte(uint8_t b) {
    screensaver_kick();
    g_aux_bytes++;

    uint32_t now = pit_ms();
    if (g_packet_idx > 0 && now - g_last_byte_ms > 500u) {
        g_packet_idx = 0;
    }
    g_last_byte_ms = now;

    if (g_packet_idx == 0 && !(b & 0x08)) return;

    g_packet[g_packet_idx++] = b;
    int need = g_has_wheel ? 4 : 3;
    if (g_packet_idx < need) return;

    g_packet_idx = 0;
    g_aux_packets++;
    uint8_t flags = g_packet[0];
    int dx = (int)g_packet[1];
    int dy = (int)g_packet[2];
    if (flags & 0x10) dx -= 256;
    if (flags & 0x20) dy -= 256;
    dy = -dy;

    if (g_has_wheel) {
        int dz = (int)(int8_t)((g_packet[3] & 0x0F) | ((g_packet[3] & 0x08) ? 0xF0 : 0));
        if (dz) g_scroll_acc -= dz;
    }

    if (!(flags & 0xC0)) {
        mouse_apply_motion(dx, dy);
    }

    uint8_t nb = (uint8_t)(flags & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE));
    g_btn_latch |= (uint8_t)(nb & ~g_btn);     /* remember press edges */
    g_btn = nb;
    g_dirty = true;
}

/* Inject a relative movement / button / wheel update from a non-PS/2
 * source (the USB HID mouse driver).  Coordinates follow HID semantics:
 * dx>0 = right, dy>0 = down, dz>0 = wheel up.  Reuses the same sensitivity,
 * clamping and scroll accumulation as the PS/2 path so behaviour is
 * identical regardless of how the mouse is attached. */
void mouse_inject(int dx, int dy, int dz, uint8_t buttons) {
    screensaver_kick();
    g_inject_reports++;   /* native USB-HID report (for diagnostics) */
    mouse_apply_motion(dx, dy);
    if (dz) g_scroll_acc += dz;
    uint8_t nb = (uint8_t)(buttons & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE));
    g_btn_latch |= (uint8_t)(nb & ~g_btn);     /* remember press edges */
    g_btn = nb;
    g_dirty = true;
}

/* ---------- IRQ12 handler ----------------------------------------------- */
/* Just drains the shared i8042 buffer; ps2_dispatch() routes each byte to
 * the keyboard or mouse handler by the AUX status bit. */
static void mouse_isr(registers_t *r) {
    (void)r;
    ps2_dispatch();
}

/* ---------- Drain helper (bounded) -------------------------------------- */
static void ps2_drain(uint32_t budget_ms) {
    uint32_t start = pit_ms();
    int drained = 0;
    while (drained < 64) {
        if (!(inb(PS2_STATUS) & 0x01)) break;
        (void)inb(PS2_DATA);
        drained++;
        if ((pit_ms() - start) > budget_ms) break;
    }
}

/* ---------- Init -------------------------------------------------------- */
void mouse_init(void) {
    debug_step("mouse: enabling PS/2 auxiliary device (gentle / SMM-safe)");

    /* WHY THIS IS "GENTLE" — the bug the user kept hitting:
     * On a BIOS USB-legacy machine (USB-3.0 keyboard + mouse, no PS/2 ports)
     * the 8042 the OS sees is an SMM *emulation* of the USB devices.  The
     * KEYBOARD works perfectly because keyboard_init() never disturbs that
     * emulation — it only drains the buffer and hooks IRQ1.  The old
     * mouse_init() instead ran an 8042 SELF-TEST (0xAA, which re-initialises
     * the whole controller), DISABLED both ports, and issued a full mouse
     * RESET (0xFF) + SET_DEFAULTS + IntelliMouse knock.  Any of those tears
     * the firmware's emulated mouse stream down, so the mouse went dead while
     * the keyboard stayed up — exactly "one works, the other doesn't".
     *
     * Fix: mirror the keyboard.  Touch the controller as little as possible:
     * enable the mouse IRQ/clock, enable data reporting, hook IRQ12.  No
     * self-test, no port-disable dance, no device reset, no sample-rate knock.
     * This still brings up a real PS/2 mouse (the BIOS already initialised it;
     * it just needs reporting on + IRQ12) and it leaves an SMM-emulated mouse
     * streaming. */
    ps2_dispatch_inhibit(true);

    /* Enable IRQ12 + mouse clock in the config byte; leave every other bit
     * (keyboard, translation) exactly as the firmware set it. */
    if (ps2_cmd(CMD_READ_CONFIG) == 0) {
        int cfg_r = ps2_data_read(PS2_WAIT_MS);
        if (cfg_r >= 0) {
            uint8_t cfg = (uint8_t)cfg_r;
            uint8_t orig = cfg;
            cfg |=  (1u << 1);   /* IRQ12 enable       */
            cfg &= ~(1u << 5);   /* mouse-clock enable */
            if (ps2_cmd(CMD_WRITE_CONFIG) == 0) ps2_data_write(cfg);
            debug_printf("[mouse] 8042 config: 0x%02x -> 0x%02x (gentle)\n",
                         orig, cfg);
        }
    }
    ps2_cmd(CMD_ENABLE_PORT2);   /* ensure aux port enabled (no-op if already) */
    ps2_drain(5);

    /* Enable data reporting only — NO reset / SET_DEFAULTS / sample-rate knock. */
    bool enable_ok = false;
    for (int r = 0; r < 3 && !enable_ok; r++)
        if (mouse_write(MOUSE_ENABLE) == 0) enable_ok = true;

    irq_install_handler(12, mouse_isr);
    /* IRQ12 lives on the slave PIC (cascade via master IR2); pic_unmask opens
     * the cascade too.  The 100 Hz PIT drain in ps2_dispatch() is the backstop
     * if IRQ12 itself is never delivered. */
    pic_unmask(12);
    ps2_dispatch_inhibit(false);

    debug_ok(enable_ok ? "mouse: ready (gentle init, reporting enabled)"
                       : "mouse: ready (gentle init, IRQ12 + PIT drain)");
}

/* Full PS/2 mouse handshake (reset + IntelliMouse knock).  Kept for an
 * explicit `mouse-reset` retry path, NOT run at boot, because on SMM-emulated
 * controllers the reset stops the firmware mouse stream.  Returns true if a
 * real device acknowledged the reset. */
bool mouse_full_reset(void) {
    ps2_dispatch_inhibit(true);
    bool ok = false;
    if (ps2_cmd(CMD_TO_MOUSE) == 0 && ps2_data_write(MOUSE_RESET) == 0) {
        int a = ps2_data_read(PS2_WAIT_MS);
        int b = ps2_data_read(PS2_LONG_WAIT_MS);
        (void)ps2_data_read(PS2_WAIT_MS);
        ok = (a == MOUSE_ACK && b == MOUSE_BAT_OK);
    }
    /* IntelliMouse knock for the scroll wheel. */
    mouse_write(MOUSE_SAMPLE_RATE); mouse_write(200);
    mouse_write(MOUSE_SAMPLE_RATE); mouse_write(100);
    mouse_write(MOUSE_SAMPLE_RATE); mouse_write(80);
    int dev_id = -1;
    if (ps2_cmd(CMD_TO_MOUSE) == 0 && ps2_data_write(MOUSE_GET_ID) == 0) {
        if (ps2_data_read(PS2_WAIT_MS) == MOUSE_ACK)
            dev_id = ps2_data_read(PS2_WAIT_MS);
    }
    if (dev_id == 0x03) g_has_wheel = true;
    mouse_write(MOUSE_SAMPLE_RATE); mouse_write(100);
    for (int r = 0; r < 3; r++) if (mouse_write(MOUSE_ENABLE) == 0) break;
    ps2_dispatch_inhibit(false);
    return ok;
}


int mouse_scroll_delta(void) {
    __asm__ volatile ("cli");
    int v = g_scroll_acc;
    g_scroll_acc = 0;
    __asm__ volatile ("sti");
    return v;
}

bool mouse_has_wheel(void) { return g_has_wheel; }

/* Diagnostic snapshot: how many aux bytes / packets the PS/2 path has seen.
 * On bare metal a steady 0 while the user moves the mouse means the firmware
 * USB-legacy handler answered our init handshake but does NOT stream the USB
 * mouse as PS/2 (common) -> the mouse needs native USB HID, not SMM legacy. */
void mouse_diag(uint32_t *bytes, uint32_t *packets, bool *wheel) {
    /* Report total activity from BOTH input paths: PS/2 aux bytes and native
     * USB-HID injected reports.  A USB mouse driven natively (post-usbnative)
     * feeds mouse_inject(), not the PS/2 aux stream, so we fold the inject
     * count in here — otherwise a perfectly working USB mouse reads as 0. */
    if (bytes)   *bytes   = g_aux_bytes + g_inject_reports;
    if (packets) *packets = g_aux_packets + g_inject_reports;
    if (wheel)   *wheel   = g_has_wheel;
}

bool mouse_poll(int *x, int *y, uint8_t *buttons) {
    mouse_motion_drain();          /* pay out banked motion for this frame */
    if (x) *x = g_x;
    if (y) *y = g_y;
    if (buttons) {
        /* Fold missed press edges into this snapshot exactly once: a
         * sub-frame click reads as pressed now and released on the next
         * poll, so the WM still sees a full press/release pair. */
        __asm__ volatile ("cli");
        uint8_t b = (uint8_t)(g_btn | g_btn_latch);
        g_btn_latch = 0;
        __asm__ volatile ("sti");
        *buttons = b;
    }
    if (!g_dirty) return false;
    g_dirty = false;
    return true;
}

int     mouse_x  (void) { return g_x; }
int     mouse_y  (void) { return g_y; }
uint8_t mouse_btn(void) { return g_btn; }

void mouse_set_bounds(int max_x, int max_y) {
    g_bound_x = max_x;
    g_bound_y = max_y;
    if (g_x > max_x) g_x = max_x;
    if (g_y > max_y) g_y = max_y;
}
