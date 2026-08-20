/* ============================================================================
 * NexxoN OS - Window manager  (v6.0)
 * ----------------------------------------------------------------------------
 * v6.0 highlights:
 *   * Linked-list framebuffer pool allocator with coalescing.  Slots can no
 *     longer leak: every wm_destroy_window returns the block to the pool and
 *     adjacent free blocks merge so future allocations of any size succeed
 *     until the pool is truly full.
 *   * Per-window resize handle.  Windows with `resizable = true` get a 12x12
 *     grip in the bottom-right corner.  Drag rebuilds the content framebuffer
 *     and fires an optional resize callback so the client can redraw.
 *   * UI facelift: title-bar gradient, sharper 3D bevels, better close button.
 * ============================================================================ */
#ifndef NEXXON_WINDOW_H
#define NEXXON_WINDOW_H

#include "types.h"
#include "gfx.h"

#define WM_MAX_WINDOWS    8
/* Aero Glass title bar: 30 px per spec. Left/right/bottom glass
 * borders are 8 px; the top uses the title bar itself. */
#define WM_TITLE_H        30
#define WM_BORDER         8
#define WM_TITLE_MAX      48
#define WM_RESIZE_GRIP    14
/* Aero-style Min/Close button size per spec: ~30x20. */
#define WM_BTN_W          30
#define WM_BTN_H          20

/* Per-window content-area click callback.
 *   (cx, cy)   - coordinates relative to the window's content rect (0,0 = top-left)
 *   pressed    - mask of buttons that went 0->1 this tick (MOUSE_BTN_*).
 *   btn        - current button state.
 * Returns true if the click was handled.  Windows that don't register a
 * callback are treated as inert content surfaces. */
struct window;
typedef bool (*win_content_click_t)(struct window *w, int cx, int cy,
                                    uint8_t pressed, uint8_t btn);

/* Per-window resize callback.  Fired after the WM has reallocated the
 * content framebuffer and updated w/h.  Client should re-render the
 * content at the new dimensions. */
typedef void (*win_resize_cb_t)(struct window *w);

/* Per-window destroy callback (ISSUE 1).  Fired by wm_destroy_window
 * just before the framebuffer is returned to the pool.  Apps that
 * cache the window_t* in static state MUST register this and clear
 * their pointer here so a later wm_create_window can't hand out the
 * same slot index and trick the app into rendering into another
 * window's content buffer ("Settings ghosting" bug).  Pointer is
 * invalid the instant the callback returns. */
typedef void (*win_destroy_cb_t)(struct window *w);

/* Per-window key callback (ISSUE 3).  Fired by the WM when this window
 * is focused and the keyboard ISR has buffered a keystroke.  Return
 * true if the key was consumed; false to let the shell's read_line see
 * it (the protected shell window itself never installs one). */
typedef bool (*win_key_handler_t)(struct window *w, int key);

/* TASK UI: scroll-wheel notifier.  `dz > 0` = wheel up, `dz < 0` = wheel
 * down (in "notches" — typically ±1 per physical detent). */
typedef bool (*win_scroll_handler_t)(struct window *w, int dz);

typedef struct window {
    bool          in_use;
    bool          visible;
    bool          minimized;         /* true = hidden from compositor, in taskbar only */
    bool          protected;         /* true = cannot be closed or dragged off */
    bool          resizable;         /* true = bottom-right grip enabled       */
    bool          maximized;         /* true = filling the desktop (above taskbar) */
    int           x, y;
    int           w, h;
    int           restore_x, restore_y, restore_w, restore_h; /* pre-maximize geometry */
    int           min_w, min_h;      /* minimum size during resize             */
    int           z;
    int           id;
    /* Taskbar pictogram (icon_id_t value; 0 = none).  Set by the owning app
     * via wm_set_icon right after creation; survives i18n title changes. */
    int           icon;
    char          title[WM_TITLE_MAX];
    /* Stable key for position memory (ISSUE 9): the title the window was
     * created with.  Survives later wm_set_title() calls (e.g. the editor
     * appending a filename) so the window reopens where it was closed. */
    char          pos_key[WM_TITLE_MAX];
    /* Open/close animation (ISSUE 13): timestamp (pit_ms) at which the
     * open animation started.  0 once the animation has finished. */
    uint32_t      anim_open_ms;
    bool          no_anim;            /* skip open/close fade (modal dialogs) */
    draw_target_t content;
    uint32_t      content_bytes;     /* bytes allocated in the pool for content.fb */

    /* Optional callback: receives left/right clicks that land inside the
     * content area (NOT the title bar or border).  Used by the explorer
     * to react to single-click selection, double-click open, and
     * right-click context menu. */
    win_content_click_t content_click;
    void               *content_user;

    /* Optional callback fired after the WM resizes this window. */
    win_resize_cb_t     resize_cb;

    /* Optional callback fired just before the WM destroys this window.
     * Lets apps null out their cached window pointer (ISSUE 1). */
    win_destroy_cb_t    destroy_cb;

    /* Optional keystroke handler invoked from wm_tick when this window
     * is focused (ISSUE 3 — non-blocking editor). */
    win_key_handler_t   key_handler;

    /* TASK UI: scroll-wheel handler.  Fired by wm_tick when the focused
     * window receives a non-zero accumulated Z delta from the mouse
     * IRQ.  NULL = window doesn't care about scroll. */
    win_scroll_handler_t scroll_handler;
} window_t;

void       wm_init          (void);
window_t  *wm_create_window (int x, int y, int w, int h, const char *title);
void       wm_destroy_window(window_t *w);
/* ISSUE 8: replace a window's title bar text after creation.  Apps that
 * register a language-change listener call this with the freshly
 * translated string so the chrome follows an EN<->HU switch without
 * being torn down and recreated. */
void       wm_set_title     (window_t *w, const char *title);
/* Set the taskbar pictogram (an icon_id_t from icons.h; 0 = letter tile). */
void       wm_set_icon      (window_t *w, int icon);
void       wm_minimize      (window_t *w);
void       wm_restore       (window_t *w);
/* Toggle a window between maximized (filling the desktop above the taskbar)
 * and its previous floating geometry.  Resizes the content framebuffer and
 * fires the resize_cb so the app re-lays-out.  No-op for protected windows
 * or windows without a resize_cb (nothing would repaint the new area). */
void       wm_toggle_maximize  (window_t *w);
bool       wm_can_maximize     (const window_t *w);
void       wm_set_focus     (window_t *w);
window_t  *wm_focused       (void);
/* Returns the focused window's id, or 0 when nothing is focused (which
 * happens transiently between destroy + refocus).  Drivers that route
 * input to a specific window should compare against this. */
int        wm_focused_id    (void);
/* Hand focus back to the shell (the first 'protected' window in slot 0).
 * Called when a child window - editor, taskmgr, script - is closed so
 * subsequent keystrokes have an obvious owner. */
void       wm_refocus_shell (void);

/* Tell the WM which window owns the shell.  Set once by the kernel right
 * after the console window is created.  Used by `wm_is_shell_focused`. */
void       wm_set_shell_window(window_t *w);

/* True iff the shell window is currently focused.  Callable from the
 * keyboard ISR to gate Ctrl+C / abort behaviour on focus - keystrokes
 * meant for the editor or another modal must NOT trigger a shell-side
 * interrupt. */
bool       wm_is_shell_focused(void);

/* Register a callback that the WM invokes when a left/right click lands
 * inside the window's content area.  Pass cb=NULL to unhook.  The user
 * pointer is forwarded verbatim - typically a module-local state ptr. */
void       wm_set_content_click(window_t *w,
                                win_content_click_t cb,
                                void *user);

/* Configure the resize behaviour of a window.  resizable=false disables
 * the grip handle entirely (used by dialogs).  min_w/min_h are clamped
 * during interactive resize so the user can never shrink a window into
 * something the chrome can't render. */
void       wm_set_resizable    (window_t *w, bool resizable,
                                int min_w, int min_h);
void       wm_set_resize_cb    (window_t *w, win_resize_cb_t cb);
/* ISSUE 1: register a destroy notifier so the app can drop its
 * cached window pointer when the WM tears the slot down (whether the
 * close came from the X button, the taskbar Bezárás, or another
 * external trigger). */
void       wm_set_destroy_cb   (window_t *w, win_destroy_cb_t cb);
/* ISSUE 3: install / clear the per-window key handler.  Pass cb=NULL
 * to opt out (the shell window never installs one — its read_line
 * loop owns the global keyboard buffer). */
void       wm_set_key_handler  (window_t *w, win_key_handler_t cb);
/* TASK UI: wire a scroll-wheel handler.  NULL detaches. */
void       wm_set_scroll_handler(window_t *w, win_scroll_handler_t cb);
/* ISSUE 4: mark every window dirty + force the next composite to
 * resize_cb each one so the i18n / theme switch repaints in a single
 * frame.  No-op when nothing is open. */
void       wm_force_global_repaint(void);
/* ISSUE 3: pump pending keys from the global keyboard ring to the
 * focused window's key_handler (if any).  Returns true if any key
 * was consumed by a window callback so read_line knows not to read
 * those bytes itself. */
bool       wm_dispatch_keys    (void);

/* ---------- Modal stack ------------------------------------------------ *
 * Models the fact that keyboard input is owned by ONE window at a time:
 * whichever subsystem (shell, editor, panic modal) is currently in its
 * own keyboard_wait_getc() loop.  The WM does NOT route IRQ1 bytes per-
 * window - keystrokes are pushed into a single global ring buffer and
 * consumed by whoever is blocked.  The modal stack lets the WM:
 *   - reject mouse-driven focus changes while a modal child is up
 *     (clicking on the shell does NOT steal input from a panic dialog);
 *   - tell diagnostics ("taskmgr") whether the shell is reachable.
 *
 * Calls nest: editor pushes itself, opens a child save-dialog that pushes
 * itself again, both pop on close.  Capacity is small on purpose - the
 * kernel has no use for >4 nested modals. */
void       wm_push_modal    (window_t *w);
void       wm_pop_modal     (window_t *w);
bool       wm_modal_active  (void);
window_t  *wm_top_modal     (void);

void       wm_mark_dirty    (void);

/* Region-based (dirty-rect) damage.  wm_damage_rect: a screen-space box that
 * changed but does NOT need a full recompose.  wm_shell_content_damage: the
 * same but in shell-terminal-content coords (the WM maps it onto the shell
 * window) — used by the high-frequency idle spinner so it no longer forces a
 * full-screen recompose every 80 ms. */
void       wm_damage_rect          (int x, int y, int w, int h);
void       wm_shell_content_damage (int cx, int cy, int cw, int ch);

/* Force an immediate full recompose (used once at boot before the shell loop
 * starts; normally the dirty-flag path in wm_tick() is sufficient). */
void       wm_present       (void);

/* Force the next present to be a FULL framebuffer blit, discarding the
 * damage-tracking shadow.  Call after any code path that wrote to the hardware
 * framebuffer directly (screensaver, a diagnostic that fills the LFB, etc.). */
void       wm_invalidate_shadow(void);

/* Periodic tick: poll mouse, run hit-test/drag, recompose if dirty.
 * Call from the shell idle loop (the sti;hlt path in read_line). */
void       wm_tick          (void);

/* Called by the VGA driver after a runtime resolution change.  Resizes the
 * back buffer, clamps all windows to the new bounds, and forces a repaint. */
void wm_handle_resolution_change(uint32_t new_w, uint32_t new_h);

/* Introspection for taskmgr -------------------------------------------- */
void wm_pool_stats (uint32_t *used_out, uint32_t *total_out);
int  wm_get_windows(window_t **buf, int max);

/* ---------- Off-screen back buffer (compositor RAM target) -------------- *
 * The WM composites every frame into a screen-sized RAM buffer (cached
 * write-back memory) and then blits it to the VESA framebuffer in a single
 * pass.  Pre-desktop full-screen UIs -- most importantly the login modal --
 * can borrow the same buffer so that their alpha-blended drawing (which
 * read-modify-writes the destination) runs against fast RAM instead of the
 * uncached hardware framebuffer.  On real hardware a blended pixel read from
 * the LFB is a slow uncached PCIe round-trip; doing thousands of them per
 * frame makes the login screen visibly crawl (the "renders in layers and
 * never finishes" symptom).  Render into wm_backbuffer(), then call
 * wm_blit_backbuffer() to present the finished frame in one write-only blit. */
draw_target_t *wm_backbuffer     (void);
void           wm_blit_backbuffer(void);

#endif /* NEXXON_WINDOW_H */
