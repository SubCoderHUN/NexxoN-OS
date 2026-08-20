/* ============================================================================
 * NexxoN OS - Context menu (compositor floating menu)  (v1.0)
 * ----------------------------------------------------------------------------
 * A right-click in the explorer (or any future GUI consumer) spawns a small
 * floating menu at the cursor.  The compositor renders the menu above all
 * windows but below the mouse cursor, and the WM dispatches the next click
 * to the menu before any window hit-test - so the menu always wins.
 *
 * One menu can be active at a time.  Items are copied into a static buffer
 * on open() so the caller doesn't have to keep its own array alive.  Clicks
 * fire a callback with the item's id; clicks outside the menu close it
 * silently.
 *
 * No heap, no IRQ context calls - safe to invoke from the WM mouse handler.
 * ============================================================================ */
#ifndef NEXXON_CTXMENU_H
#define NEXXON_CTXMENU_H

#include "types.h"
#include "gfx.h"

#define CTXMENU_MAX_ITEMS   8
#define CTXMENU_LABEL_MAX   24

/* Action callback.  `item_id` is the id field of the clicked item; `user`
 * is whatever opaque pointer was passed to ctxmenu_open.  The menu is
 * already closed by the time the callback fires, so the callback is free
 * to open another menu or destroy windows. */
typedef void (*ctxmenu_cb_t)(int item_id, void *user);

typedef struct {
    char label[CTXMENU_LABEL_MAX];
    int  id;
    bool separator;       /* if true, label is ignored and a divider is drawn */
    bool dangerous;       /* draw label in red (e.g., Delete) */
    bool disabled;        /* draw label dimmed; click does nothing */
} ctxmenu_item_t;

/* Open a context menu at screen coordinate (x, y).  The position is
 * clipped so the menu always fits inside the screen.  `n_items` is
 * capped at CTXMENU_MAX_ITEMS - extra items are silently truncated.
 * Re-opening while another menu is up replaces it. */
void  ctxmenu_open (int x, int y,
                    const ctxmenu_item_t *items, int n_items,
                    ctxmenu_cb_t cb, void *user);
void  ctxmenu_close(void);
bool  ctxmenu_active(void);

/* Compositor hooks.  Called by the window manager. */
void  ctxmenu_draw (draw_target_t *t);

/* Return true if the click was consumed by the menu.  `pressed_btn` is
 * the rising-edge mask supplied by wm_handle_mouse (i.e., bits that
 * went 0->1 this tick).  The caller MUST check ctxmenu_active() first,
 * but the function is also safe to call when no menu is up - it just
 * returns false. */
bool  ctxmenu_handle_click(int mx, int my, uint8_t pressed_btn);

#endif /* NEXXON_CTXMENU_H */
