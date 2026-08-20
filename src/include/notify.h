/* ============================================================================
 * NexxoN OS - Notification center  (v1.0)
 * ----------------------------------------------------------------------------
 * Lightweight toast / banner overlay system.  notify_post() pushes a
 * message into a 4-slot ring; the compositor draws each live toast at
 * the bottom-right corner with a 5-second auto-dismiss timeout.
 * Non-blocking - the caller never waits for the user.
 *
 * Toasts are styled with the active theme and the active language's
 * STR_* tokens.  The compositor calls notify_tick() once per frame to
 * expire timed-out entries.
 * ============================================================================ */
#ifndef NEXXON_NOTIFY_H
#define NEXXON_NOTIFY_H

#include "types.h"

#define NOTIFY_TITLE_MAX  48
#define NOTIFY_BODY_MAX   128
#define NOTIFY_MAX        4
#define NOTIFY_TIMEOUT_MS 5000u

typedef enum {
    NOTIFY_INFO    = 0,
    NOTIFY_WARNING = 1,
    NOTIFY_ERROR   = 2,
    NOTIFY_SUCCESS = 3,
} notify_kind_t;

#define NOTIFY_ACTION_LABEL_MAX 24
typedef void (*notify_action_cb_t)(int slot_idx);

typedef struct {
    bool              in_use;
    notify_kind_t     kind;
    uint32_t          posted_ms;
    char              title[NOTIFY_TITLE_MAX];
    char              body[NOTIFY_BODY_MAX];
    char              action_label[NOTIFY_ACTION_LABEL_MAX];
    notify_action_cb_t action_cb;
} notify_slot_t;

void notify_init   (void);
int  notify_post   (notify_kind_t kind, const char *title, const char *body);
/* Post a toast with a clickable action button.  Returns the slot index. */
int  notify_post_action(notify_kind_t kind, const char *title, const char *body,
                        const char *action_label, notify_action_cb_t action_cb);
void notify_tick   (void);                /* compositor expires slots here  */
/* Handle a click at screen coordinate (sx, sy).  Returns true if consumed. */
bool notify_handle_click(int sx, int sy);

int           notify_count(void);
notify_slot_t *notify_slot(int idx);

/* Draw active toasts onto the screen target.  Called by the compositor
 * after the windows and taskbar are painted. */
#include "gfx.h"
void notify_draw   (draw_target_t *t);

#endif /* NEXXON_NOTIFY_H */
