/* ============================================================================
 * NexxoN OS - System tray  (v1.0)
 * ----------------------------------------------------------------------------
 * Right-side cluster of micro-icons in the taskbar:
 *   * Volume widget   - click toggles mute, scroll changes volume,
 *                       hover shows the current level.
 *   * Network icon    - polls netif state every second; renders a
 *                       crossed-out icon when no IPv4 lease is active.
 *   * Notify icon     - count badge for unread notifications.
 *
 * Each applet implements draw + click + hover.  The taskbar lays them
 * out from right to left with a fixed 20 px stride.
 * ============================================================================ */
#ifndef NEXXON_TRAY_H
#define NEXXON_TRAY_H

#include "types.h"
#include "gfx.h"

#define TRAY_ICON_W       18
#define TRAY_ICON_H       18
#define TRAY_STRIDE       28
#define TRAY_MARGIN       8

/* The interactive horizontal volume slider that lives to the left of the
 * tray icons.  Width in pixels (track area, not including the leading
 * speaker glyph). */
#define TRAY_VOL_SLIDER_W 96
#define TRAY_VOL_SLIDER_H 12
#define TRAY_VOL_GLYPH_W  16
#define TRAY_VOL_TOTAL_W  (TRAY_VOL_GLYPH_W + TRAY_VOL_SLIDER_W + 4)

void tray_draw            (draw_target_t *t, int taskbar_top, int screen_w);
bool tray_handle_click    (int screen_x, int screen_y, bool right_btn);

/* #11: network-info popup (toggled by the tray network icon). */
bool tray_netinfo_open    (void);
void tray_netinfo_close   (void);
void tray_handle_scroll   (int dz);
/* TASK GUI overhaul: continuous drag tracking on the volume slider.
 * The WM forwards mouse-move events while the left button is held; if
 * the cursor lands inside the slider track, the master volume is
 * updated proportionally.  Returns true if the slider was being
 * tracked (so the caller can swallow the move and skip other hit-
 * tests for the same tick). */
bool tray_handle_drag     (int screen_x, int screen_y, bool left_btn);

/* Geometry probes used by the taskbar layout code to avoid bleeding
 * into the clock / network / notify slots. */
int  tray_volume_x        (int screen_w);
int  tray_cluster_left    (int screen_w);
int  tray_cluster_right   (int screen_w);

/* Quick state probes for the volume widget and a hover-bubble. */
int  tray_volume_get      (void);
void tray_volume_set      (int v);
void tray_volume_mute     (bool m);
bool tray_volume_muted    (void);

#endif /* NEXXON_TRAY_H */
