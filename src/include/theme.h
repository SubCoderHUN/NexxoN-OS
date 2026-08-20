/* ============================================================================
 * NexxoN OS - Theme engine (TASK 31)
 * ----------------------------------------------------------------------------
 * Lightweight global palette toggle.  Apps that want to respect the
 * Light/Dark setting query theme_color(KEY) instead of hardcoding ARGB
 * literals.  The compositor (panel + start menu) is the canonical
 * consumer; per-app tabs migrate over incrementally.
 *
 * Switching modes flips an integer and marks the WM dirty — no IPC
 * broadcast yet, but every redraw call after the switch reads the new
 * palette so the next compose pass paints the new look.
 * ============================================================================ */
#ifndef NEXXON_THEME_H
#define NEXXON_THEME_H

#include "types.h"

typedef enum {
    THEME_PANEL_TOP = 0,
    THEME_PANEL_BOT,
    THEME_PANEL_FG,
    THEME_MENU_BG,
    THEME_MENU_FG,
    THEME_MENU_HDR_TOP,
    THEME_MENU_HDR_BOT,
    THEME_WINDOW_BG,
    THEME_WINDOW_FG,
    THEME_ACCENT,
    THEME_KEY_COUNT
} theme_key_t;

void     theme_init     (void);
void     theme_set_dark (bool dark);
bool     theme_is_dark  (void);
uint32_t theme_color    (theme_key_t key);

/* ============================================================================
 * Liquid-Glass design tokens  (the "Modernised Aero / Liquid Glass" look)
 * ----------------------------------------------------------------------------
 * One central place for the frosted-glass palette, the 8 px layout grid and
 * the animation timing so the values are NOT scattered as hex literals across
 * the compositor, desktop, tray, chrome, popup and app code.  Every glass
 * surface is composited the same way: blur the backdrop, blend one of the
 * GLASS_* translucent fills on top, then a 1 px 3-D bevel (light top/left,
 * dark bottom/right) and rounded corners.
 * ============================================================================ */

/* ---- Frosted panel fills (ARGB; alpha = frost strength over a blurred
 * backdrop).  Tuned so 25-40 % reads as glass once the backdrop is blurred. */
#define GLASS_FROST          0x48FFFFFFu   /* generic frosted white  ~28 %   */
#define GLASS_TASKBAR        0xB41A2B40u   /* taskbar: cool blue glass ~70 % */
#define GLASS_MENU_LEFT      0x8AF4F8FFu   /* start menu left pane   ~54 %   */
#define GLASS_MENU_RIGHT     0x9614283Fu   /* start menu right pane  bluish  */
#define GLASS_POPUP          0xC6EEF3FAu   /* ctx menu / dialog body         */
#define GLASS_POPUP_DARK     0xB4101A28u   /* dark popup (tooltip / hud)     */
#define GLASS_TITLE_FOCUS    0x9CDCEBFAu   /* focused window title strip     */
#define GLASS_TITLE_UNFOCUS  0x8CC4CCD6u   /* unfocused window title strip   */

/* ---- 3-D glass bevel: light top/left highlight, dark bottom/right shade -- */
#define GLASS_EDGE_LIGHT     0xCCFFFFFFu   /* #FFFFFF ~80 % (gloss highlight) */
#define GLASS_EDGE_DARK      0x42000000u   /* #000000 ~26 % (depth shadow)    */
#define GLASS_INNER_LIGHT    0x80FFFFFFu   /* inner content seam highlight    */
#define GLASS_GLOSS          0x55FFFFFFu   /* top "shiny" sweep on a panel    */

/* ---- Selection / hover -------------------------------------------------- */
#define GLASS_HOVER          0x26FFFFFFu   /* #FFFFFF 15 % hover wash         */
#define GLASS_HOVER_DARK     0x33FFFFFFu   /* hover on a dark glass pane      */
#define GLASS_SELECT         0x551E6FE0u   /* selected-row accent wash        */

/* ---- Text on glass ------------------------------------------------------ */
#define GLASS_TEXT_DARK      0xFF1A1A1Au   /* on light frosted surfaces       */
#define GLASS_TEXT_MUTED     0xFF566070u   /* secondary text on light glass   */
#define GLASS_TEXT_LIGHT     0xFFF2F6FBu   /* on dark glass / terminal        */
#define GLASS_ACCENT         0xFF1E6FE0u   /* primary blue accent             */
#define GLASS_ACCENT_DEEP    0xFF0E3FA0u

/* ---- 8 px layout grid (strict) ----------------------------------------- */
#define GRID_UNIT            8
#define PAD_V                8             /* vertical content padding        */
#define PAD_H                16            /* horizontal content padding      */
#define RADIUS_PANEL         8             /* window / panel corner radius    */
#define RADIUS_HOVER         4             /* hover / small-control radius    */
#define GLASS_BLUR_R         6             /* backdrop blur half-window       */
#define TASKBAR_H            40            /* taskbar height (fixed, spec §4) */

/* ---- Animation (ease-out cubic, PIT/RTC-driven, frame independent) ------ */
#define ANIM_FAST_MS         110u          /* hover / focus                   */
#define ANIM_MENU_MS         170u          /* start menu / context menu       */
#define ANIM_WIN_MS          150u          /* window open / close / min       */

/* Ease-out cubic: maps elapsed/duration to a 0..256 fixed-point progress
 * (256 == fully arrived).  f(p) = 1-(1-p)^3.  Used by every "liquid" motion
 * so openings/closings decelerate smoothly instead of moving linearly. */
uint32_t anim_ease_out_cubic(uint32_t elapsed_ms, uint32_t duration_ms);

#endif /* NEXXON_THEME_H */
