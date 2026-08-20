/* ============================================================================
 * NexxoN OS - PS/2 mouse driver  (v4.0)
 * ----------------------------------------------------------------------------
 * Talks to the 8042 keyboard controller's auxiliary device using the
 * canonical PS/2 init dance:
 *
 *      0xA8                enable aux device
 *      0x20 / 0x60         read/write "compaq status byte" - enable IRQ12,
 *                          re-enable mouse clock
 *      0xD4 <byte>         next data-port write is delivered to the mouse
 *      0xF6                set defaults
 *      0xF4                enable data reporting
 *
 * The mouse then delivers 3-byte packets via IRQ 12, parsed by mouse_isr:
 *
 *      byte 0   YO XO Ysign Xsign 1 Mid Right Left
 *      byte 1   X delta (9-bit signed: sign in byte 0)
 *      byte 2   Y delta (9-bit signed: sign in byte 0)
 *
 * Tracked state (X, Y, button mask, "dirty" flag) is exposed to the window
 * manager which polls it from the recompositor.
 * ============================================================================ */
#ifndef NEXXON_MOUSE_H
#define NEXXON_MOUSE_H

#include "types.h"

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

void mouse_init(void);

/* Feed one byte (already read from the i8042 output buffer with its AUX
 * bit set) into the PS/2 mouse packet state machine.  Called by
 * ps2_dispatch(); see keyboard.h. */
void mouse_handle_byte(uint8_t b);

/* Inject a relative movement from a non-PS/2 source (USB HID mouse).
 * dx>0=right, dy>0=down, dz>0=wheel up; buttons is a MOUSE_BTN_* mask. */
void mouse_inject(int dx, int dy, int dz, uint8_t buttons);

/* Snap-read the current pointer state.  Returns true (and clears the
 * internal dirty flag) if at least one packet has arrived since the last
 * call - the WM uses this as its "needs recompose" trigger. */
bool mouse_poll(int *x, int *y, uint8_t *buttons);

int     mouse_x   (void);
int     mouse_y   (void);
uint8_t mouse_btn (void);

/* The mouse is clamped to whatever screen extents the WM tells us. */
void mouse_set_bounds(int max_x, int max_y);

/* Pointer sensitivity (1..MAX): a LINEAR fixed-point gain shared by the PS/2 and
 * native USB-HID paths.  Cursor distance is proportional to how far the mouse
 * moved, not how fast (predictable — no acceleration); the sub-pixel remainder
 * is carried so every pixel is reachable (no "jumping between points").  Tuned
 * brisk: DEFAULT is already ~3x and the slider reaches ~7.5x.  Adjustable in Settings
 * (Gephaz) and persisted to /sys/gephaz.cfg, so it survives a reboot on RAMFS
 * and on an installed system alike. */
#define MOUSE_SENS_MIN      1
#define MOUSE_SENS_MAX      10
#define MOUSE_SENS_DEFAULT  4
void mouse_set_sensitivity(int s);
int  mouse_get_sensitivity(void);

/* TASK UI: IntelliMouse scroll-wheel.  Returns the accumulated Z delta
 * since the last call and atomically resets the counter.  Positive =
 * wheel up (away from user); negative = wheel down. */
int  mouse_scroll_delta(void);

/* True iff the IntelliMouse extension was successfully enabled at
 * boot, so the WM knows whether to wire the scroll route at all. */
bool mouse_has_wheel(void);

/* Diagnostic snapshot of the PS/2 aux byte/packet counters.  Any argument may
 * be NULL.  Used by the shell `inf` command so a bare-metal user can tell
 * whether the firmware actually streams the USB mouse as PS/2. */
void mouse_diag(uint32_t *bytes, uint32_t *packets, bool *wheel);

/* Full PS/2 mouse handshake (reset + IntelliMouse knock).  NOT run at boot
 * (it stops an SMM-emulated mouse stream); exposed for an explicit retry on a
 * real PS/2 mouse.  Returns true if a device acknowledged the reset. */
bool mouse_full_reset(void);

#endif /* NEXXON_MOUSE_H */
