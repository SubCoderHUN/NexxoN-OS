/* ============================================================================
 * NexxoN OS - Desktop Environment  (v2.0)
 * ----------------------------------------------------------------------------
 * v2.0 changes over v1:
 *   - Right-click on the desktop now opens a context menu (BUG 1).  Empty
 *     space gets "New Icon... / Refresh / About"; clicking on an icon
 *     gets "Open / Rename / Edit command / Delete / Properties".
 *   - Icons persist across reboots via /sys/desktop.cfg on NXFS (BUG 3).
 *     desktop_add_icon / desktop_remove_icon auto-save; desktop_load_icons
 *     is called at boot once NXFS is mounted.
 *   - UI facelift: gradient panel, sharper start button bevels, richer
 *     icon design (folder-sheet look with shadow), refined start menu
 *     padding and hover state (BUG 7).
 * ============================================================================ */
#ifndef NEXXON_DESKTOP_H
#define NEXXON_DESKTOP_H

#include "types.h"
#include "gfx.h"

#define DESKTOP_PANEL_H        40
#define DESKTOP_ICON_W         72
#define DESKTOP_ICON_H         64
#define DESKTOP_ICON_BOX       44    /* the coloured square (icon body)  */
#define DESKTOP_ICON_GAP_X     20
#define DESKTOP_ICON_GAP_Y     18
#define DESKTOP_ICON_LABEL_MAX 16
#define DESKTOP_CMD_MAX        96
#define DESKTOP_MAX_ICONS      32

/* On-disk persistence: hidden NXFS file holding label|cmd lines, one
 * per icon, no header.  Created lazily by desktop_save_icons(). */
#define DESKTOP_CFG_DIR        "sys"
#define DESKTOP_CFG_FILE       "desktop.cfg"

/* ---------- Lifecycle ---------------------------------------------------- */
void  desktop_init(void);

/* ---------- Geometry helpers (used by the WM to clip windows) ---------- */
int   desktop_panel_top(void);          /* y of the panel's top edge   */
int   desktop_usable_height(void);      /* screen height minus panel   */

/* ---------- Render hooks (called by the WM compositor) ----------------- */
void  desktop_draw_background(draw_target_t *t);    /* wallpaper + icons */
void  desktop_draw_panel    (draw_target_t *t);     /* always-on-top bar */
void  desktop_draw_menu     (draw_target_t *t);     /* start menu, if up */
/* Type-aware shortcut/file icon (app plaque, .txt/.cfg/file sheet, custom
 * tile) at (x,y) box size sz.  Shared by the desktop, Explorer, and the
 * shortcut / new-file pickers so every icon looks identical. */
void  desktop_draw_shortcut_icon(draw_target_t *t, int x, int y, int sz,
                                 const char *label, const char *cmd);
/* Populate a brand-new (empty) desktop with a few default shortcuts. */
void  desktop_seed_defaults(void);

/* ---------- Mouse handlers --------------------------------------------- *
 * Return values:
 *   panel_click  - true  = click consumed by panel/menu, WM must IGNORE it.
 *   icon_click   - true  = a desktop icon was hit, no further work.
 *   right_click  - true  = WM should mark dirty (we either opened a menu
 *                          or otherwise changed visible state).
 */
bool  desktop_handle_left_click_panel(int mx, int my);
bool  desktop_handle_left_click_icon (int mx, int my, uint32_t now_ms);
bool  desktop_handle_right_click     (int mx, int my);
/* TASK 9: right-click on a taskbar tile opens a "Bezarás / Restore"
 * context menu for the corresponding window.  Returns true when the
 * click landed on a tile and a menu was opened. */
bool  desktop_handle_right_click_panel(int mx, int my);

/* TASK 33: calendar popup attached to the taskbar clock.  Toggled on
 * by clicking the clock; closed by clicking outside or pressing the
 * clock again.  Rendered by the WM as part of desktop_draw_menu(). */
void  desktop_draw_calendar(draw_target_t *t);
bool  desktop_calendar_open(void);

/* Close the start menu (called by the WM when the user clicks outside
 * any active menu region so the menu doesn't get stuck open). */
void  desktop_close_menu(void);
bool  desktop_menu_open(void);

/* Toggle the start menu open/closed (with the open/close slide animation).
 * Driven by the Start button and by tapping the Windows key. */
void  desktop_toggle_menu(void);

/* ---------- Icon table API --------------------------------------------- *
 * desktop_add_icon: copies label + command into the next free slot.  Returns
 * the icon's slot index on success, -1 if the table is full or the label/
 * command is too long.  Auto-saves /sys/desktop.cfg on success so the icon
 * survives reboot.
 *
 * desktop_remove_icon / desktop_update_icon: same auto-save behaviour. */
int   desktop_add_icon   (const char *label, const char *cmdline);
int   desktop_remove_icon(int idx);
int   desktop_update_icon(int idx, const char *label, const char *cmdline);
void  desktop_clear_icons(void);
int   desktop_icon_count (void);

/* Persistence:
 *   desktop_load_icons - reads /sys/desktop.cfg and re-populates the icon
 *                        table.  Safe to call before NXFS is mounted
 *                        (no-op in that case).  Returns the icon count.
 *   desktop_save_icons - explicit save; normally invoked from the other
 *                        mutation helpers but exposed for completeness. */
int   desktop_load_icons (void);
int   desktop_save_icons (void);

/* Wallpaper:
 *   desktop_load_wallpaper - reads /wallpaper.bmp from NXFS and decodes
 *                            it via the BMP parser.  When successful,
 *                            desktop_draw_background() blits the image
 *                            instead of the default solid-colour
 *                            pattern (centred and clipped to fit).
 *                            Returns true on success. */
bool  desktop_load_wallpaper(void);
bool  desktop_has_wallpaper (void);

/* Pull a pending command queued by an icon double-click (or by the start
 * menu's "open shell" action, etc.).  Returns NULL when none pending;
 * otherwise returns a pointer to an internal buffer and clears the queue. */
const char *desktop_drain_pending_command(void);
bool        desktop_has_pending_command(void);

/* #10: start-menu search.  desktop_search_active() is true while the search
 * box has keyboard focus; the shell idle loop then forwards keystrokes to
 * desktop_handle_key() instead of treating them as shell input. */
bool        desktop_search_active(void);
void        desktop_handle_key(int c);

#endif /* NEXXON_DESKTOP_H */
