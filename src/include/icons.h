/* ============================================================================
 * NexxoN OS - Vector pictogram icon set (Liquid Glass)
 * ----------------------------------------------------------------------------
 * Hand-drawn, anti-aliased, size-scalable app icons shared by the Start
 * menu, the taskbar tiles, the desktop shortcuts and any picker UI.  All
 * geometry is integer math scaled from the requested box size, so the same
 * icon renders crisply at 16 px (taskbar) and 48 px (desktop).
 * ============================================================================ */
#ifndef NEXXON_ICONS_H
#define NEXXON_ICONS_H

#include "types.h"
#include "gfx.h"

typedef enum {
    ICON_NONE = 0,
    ICON_TASKMGR,
    ICON_SETTINGS,
    ICON_EXPLORER,
    ICON_BROWSER,
    ICON_EDITOR,
    ICON_NEXSHEET,
    ICON_USERS,
    ICON_DEVMGR,
    ICON_STORE,
    ICON_MUSIC,
    ICON_INSTALLER,
    ICON_DISPLAYS,
    ICON_WIFI,
    ICON_BLUETOOTH,
    ICON_TRASH,
    ICON_POWER,
    ICON_RESTART,
    ICON_DOOM,
    ICON_PROGRAMS,
    ICON_COUNT
} icon_id_t;

/* Draw icon `id` into the sz x sz box at (x,y).  ICON_NONE draws nothing. */
void icon_draw(draw_target_t *t, int x, int y, int sz, icon_id_t id);

/* Map an app launch command key ("taskmgr", "settings", ...) to its icon.
 * Unknown / file / custom commands return ICON_NONE. */
icon_id_t icon_for_cmd(const char *cmd);

#endif /* NEXXON_ICONS_H */
