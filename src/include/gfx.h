/* ============================================================================
 * NexxoN OS - Graphics target abstraction  (v4.0)
 * ----------------------------------------------------------------------------
 * Every pixel-pushing routine in the kernel now takes an explicit
 * draw_target_t pointer so we can render into ANY framebuffer:
 *
 *   * the main VESA linear framebuffer (the "screen")
 *   * a window's content framebuffer (owned by the WM)
 *   * an off-screen scratch buffer for compositing
 *
 * Targets are 32-bpp ARGB (alpha ignored).  Pitch is a separate field so
 * window framebuffers can be packed tighter than the main screen.
 * ============================================================================ */
#ifndef NEXXON_GFX_H
#define NEXXON_GFX_H

#include "types.h"

typedef struct {
    uint8_t *fb;          /* base pointer                     */
    uint32_t pitch;       /* bytes per row (>= width * 4)     */
    uint32_t width;       /* pixels per row                   */
    uint32_t height;      /* total rows                       */
    /* Optional scissor box for region-based (dirty-rect) compositing.  When
     * clip_on is false the whole target is writable (default; zero-init keeps
     * every existing draw_target_t full-surface).  gfx_set_clip() intersects
     * the requested box with the target bounds; every write primitive
     * (putpixel / blend_pixel / fill_rect / blend_rect / blit / clear / blur)
     * honours it, so the compositor can recompose+present only what changed. */
    bool     clip_on;
    int      clip_x, clip_y, clip_w, clip_h;
} draw_target_t;

/* Restrict subsequent draws on `t` to [x,y,w,h] (intersected with bounds). */
void gfx_set_clip     (draw_target_t *t, int x, int y, int w, int h);
void gfx_reset_clip   (draw_target_t *t);

/* The main VESA framebuffer wrapped as a target.  Filled in by vga_init(). */
draw_target_t *gfx_screen(void);

/* ---- Pixel-level primitives ------------------------------------------- */
void gfx_clear        (draw_target_t *t, uint32_t color);
void gfx_putpixel     (draw_target_t *t, int x, int y, uint32_t c);
void gfx_fill_rect    (draw_target_t *t, int x, int y, int w, int h, uint32_t c);
void gfx_draw_hline   (draw_target_t *t, int x, int y, int w, uint32_t c);
void gfx_draw_vline   (draw_target_t *t, int x, int y, int h, uint32_t c);
void gfx_draw_rect    (draw_target_t *t, int x, int y, int w, int h, uint32_t c);
void gfx_scroll_up    (draw_target_t *t, int pixels, uint32_t bg_fill);

/* Alpha-blend a single ARGB pixel against the existing target pixel.
 * `alpha` is taken from the high byte of `c` (0 = transparent, 255 =
 * opaque).  Used by the v6 drop-shadow + rounded-corner code paths. */
void gfx_blend_pixel  (draw_target_t *t, int x, int y, uint32_t c);
void gfx_blend_rect   (draw_target_t *t, int x, int y, int w, int h,
                       uint32_t argb);

/* Paint a soft drop shadow under a rectangular widget.  The shadow is a
 * smoothly-decaying alpha gradient that reads the existing target
 * pixels and blends a dark colour on top.  Used by the WM compositor
 * before drawing each window. */
void gfx_drop_shadow  (draw_target_t *t, int x, int y, int w, int h,
                       int radius);

/* Anti-aliased character renderer: 2x2 supersampled blend of the 8x8
 * bitmap glyph into the target.  Slower than gfx_draw_char but visibly
 * smoother.  Used by the WM title bars and Gephaz headers. */
void gfx_draw_char_aa (draw_target_t *t, int x, int y, char c,
                       uint32_t fg, uint32_t bg);
void gfx_draw_string_aa(draw_target_t *t, int x, int y, const char *s,
                        uint32_t fg, uint32_t bg);

/* 2x chrome text: the 8x8 glyph upscaled to a smooth 16x16 via EPX
 * (scale2x keeps diagonals connected) and edge-blended like the AA
 * renderer.  For surfaces where the 8 px face is too small to read
 * comfortably (window titles, taskbar, menus, dialog headers); glyph
 * advance is 16 px.  bg alpha 0 = transparent, like the AA renderer. */
void gfx_draw_char_aa2x  (draw_target_t *t, int x, int y, char c,
                          uint32_t fg, uint32_t bg);
void gfx_draw_string_aa2x(draw_target_t *t, int x, int y, const char *s,
                          uint32_t fg, uint32_t bg);
/* Clipped 2x variant: stops before max_w and appends ".." if truncated. */
int  gfx_draw_string_aa2x_clipped(draw_target_t *t, int x, int y, int max_w,
                                  const char *s, uint32_t fg, uint32_t bg);

/* Per-row horizontal inset following the circular corner arc of a rounded
 * rect (radius rad, total height h).  Use for gradient/gloss row fills so
 * they never paint over the AA corner pixels (square-corner artefacts). */
int gfx_round_row_inset(int row, int h, int rad);

/* Alpha-BLENDED rounded-rect outline with AA corner arcs (gfx_draw_round_rect
 * pokes raw pixels, so translucent outlines land as solid chunks). */
void gfx_draw_round_rect_blend(draw_target_t *t, int x, int y, int w, int h,
                               int radius, uint32_t color);

/* Aero widget: rounded, vertically-graded glossy button with a centred
 * AA label.  One implementation so Settings / dialogs / Explorer /
 * Browser buttons all share the same modern look.  `base` is the body
 * colour (full alpha); `hot` brightens it (hover). */
void gfx_draw_button_aero(draw_target_t *t, int x, int y, int w, int h,
                          const char *label, uint32_t base, bool hot);

/* Shared Aero tab — THE canonical tab look (matches the Settings tab strip) so
 * every tabbed surface is identical.  `active` = the selected tab (glossy
 * `accent` fill, top accent stripe, fused into the content below); otherwise a
 * quiet raised tab that lightens on `hot`.  Label is AA-centred + clipped. */
void gfx_draw_tab_aero(draw_target_t *t, int x, int y, int w, int h,
                       const char *label, uint32_t accent, bool active, bool hot);

/* Shared Aero slider track + thumb.  Draws a sunken glass track across
 * [x,y,w,h] (centred groove) with the filled portion in `accent`, and a glossy
 * round thumb at fraction `pos` (0..1000).  One look for every volume/scroll
 * slider in the system. */
void gfx_draw_slider_aero(draw_target_t *t, int x, int y, int w, int h,
                          int pos_permil, uint32_t accent);

/* ---- Font glyphs ------------------------------------------------------ */
void gfx_draw_char    (draw_target_t *t, int x, int y,
                       char c, uint32_t fg, uint32_t bg);
void gfx_draw_string  (draw_target_t *t, int x, int y,
                       const char *s, uint32_t fg, uint32_t bg);

/* ---- Bit-blit: copy `src` (whole) onto `dst` at (dx, dy) -------------- */
void gfx_blit         (draw_target_t *dst, int dx, int dy, draw_target_t *src);

/* ============================================================================
 * Liquid-Glass compositing primitives
 * ----------------------------------------------------------------------------
 * The frosted-glass look = blur whatever is already in the target under the
 * panel, blend a translucent fill on top, then a 1 px 3-D bevel + rounded
 * corners.  These run on the WM back buffer (cached RAM) so they are cheap;
 * the damage-tracked present only pushes the rows that actually changed.
 * ============================================================================ */

/* Fast separable box blur of a sub-rectangle, IN PLACE.  `radius` is the
 * half-window (>=1); the window is (2*radius+1) px.  Two O(w*h) running-sum
 * passes (horizontal then vertical), independent of radius.  Edge pixels
 * clamp (replicate).  Forces the alpha byte to 0xFF.  This is the backdrop
 * blur every glass surface sits on. */
void gfx_box_blur     (draw_target_t *t, int x, int y, int w, int h, int radius);

/* Alpha-blend `argb` (its high byte is the opacity) over a rounded
 * rectangle, antialiasing the corner arcs.  The translucent twin of
 * gfx_fill_round_rect — used for frosted fills that must let the blurred
 * backdrop show through. */
void gfx_blend_round_rect(draw_target_t *t, int x, int y, int w, int h,
                          int radius, uint32_t argb);

/* 1 px 3-D glass bevel around a (rounded) rect: `light` on the top + left
 * edges (the gloss highlight), `dark` on the bottom + right (the depth
 * shadow), with the corner arcs in `light`.  No fill — overlay only. */
void gfx_glass_bevel  (draw_target_t *t, int x, int y, int w, int h,
                       int radius, uint32_t light, uint32_t dark);

/* The whole frosted-glass panel in one call: blur the backdrop, blend
 * `fill` over a rounded rect, lay a top gloss sweep, then the 3-D bevel.
 * `radius` rounds the corners (0 = square, e.g. the full-width taskbar). */
void gfx_glass_panel  (draw_target_t *t, int x, int y, int w, int h,
                       int radius, uint32_t fill);

/* ---- UTF-8 aware text measurement ------------------------------------- *
 * Counts the number of visual code-points (glyphs) in a UTF-8 string and
 * returns the pixel width they will occupy when rendered through the 8x8
 * bitmap font path.  Use these instead of `strlen() * FONT_GLYPH_W` for
 * any string that might contain Hungarian or other Latin-1 accented
 * characters, otherwise layout offsets are off by 1-2 px per accent. */
int  gfx_string_glyph_count(const char *s);
int  gfx_string_pixel_width(const char *s);

/* ISSUE 2 (precise layout bounds): render `s` at (x, y) but clip the
 * output to `max_w` pixels and append "..." when the source string
 * would have overrun the budget.  Returns the actual pixel width
 * drawn so callers can position adjacent decorations.  Both bitmap
 * and anti-aliased variants behave identically except for the
 * rasterisation pass. */
int  gfx_draw_string_clipped   (draw_target_t *t, int x, int y, int max_w,
                                const char *s, uint32_t fg, uint32_t bg);
int  gfx_draw_string_aa_clipped(draw_target_t *t, int x, int y, int max_w,
                                const char *s, uint32_t fg, uint32_t bg);

/* ============================================================================
 * TASK 5: Advanced Graphics Engine v2.0
 * ----------------------------------------------------------------------------
 * Rounded rectangles, soft drop shadows that respect those corners, an
 * outline-style "TTF-look" rasteriser with 2x supersampled coverage, and
 * spline / curve renderers for the Task Manager performance graphs.
 * ============================================================================ */

/* Filled rectangle with rounded corners.  `radius` clamped to min(w,h)/2
 * automatically.  Antialiased edges (3-level coverage).  When `radius`
 * is 0 falls through to gfx_fill_rect.                                  */
void gfx_fill_round_rect    (draw_target_t *t, int x, int y, int w, int h,
                             int radius, uint32_t color);

/* Outline of a rounded rectangle.  Single-pixel stroke. */
void gfx_draw_round_rect    (draw_target_t *t, int x, int y, int w, int h,
                             int radius, uint32_t color);

/* Drop shadow under a rounded widget.  Smoothly fades from inner
 * opacity to 0 over `blur` pixels and clips against the rounded
 * corners so the shadow's silhouette matches the widget exactly. */
void gfx_drop_shadow_round  (draw_target_t *t, int x, int y, int w, int h,
                             int radius, int blur);

/* Polyline with anti-aliasing.  Each segment is rasterised with
 * Wu's algorithm.  Used by the Task Manager CPU / RAM / Network
 * history graphs. */
void gfx_polyline_aa        (draw_target_t *t, const int *xs, const int *ys,
                             int count, uint32_t color);

/* Cardinal spline through the supplied control points.  `tension`
 * 0..255 maps to 0.0..1.0 - higher values produce sharper curves.
 * Internally tessellates each segment into `segments` straight lines
 * then draws them with the polyline routine. */
void gfx_spline_cardinal_aa (draw_target_t *t, const int *xs, const int *ys,
                             int count, int segments, int tension,
                             uint32_t color);

/* Filled circle / ring.  Centre (cx, cy), radius `r`.  Antialiased
 * via 4x4 coverage of the boundary band. */
void gfx_fill_circle        (draw_target_t *t, int cx, int cy, int r,
                             uint32_t color);

/* ---- Outline-style "TTF" rasteriser ---------------------------------- *
 * Renders the bitmap glyph at a configurable scale factor while
 * computing per-pixel coverage from a 2x2 supersampled mask of the
 * underlying outline.  At scale 1 it behaves like gfx_draw_char_aa;
 * at scale 2/3/4 it produces clean stair-step-free large glyphs
 * suitable for window titles and Gephaz section headers.            */
void gfx_draw_char_ttf      (draw_target_t *t, int x, int y, char c,
                             int scale, uint32_t fg, uint32_t bg);
void gfx_draw_string_ttf    (draw_target_t *t, int x, int y, const char *s,
                             int scale, uint32_t fg, uint32_t bg);
int  gfx_ttf_advance        (int scale);    /* per-char advance in pixels */

/* Italic (oblique) text: glyphs are sheared right toward the top so the
 * text slants.  Foreground-only (transparent background — the caller is
 * expected to have already painted the cell/area).  Stops once the next
 * glyph would exceed max_w pixels (max_w<=0 = no clip). */
void gfx_draw_string_italic_clipped(draw_target_t *t, int x, int y, int max_w,
                                    const char *s, uint32_t fg);

#endif /* NEXXON_GFX_H */
