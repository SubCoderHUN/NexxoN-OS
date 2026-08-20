/* ============================================================================
 * NexxoN OS - Graphics target primitives
 * ----------------------------------------------------------------------------
 * Plain unaccelerated 32-bpp drawing.  Hot paths (rect fill, scroll, blit)
 * use 32-bit pointer arithmetic so the compiler emits movsd / stosd loops.
 * ============================================================================ */
#include "gfx.h"
#include "font.h"
#include "string.h"
#include "theme.h"   /* GLASS_* tokens for gfx_glass_panel */

static draw_target_t g_screen_target;

draw_target_t *gfx_screen(void) { return &g_screen_target; }

/* Inline pixel helper - never NULL-checks because callers already
 * guarantee a valid target.  Bounds-checks the (x, y) pair. */
static inline uint32_t *pixel_ptr(draw_target_t *t, int x, int y) {
    return (uint32_t *)(t->fb + (uint32_t)y * t->pitch + (uint32_t)x * 4);
}

/* ---- Scissor clip (region-based compositing) -------------------------- */
void gfx_set_clip(draw_target_t *t, int x, int y, int w, int h) {
    if (!t) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if ((uint32_t)(x + w) > t->width)  w = (int)t->width  - x;
    if ((uint32_t)(y + h) > t->height) h = (int)t->height - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    t->clip_on = true;
    t->clip_x = x; t->clip_y = y; t->clip_w = w; t->clip_h = h;
}
void gfx_reset_clip(draw_target_t *t) { if (t) t->clip_on = false; }

/* True when (x,y) is outside the active scissor box. */
static inline bool clip_rejects(const draw_target_t *t, int x, int y) {
    return t->clip_on && (x < t->clip_x || x >= t->clip_x + t->clip_w ||
                          y < t->clip_y || y >= t->clip_y + t->clip_h);
}
/* Intersect [x,y,w,h] (in/out) with the active scissor box.  Returns false
 * when the result is empty. */
static inline bool clip_box(const draw_target_t *t, int *x, int *y,
                            int *w, int *h) {
    if (!t->clip_on) return *w > 0 && *h > 0;
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < t->clip_x)               x0 = t->clip_x;
    if (y0 < t->clip_y)               y0 = t->clip_y;
    if (x1 > t->clip_x + t->clip_w)   x1 = t->clip_x + t->clip_w;
    if (y1 > t->clip_y + t->clip_h)   y1 = t->clip_y + t->clip_h;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return *w > 0 && *h > 0;
}

void gfx_putpixel(draw_target_t *t, int x, int y, uint32_t c) {
    if (!t || !t->fb) return;
    if (x < 0 || y < 0 || (uint32_t)x >= t->width || (uint32_t)y >= t->height) return;
    if (clip_rejects(t, x, y)) return;
    *pixel_ptr(t, x, y) = c;
}

void gfx_clear(draw_target_t *t, uint32_t color) {
    if (!t || !t->fb) return;
    if (t->clip_on) { gfx_fill_rect(t, 0, 0, (int)t->width, (int)t->height, color); return; }
    for (uint32_t y = 0; y < t->height; y++) {
        uint32_t *p = (uint32_t *)(t->fb + y * t->pitch);
        for (uint32_t x = 0; x < t->width; x++) p[x] = color;
    }
}

void gfx_fill_rect(draw_target_t *t, int x, int y, int w, int h, uint32_t c) {
    if (!t || !t->fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((uint32_t)(x + w) > t->width)  w = (int)t->width  - x;
    if ((uint32_t)(y + h) > t->height) h = (int)t->height - y;
    if (!clip_box(t, &x, &y, &w, &h)) return;
    for (int row = 0; row < h; row++) {
        uint32_t *p = pixel_ptr(t, x, y + row);
        for (int col = 0; col < w; col++) p[col] = c;
    }
}

void gfx_draw_hline(draw_target_t *t, int x, int y, int w, uint32_t c) {
    gfx_fill_rect(t, x, y, w, 1, c);
}
void gfx_draw_vline(draw_target_t *t, int x, int y, int h, uint32_t c) {
    gfx_fill_rect(t, x, y, 1, h, c);
}
void gfx_draw_rect(draw_target_t *t, int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    gfx_draw_hline(t, x,         y,         w, c);
    gfx_draw_hline(t, x,         y + h - 1, w, c);
    gfx_draw_vline(t, x,         y,         h, c);
    gfx_draw_vline(t, x + w - 1, y,         h, c);
}

/* ---------- Anti-aliased glyph coverage ---------------------------------- *
 * The bitmap font is 1-bit 8x8, so diagonals and curves come out as hard
 * staircases — "the whole system looks pixelated".  Rather than ship a bigger
 * font (which would reflow every layout) we derive a grayscale COVERAGE map
 * from the existing glyphs once, at first use: solid pixels stay fully opaque
 * (stems remain crisp) and the concave notches of a staircase get partial
 * coverage so the edge reads as a smooth slope.  Built in RAM from the kernel's
 * embedded font, so it is identical on a LIVE CD and an installed disk.  Every
 * piece of text in the OS goes through gfx_draw_char(), so this smooths the
 * whole UI at once with no layout change. */
#define GFX_AA_NOTCH   140u                 /* coverage of a smoothed notch */
static uint8_t g_font_aa[256][FONT_GLYPH_W * FONT_GLYPH_H];
static bool    g_font_aa_ready = false;

static inline int aa_on(const uint8_t *g, int r, int c) {
    if (r < 0 || r >= FONT_GLYPH_H || c < 0 || c >= FONT_GLYPH_W) return 0;
    return (g[r] >> c) & 1;
}

static void font_aa_build(void) {
    for (int ch = 0; ch < 256; ch++) {
        const uint8_t *g = font8x8[ch];
        for (int r = 0; r < FONT_GLYPH_H; r++) {
            for (int c = 0; c < FONT_GLYPH_W; c++) {
                uint8_t v = 0;
                if (aa_on(g, r, c)) {
                    v = 255;
                } else {
                    int u = aa_on(g, r - 1, c), d = aa_on(g, r + 1, c);
                    int l = aa_on(g, r, c - 1), rr = aa_on(g, r, c + 1);
                    /* A concave corner = two ON neighbours that are
                     * perpendicular (an L).  Opposite ON neighbours (a 1px
                     * gap) are left alone so thin features don't smear. */
                    int corner = (u && l) || (u && rr) || (d && l) || (d && rr);
                    int opp    = (u && d) || (l && rr);
                    if (corner && !opp) v = GFX_AA_NOTCH;
                }
                g_font_aa[ch][r * FONT_GLYPH_W + c] = v;
            }
        }
    }
    g_font_aa_ready = true;
}

/* Blend fg over `behind` by 8-bit coverage (0=behind, 255=fg). */
static inline uint32_t gfx_blend(uint32_t fg, uint32_t behind, uint32_t cov) {
    uint32_t ia = 255u - cov;
    uint32_t r = (((fg >> 16) & 0xFF) * cov + ((behind >> 16) & 0xFF) * ia) / 255u;
    uint32_t g = (((fg >>  8) & 0xFF) * cov + ((behind >>  8) & 0xFF) * ia) / 255u;
    uint32_t b = (((fg      ) & 0xFF) * cov + ((behind      ) & 0xFF) * ia) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline uint32_t gfx_read_px(draw_target_t *t, int x, int y) {
    if (x < 0 || y < 0 || (uint32_t)x >= t->width || (uint32_t)y >= t->height) return 0;
    return *pixel_ptr(t, x, y);
}

void gfx_draw_char(draw_target_t *t, int x, int y,
                   char c, uint32_t fg, uint32_t bg) {
    if (!t || !t->fb) return;
    if (!g_font_aa_ready) font_aa_build();
    const uint8_t *cov = g_font_aa[(uint8_t)c];
    /* bg alpha = 0 -> transparent: the glyph sits directly on whatever is
     * already on the target (no solid box behind taskbar/title labels). */
    bool bg_transparent = ((bg >> 24) & 0xFFu) == 0u;
    for (int row = 0; row < FONT_GLYPH_H; row++) {
        for (int col = 0; col < FONT_GLYPH_W; col++) {
            uint8_t a = cov[row * FONT_GLYPH_W + col];
            if (a == 255u) {
                gfx_putpixel(t, x + col, y + row, fg);
            } else if (a == 0u) {
                if (!bg_transparent) gfx_putpixel(t, x + col, y + row, bg);
            } else {
                uint32_t behind = bg_transparent ? gfx_read_px(t, x + col, y + row)
                                                 : bg;
                gfx_putpixel(t, x + col, y + row, gfx_blend(fg, behind, a));
            }
        }
    }
}

/* ----------------------------------------------------------------------
 * UTF-8 -> single-byte glyph index mapper.  Source files compile as
 * UTF-8 by default so Hungarian characters like "ő" enter the binary
 * as multi-byte sequences (e.g. 0xC5 0x91).  The bitmap font.c table
 * stores Hungarian glyphs at their ISO-8859-2 single-byte indices
 * (0xE1..0xFC).  This helper decodes a single UTF-8 code point and
 * returns the matching glyph table index so menus, dialogs, taskbar
 * labels, login strings, etc. all render Hungarian accented letters
 * without source-level transliteration. */
static uint32_t utf8_next_cp(const char **pp) {
    const unsigned char *p = (const unsigned char *)(*pp);
    unsigned char b0 = *p;
    if (b0 < 0x80) { *pp = (const char *)(p + 1); return b0; }
    /* 2-byte: 110xxxxx 10xxxxxx */
    if ((b0 & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (p[1] & 0x3F);
        *pp = (const char *)(p + 2);
        return cp;
    }
    /* 3-byte: 1110xxxx 10xxxxxx 10xxxxxx */
    if ((b0 & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) |
                      ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *pp = (const char *)(p + 3);
        return cp;
    }
    /* Malformed / unsupported - treat the byte as ISO-8859-2. */
    *pp = (const char *)(p + 1);
    return b0;
}

static uint8_t glyph_index_for(uint32_t cp) {
    if (cp < 0x80) return (uint8_t)cp;
    /* Map common Latin-1 / Latin Extended-A code points used by HU. */
    switch (cp) {
        /* Uppercase */
        case 0x00C1: return 0xC1;  /* Á */
        case 0x00C9: return 0xC9;  /* É */
        case 0x00CD: return 0xCD;  /* Í */
        case 0x00D3: return 0xD3;  /* Ó */
        case 0x0150: return 0xD5;  /* Ő (double acute) */
        case 0x00D5: return 0xD5;  /* Õ - reuse Ő slot */
        case 0x00D6: return 0xD6;  /* Ö */
        case 0x00DA: return 0xDA;  /* Ú */
        case 0x0170: return 0xDB;  /* Ű (double acute) */
        case 0x00DB: return 0xDB;  /* Û - reuse Ű slot */
        case 0x00DC: return 0xDC;  /* Ü */
        /* Lowercase */
        case 0x00E1: return 0xE1;  /* á */
        case 0x00E9: return 0xE9;  /* é */
        case 0x00ED: return 0xED;  /* í */
        case 0x00F3: return 0xF3;  /* ó */
        case 0x0151: return 0xF5;  /* ő */
        case 0x00F5: return 0xF5;  /* õ */
        case 0x00F6: return 0xF6;  /* ö */
        case 0x00FA: return 0xFA;  /* ú */
        case 0x0171: return 0xFB;  /* ű */
        case 0x00FB: return 0xFB;  /* û */
        case 0x00FC: return 0xFC;  /* ü */
        default:     return (uint8_t)(cp <= 0xFF ? cp : '?');
    }
}

/* Count the number of code points (visual columns) in a UTF-8 string.
 * Public helper so layout code can compute pixel width as
 *      n * FONT_GLYPH_W
 * instead of strlen() * FONT_GLYPH_W which over-counts multi-byte
 * Hungarian characters by 1-2 columns each. */
int gfx_string_glyph_count(const char *s) {
    if (!s) return 0;
    int n = 0;
    while (*s) {
        utf8_next_cp(&s);
        n++;
    }
    return n;
}

int gfx_string_pixel_width(const char *s) {
    return gfx_string_glyph_count(s) * FONT_GLYPH_W;
}

void gfx_draw_string(draw_target_t *t, int x, int y,
                     const char *s, uint32_t fg, uint32_t bg) {
    while (*s) {
        uint32_t cp  = utf8_next_cp(&s);
        uint8_t  ix  = glyph_index_for(cp);
        gfx_draw_char(t, x, y, (char)ix, fg, bg);
        x += FONT_GLYPH_W;
    }
}

void gfx_draw_string_italic_clipped(draw_target_t *t, int x, int y, int max_w,
                                    const char *s, uint32_t fg) {
    if (!t || !s) return;
    int x0 = x;
    while (*s) {
        if (max_w > 0 && (x - x0) > max_w - FONT_GLYPH_W) break;
        uint32_t cp = utf8_next_cp(&s);
        const uint8_t *gly = font8x8[glyph_index_for(cp)];
        for (int row = 0; row < FONT_GLYPH_H; row++) {
            /* Shear: upper rows lean further right -> a right-slanting
             * oblique.  0..3 px over the 8 px glyph height. */
            int shear = (FONT_GLYPH_H - 1 - row) / 2;
            uint8_t bits = gly[row];
            for (int col = 0; col < FONT_GLYPH_W; col++) {
                if (bits & (1u << col))
                    gfx_putpixel(t, x + col + shear, y + row, fg);
            }
        }
        x += FONT_GLYPH_W;
    }
}

void gfx_scroll_up(draw_target_t *t, int pixels, uint32_t bg_fill) {
    if (!t || !t->fb || pixels <= 0) return;
    if ((uint32_t)pixels >= t->height) { gfx_clear(t, bg_fill); return; }
    for (uint32_t y = 0; y + (uint32_t)pixels < t->height; y++) {
        uint8_t *dst = t->fb + y * t->pitch;
        uint8_t *src = t->fb + (y + (uint32_t)pixels) * t->pitch;
        memcpy(dst, src, t->width * 4);
    }
    for (uint32_t y = t->height - (uint32_t)pixels; y < t->height; y++) {
        uint32_t *p = (uint32_t *)(t->fb + y * t->pitch);
        for (uint32_t x = 0; x < t->width; x++) p[x] = bg_fill;
    }
}

void gfx_blit(draw_target_t *dst, int dx, int dy, draw_target_t *src) {
    if (!dst || !src || !dst->fb || !src->fb) return;
    /* Clip the source rectangle into the destination viewport. */
    int sx0 = 0, sy0 = 0;
    int sw = (int)src->width;
    int sh = (int)src->height;
    if (dx < 0) { sx0 -= dx; sw += dx; dx = 0; }
    if (dy < 0) { sy0 -= dy; sh += dy; dy = 0; }
    if (sw <= 0 || sh <= 0) return;
    if ((uint32_t)(dx + sw) > dst->width)  sw = (int)dst->width  - dx;
    if ((uint32_t)(dy + sh) > dst->height) sh = (int)dst->height - dy;
    if (sw <= 0 || sh <= 0) return;

    /* Region-based compositing: clamp the destination to the scissor box and
     * shift the source origin by the same delta so the copy stays aligned. */
    if (dst->clip_on) {
        int cx0 = dst->clip_x, cy0 = dst->clip_y;
        int cx1 = dst->clip_x + dst->clip_w, cy1 = dst->clip_y + dst->clip_h;
        if (dx < cx0) { int d = cx0 - dx; sx0 += d; sw -= d; dx = cx0; }
        if (dy < cy0) { int d = cy0 - dy; sy0 += d; sh -= d; dy = cy0; }
        if (dx + sw > cx1) sw = cx1 - dx;
        if (dy + sh > cy1) sh = cy1 - dy;
        if (sw <= 0 || sh <= 0) return;
    }

    for (int row = 0; row < sh; row++) {
        uint8_t *d = dst->fb + (uint32_t)(dy + row) * dst->pitch + (uint32_t)dx * 4;
        uint8_t *s = src->fb + (uint32_t)(sy0 + row) * src->pitch + (uint32_t)sx0 * 4;
        memcpy(d, s, (size_t)sw * 4);
    }
}

/* ---- Alpha blending (TASK 24) ----------------------------------------- */
static inline uint8_t blend_ch(uint8_t dst, uint8_t src, uint8_t a) {
    /* Standard "source over" blend: out = src*a + dst*(255-a). */
    uint32_t out = (uint32_t)src * a + (uint32_t)dst * (uint32_t)(255 - a);
    return (uint8_t)((out + 127) / 255);
}

void gfx_blend_pixel(draw_target_t *t, int x, int y, uint32_t c) {
    if (!t || !t->fb) return;
    if (x < 0 || y < 0 || (uint32_t)x >= t->width || (uint32_t)y >= t->height) return;
    if (clip_rejects(t, x, y)) return;
    uint8_t a = (uint8_t)((c >> 24) & 0xFF);
    if (a == 0) return;
    uint32_t *p = (uint32_t *)(t->fb + (uint32_t)y * t->pitch + (uint32_t)x * 4);
    uint32_t d = *p;
    uint8_t sr = (uint8_t)((c >> 16) & 0xFF);
    uint8_t sg = (uint8_t)((c >>  8) & 0xFF);
    uint8_t sb = (uint8_t)(c & 0xFF);
    uint8_t dr = (uint8_t)((d >> 16) & 0xFF);
    uint8_t dg = (uint8_t)((d >>  8) & 0xFF);
    uint8_t db = (uint8_t)(d & 0xFF);
    uint8_t r = blend_ch(dr, sr, a);
    uint8_t g = blend_ch(dg, sg, a);
    uint8_t b = blend_ch(db, sb, a);
    *p = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void gfx_blend_rect(draw_target_t *t, int x, int y, int w, int h, uint32_t c) {
    if (!t || !t->fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((uint32_t)(x + w) > t->width)  w = (int)t->width  - x;
    if ((uint32_t)(y + h) > t->height) h = (int)t->height - y;
    if (!clip_box(t, &x, &y, &w, &h)) return;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            gfx_blend_pixel(t, x + col, y + row, c);
        }
    }
}

/* Drop shadow: a soft, diffuse outset glow.  Implemented as concentric
 * rings of decreasing alpha so the cost stays O(radius * perimeter) rather
 * than O(radius * area).  Tuned to match the gentle popup softening
 * (gfx_drop_shadow_round): a low peak alpha (~26) that fades smoothly to ~2
 * over the band, biased a couple of pixels downward so the window reads as
 * gently lifted off the desktop instead of wearing a hard black outline. */
void gfx_drop_shadow(draw_target_t *t, int x, int y, int w, int h, int radius) {
    if (!t || !t->fb || w <= 0 || h <= 0 || radius <= 0) return;
    const int oy = 2;                       /* downward bias */
    for (int r = 1; r <= radius; r++) {
        /* Quadratic falloff (1-(r/R))^2 scaled to a 26 peak -> softer than a
         * straight linear ramp, so the band has no hard inner edge. */
        int t_n = radius - r;               /* radius..0 */
        int a = (26 * t_n * t_n) / (radius * radius);
        if (a < 2) a = 2;
        uint32_t c = ((uint32_t)a << 24);   /* black with alpha */

        /* Top edge */
        for (int dx = -r; dx < w + r; dx++)
            gfx_blend_pixel(t, x + dx, y - r + oy, c);
        /* Bottom edge */
        for (int dx = -r; dx < w + r; dx++)
            gfx_blend_pixel(t, x + dx, y + h + r - 1 + oy, c);
        /* Left + right edges */
        for (int dy = -r; dy < h + r; dy++) {
            gfx_blend_pixel(t, x - r,         y + dy + oy, c);
            gfx_blend_pixel(t, x + w + r - 1, y + dy + oy, c);
        }
    }
}

/* ---- Anti-aliased font (TASK 24) ------------------------------------- */
/* Render the 8x8 glyph at native size but blend half-on/half-off
 * neighbouring pixels with a grey value, smoothing diagonal stair-
 * stepping.  Not real TTF - real TTF requires an outline-extraction +
 * Bezier rasteriser that doesn't fit in this milestone - but the
 * visual result is noticeably less jaggy at WM title-bar / Gephaz
 * header sizes. */
void gfx_draw_char_aa(draw_target_t *t, int x, int y,
                      char c, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font8x8[(uint8_t)c];
    /* bg alpha = 0 -> transparent: leave the destination pixel untouched
     * for OFF cells, and skip the bg pre-fill in the AA edge band so the
     * glyph blends straight onto whatever is already painted (glass title
     * bar, login panel, taskbar gradient, etc.). */
    bool bg_transparent = ((bg >> 24) & 0xFFu) == 0u;
    for (int row = 0; row < FONT_GLYPH_H; row++) {
        uint8_t bits = glyph[row];
        uint8_t prev = (row > 0) ? glyph[row - 1] : 0;
        uint8_t next = (row + 1 < FONT_GLYPH_H) ? glyph[row + 1] : 0;
        for (int col = 0; col < FONT_GLYPH_W; col++) {
            bool on = (bits >> col) & 1;
            if (on) {
                gfx_putpixel(t, x + col, y + row, fg);
                continue;
            }
            /* Off pixel: count on neighbours.  More = heavier blend. */
            int n = 0;
            if (col > 0          && ((bits >> (col - 1)) & 1)) n++;
            if (col + 1 < 8      && ((bits >> (col + 1)) & 1)) n++;
            if ((prev >> col) & 1) n++;
            if ((next >> col) & 1) n++;
            if (n == 0) {
                if (!bg_transparent) gfx_putpixel(t, x + col, y + row, bg);
                continue;
            }
            uint8_t a = (uint8_t)(40 + n * 30);   /* 70..160                */
            uint32_t mix = ((uint32_t)a << 24) | (fg & 0x00FFFFFFu);
            if (!bg_transparent) gfx_putpixel(t, x + col, y + row, bg);
            gfx_blend_pixel(t, x + col, y + row, mix);
        }
    }
}

void gfx_draw_string_aa(draw_target_t *t, int x, int y, const char *s,
                        uint32_t fg, uint32_t bg) {
    while (*s) {
        uint32_t cp = utf8_next_cp(&s);
        uint8_t  ix = glyph_index_for(cp);
        gfx_draw_char_aa(t, x, y, (char)ix, fg, bg);
        x += FONT_GLYPH_W;
    }
}

/* Shared implementation between bitmap + AA clipped renderers.  Picks
 * the renderer via the `aa` flag so we don't duplicate the geometry.
 * UTF-8 aware: counts visual code-points, not raw bytes, so Hungarian
 * accented characters do not artificially inflate the "doesn't fit"
 * trigger. */
static int draw_string_clipped_impl(draw_target_t *t, int x, int y, int max_w,
                                    const char *s, uint32_t fg, uint32_t bg,
                                    bool aa) {
    if (!s || !*s || max_w <= 0) return 0;
    int len = gfx_string_glyph_count(s);
    int pixels_needed = len * FONT_GLYPH_W;
    if (pixels_needed <= max_w) {
        /* Fits — emit verbatim. */
        if (aa) gfx_draw_string_aa(t, x, y, s, fg, bg);
        else    gfx_draw_string   (t, x, y, s, fg, bg);
        return pixels_needed;
    }
    /* Doesn't fit — reserve 3 glyphs (24 px) for the ellipsis and
     * print as many leading chars as remain. */
    int ellipsis_w = 3 * FONT_GLYPH_W;
    int payload_w  = max_w - ellipsis_w;
    if (payload_w < FONT_GLYPH_W) {
        int dots = max_w / FONT_GLYPH_W;
        if (dots > 3) dots = 3;
        for (int i = 0; i < dots; i++) {
            if (aa) gfx_draw_char_aa(t, x + i * FONT_GLYPH_W, y, '.', fg, bg);
            else    gfx_draw_char   (t, x + i * FONT_GLYPH_W, y, '.', fg, bg);
        }
        return dots * FONT_GLYPH_W;
    }
    int payload_chars = payload_w / FONT_GLYPH_W;
    int cx = x;
    const char *p = s;
    for (int i = 0; i < payload_chars && *p; i++) {
        uint32_t cp = utf8_next_cp(&p);
        uint8_t  ix = glyph_index_for(cp);
        if (aa) gfx_draw_char_aa(t, cx, y, (char)ix, fg, bg);
        else    gfx_draw_char   (t, cx, y, (char)ix, fg, bg);
        cx += FONT_GLYPH_W;
    }
    for (int i = 0; i < 3; i++) {
        if (aa) gfx_draw_char_aa(t, cx, y, '.', fg, bg);
        else    gfx_draw_char   (t, cx, y, '.', fg, bg);
        cx += FONT_GLYPH_W;
    }
    return cx - x;
}

int gfx_draw_string_clipped(draw_target_t *t, int x, int y, int max_w,
                            const char *s, uint32_t fg, uint32_t bg) {
    return draw_string_clipped_impl(t, x, y, max_w, s, fg, bg, false);
}

int gfx_draw_string_aa_clipped(draw_target_t *t, int x, int y, int max_w,
                               const char *s, uint32_t fg, uint32_t bg) {
    return draw_string_clipped_impl(t, x, y, max_w, s, fg, bg, true);
}

/* ---- 2x chrome text (EPX-upscaled, edge-blended) ---------------------- *
 * The 8x8 face is sharp but plain too small for 30 px title bars and the
 * taskbar.  Each glyph is expanded to 16x16 with the EPX/scale2x rule —
 * which keeps diagonal strokes connected instead of stair-stepping — and
 * then receives the same neighbour-count edge blend as the 1x AA path.   */
void gfx_draw_char_aa2x(draw_target_t *t, int x, int y,
                        char c, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font8x8[(uint8_t)c];
    bool bg_transparent = ((bg >> 24) & 0xFFu) == 0u;

    /* Source pixel lookup with bounds (off outside). */
    uint8_t big[16][16];
    for (int r = 0; r < 8; r++) {
        for (int col = 0; col < 8; col++) {
            int P = (glyph[r] >> col) & 1;
            int A = (r > 0) ? (glyph[r-1] >> col) & 1 : 0;          /* up    */
            int D = (r < 7) ? (glyph[r+1] >> col) & 1 : 0;          /* down  */
            int C = (col > 0) ? (glyph[r] >> (col-1)) & 1 : 0;      /* left  */
            int B = (col < 7) ? (glyph[r] >> (col+1)) & 1 : 0;      /* right */
            int E0 = P, E1 = P, E2 = P, E3 = P;
            if (C == A && C != D && A != B) E0 = A;
            if (A == B && A != C && B != D) E1 = B;
            if (D == C && D != B && C != A) E2 = C;
            if (B == D && B != A && D != C) E3 = D;
            big[r*2  ][col*2  ] = (uint8_t)E0;
            big[r*2  ][col*2+1] = (uint8_t)E1;
            big[r*2+1][col*2  ] = (uint8_t)E2;
            big[r*2+1][col*2+1] = (uint8_t)E3;
        }
    }
    for (int r = 0; r < 16; r++) {
        for (int col = 0; col < 16; col++) {
            if (big[r][col]) {
                gfx_putpixel(t, x + col, y + r, fg);
                continue;
            }
            int n = 0;
            if (col > 0  && big[r][col-1]) n++;
            if (col < 15 && big[r][col+1]) n++;
            if (r > 0    && big[r-1][col]) n++;
            if (r < 15   && big[r+1][col]) n++;
            if (n == 0) {
                if (!bg_transparent) gfx_putpixel(t, x + col, y + r, bg);
                continue;
            }
            uint8_t a = (uint8_t)(36 + n * 26);
            uint32_t mix = ((uint32_t)a << 24) | (fg & 0x00FFFFFFu);
            if (!bg_transparent) gfx_putpixel(t, x + col, y + r, bg);
            gfx_blend_pixel(t, x + col, y + r, mix);
        }
    }
}

void gfx_draw_string_aa2x(draw_target_t *t, int x, int y, const char *s,
                          uint32_t fg, uint32_t bg) {
    while (*s) {
        uint32_t cp = utf8_next_cp(&s);
        uint8_t  ix = glyph_index_for(cp);
        gfx_draw_char_aa2x(t, x, y, (char)ix, fg, bg);
        x += FONT_GLYPH_W * 2;
    }
}

int gfx_draw_string_aa2x_clipped(draw_target_t *t, int x, int y, int max_w,
                                 const char *s, uint32_t fg, uint32_t bg) {
    if (!s || !*s || max_w <= 0) return 0;
    int gw = FONT_GLYPH_W * 2;
    int need = gfx_string_glyph_count(s) * gw;
    int cx = x;
    if (need <= max_w) {
        gfx_draw_string_aa2x(t, x, y, s, fg, bg);
        return need;
    }
    int budget = max_w - 2 * gw;                /* room for ".." */
    const char *p = s;
    while (*p && budget >= gw) {
        uint32_t cp = utf8_next_cp(&p);
        gfx_draw_char_aa2x(t, cx, y, (char)glyph_index_for(cp), fg, bg);
        cx += gw;
        budget -= gw;
    }
    for (int i = 0; i < 2 && cx + gw <= x + max_w; i++) {
        gfx_draw_char_aa2x(t, cx, y, '.', fg, bg);
        cx += gw;
    }
    return cx - x;
}

/* ---- Shared Aero button ------------------------------------------------ *
 * Rounded body, vertical light->base gradient, glass gloss on the upper
 * half, soft 1 px outline, centred AA label.  Hover lightens the body.   */
static uint32_t aero_lighten(uint32_t c, int amt) {
    int r = (int)((c >> 16) & 0xFF) + amt;
    int g = (int)((c >>  8) & 0xFF) + amt;
    int b = (int)( c        & 0xFF) + amt;
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    if (r < 0) r = 0;     if (g < 0) g = 0;     if (b < 0) b = 0;
    return (c & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8)
         | (uint32_t)b;
}

static int isqrt_i(int v);   /* defined further down (shared AA helper) */

/* BLENDED rounded-rect outline.  gfx_draw_round_rect pokes raw pixels, so
 * translucent outline colours (0x60000000 button rings etc.) land as solid
 * near-black chunks on the corner arcs.  This variant alpha-blends every
 * pixel and anti-aliases the arc via distance coverage. */
void gfx_draw_round_rect_blend(draw_target_t *t, int x, int y, int w, int h,
                               int radius, uint32_t color) {
    if (!t || w <= 0 || h <= 0) return;
    if (radius <= 0) {
        gfx_blend_rect(t, x, y, w, 1, color);
        gfx_blend_rect(t, x, y + h - 1, w, 1, color);
        gfx_blend_rect(t, x, y + 1, 1, h - 2, color);
        gfx_blend_rect(t, x + w - 1, y + 1, 1, h - 2, color);
        return;
    }
    int maxr = (w < h ? w : h) / 2;
    if (radius > maxr) radius = maxr;
    /* Straight edges */
    gfx_blend_rect(t, x + radius, y,             w - 2 * radius, 1, color);
    gfx_blend_rect(t, x + radius, y + h - 1,     w - 2 * radius, 1, color);
    gfx_blend_rect(t, x,             y + radius, 1, h - 2 * radius, color);
    gfx_blend_rect(t, x + w - 1,     y + radius, 1, h - 2 * radius, color);
    /* AA corner arcs: coverage from the distance to the arc circle. */
    uint8_t ca = (uint8_t)((color >> 24) & 0xFFu);
    for (int dy = 0; dy <= radius; dy++) {
        for (int dx = 0; dx <= radius; dx++) {
            int rx = radius - dx, ry = radius - dy;
            int d_q8 = isqrt_i((rx * rx + ry * ry) * 65536);
            int cov = 256 - (d_q8 > radius * 256 ? d_q8 - radius * 256
                                                 : radius * 256 - d_q8);
            if (cov <= 0) continue;
            uint8_t a = (uint8_t)((cov >= 256 ? 255 : cov) * ca / 255);
            if (!a) continue;
            uint32_t c = (color & 0x00FFFFFFu) | ((uint32_t)a << 24);
            gfx_blend_pixel(t, x + dx,             y + dy,             c);
            gfx_blend_pixel(t, x + w - 1 - dx,     y + dy,             c);
            gfx_blend_pixel(t, x + dx,             y + h - 1 - dy,     c);
            gfx_blend_pixel(t, x + w - 1 - dx,     y + h - 1 - dy,     c);
        }
    }
}

/* Per-row horizontal inset that follows the CIRCULAR arc of a rounded rect
 * (radius `rad`, total height `h`).  The old widgets used a straight
 * `radius - r` chamfer, which cut every corner as a 45° diagonal and left
 * visible square artefacts over the AA silhouette. */
int gfx_round_row_inset(int row, int h, int rad) {
    int d = -1;
    if (row < rad)            d = rad - 1 - row;
    else if (row >= h - rad)  d = row - (h - rad);
    if (d < 0) return 0;
    int rem = rad * rad - (d + 1) * (d + 1);
    if (rem < 0) rem = 0;
    return rad - isqrt_i(rem);
}

void gfx_draw_button_aero(draw_target_t *t, int x, int y, int w, int h,
                          const char *label, uint32_t base, bool hot) {
    if (w < 8 || h < 8) return;
    uint32_t body = hot ? aero_lighten(base, 28) : base;
    uint32_t top  = aero_lighten(body,  46);
    uint32_t bot  = aero_lighten(body, -26);
    int radius = (h >= 24) ? 5 : 3;

    /* Body: rounded silhouette, then a vertical gradient inside it.  The
     * per-row inset follows the corner ARC so the gradient never paints
     * over the AA corner pixels (no more square-corner artefacts). */
    gfx_fill_round_rect(t, x, y, w, h, radius, body);
    for (int r = 1; r < h - 1; r++) {
        int inset = gfx_round_row_inset(r, h, radius);
        uint32_t c = aero_lighten(top, -(int)(((46 + 26) * r) / (h - 1)));
        (void)bot;
        gfx_fill_rect(t, x + 1 + inset, y + r, w - 2 - 2 * inset, 1, c);
    }
    /* Glass gloss: translucent white wash over the upper 45%. */
    int gloss_h = (h * 45) / 100;
    for (int r = 1; r <= gloss_h; r++) {
        int inset = gfx_round_row_inset(r, h, radius);
        uint8_t a = (uint8_t)(70 - (r * 50) / (gloss_h + 1));
        gfx_blend_rect(t, x + 1 + inset, y + r, w - 2 - 2 * inset, 1,
                       ((uint32_t)a << 24) | 0x00FFFFFFu);
    }
    /* Soft outline + inner top highlight (BLENDED — raw draw poked the
     * alpha byte in as near-black corner chunks). */
    gfx_draw_round_rect_blend(t, x, y, w, h, radius, 0x60000000u);
    gfx_blend_rect(t, x + radius, y + 1, w - 2 * radius, 1, 0x70FFFFFFu);

    if (label && label[0]) {
        int lw = gfx_string_pixel_width(label);
        int lx = x + (w - lw) / 2;
        if (lx < x + 4) lx = x + 4;
        int ly = y + (h - FONT_GLYPH_H) / 2;
        /* Pick the label colour by body luminance so the ONE button helper
         * serves both accent buttons (dark body -> white label) and neutral
         * toolbar buttons (light body -> dark label). */
        int luma = (((int)((body >> 16) & 0xFF)) * 30 +
                    ((int)((body >>  8) & 0xFF)) * 59 +
                    ((int)( body        & 0xFF)) * 11) / 100;
        if (luma > 150) {
            gfx_draw_string_aa(t, lx, ly, label, 0xFF1A2430u, 0x00000000u);
        } else {
            gfx_draw_string_aa(t, lx + 1, ly + 1, label, 0xFF101820u, 0x00000000u);
            gfx_draw_string_aa(t, lx, ly, label, 0xFFFFFFFFu, 0x00000000u);
        }
    }
}

/* ---- Shared Aero tab --------------------------------------------------- */
void gfx_draw_tab_aero(draw_target_t *t, int x, int y, int w, int h,
                       const char *label, uint32_t accent, bool active, bool hot) {
    if (!t || w < 8 || h < 8) return;
    uint32_t fg;
    if (active) {
        /* Glossy accent tab, rounded top, fused into the content below (+3 px),
         * a bright accent stripe at the very top, vertical gloss + soft ring.
         * Gloss rows + the stripe follow the corner arc (no square corners). */
        gfx_fill_round_rect(t, x, y, w, h + 3, 4, accent);
        gfx_fill_rect(t, x + 1, y + h, w - 2, 3, accent);
        for (int r = 1; r < h / 2; r++) {
            int inset = gfx_round_row_inset(r, h + 3, 4);
            uint8_t a = (uint8_t)(0x4E - (r * 0x40) / (h / 2 + 1));
            gfx_blend_rect(t, x + 1 + inset, y + r, w - 2 - 2 * inset, 1,
                           ((uint32_t)a << 24) | 0x00FFFFFFu);
        }
        {
            int in0 = gfx_round_row_inset(0, h + 3, 4);
            int in1 = gfx_round_row_inset(1, h + 3, 4);
            gfx_blend_rect(t, x + 1 + in0, y,     w - 2 - 2 * in0, 1,
                           (GLASS_ACCENT & 0x00FFFFFFu) | 0xC8000000u);
            gfx_blend_rect(t, x + 1 + in1, y + 1, w - 2 - 2 * in1, 1,
                           (GLASS_ACCENT & 0x00FFFFFFu) | 0xC8000000u);
        }
        gfx_draw_round_rect_blend(t, x, y, w, h + 3, 4, 0x44FFFFFFu);
        fg = 0xFFFFFFFFu;
    } else {
        /* Rounded-TOP-only silhouette so inactive tabs match the family. */
        uint32_t base = hot ? 0xFFE8EEF6u : 0xFFD9DEE7u;   /* light gray-blue */
        for (int r = 0; r < h; r++) {
            int inset = (r < 4) ? gfx_round_row_inset(r, h + 8, 4) : 0;
            gfx_fill_rect(t, x + inset, y + r, w - 2 * inset, 1, base);
        }
        for (int r = 0; r < h / 2; r++) {
            int inset = (r < 4) ? gfx_round_row_inset(r, h + 8, 4) : 0;
            gfx_blend_rect(t, x + inset, y + r, w - 2 * inset, 1,
                           hot ? 0x33FFFFFFu : 0x18FFFFFFu);
        }
        {
            int in0 = gfx_round_row_inset(0, h + 8, 4);
            gfx_blend_rect(t, x + in0, y, w - 2 * in0, 1, GLASS_EDGE_LIGHT);
        }
        gfx_blend_rect(t, x, y + h - 1, w, 1, GLASS_EDGE_DARK);                /* depth */
        fg = GLASS_TEXT_DARK;
    }
    if (label && label[0]) {
        int lw = gfx_string_pixel_width(label);
        int avail = w - 6;
        int tx = (lw <= avail) ? x + (w - lw) / 2 : x + 3;
        gfx_draw_string_aa_clipped(t, tx, y + (h - FONT_GLYPH_H) / 2, avail,
                                   label, fg, 0x00000000u);
    }
}

/* ---- Shared Aero slider ------------------------------------------------ */
static int isqrt_i(int v);   /* defined further down */
static inline void gfx_set_or_blend(draw_target_t *t, int x, int y,
                                    uint32_t color, uint8_t alpha);

/* Opaque ARGB linear interpolation a->b (num/den), forced to alpha 0xFF. */
static uint32_t gfx_argb_mix(uint32_t a, uint32_t b, int num, int den) {
    if (den <= 0) return 0xFF000000u | (a & 0x00FFFFFFu);
    if (num < 0) num = 0; if (num > den) num = den;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int rr = ar + (br - ar) * num / den;
    int rg = ag + (bg - ag) * num / den;
    int rb = ab + (bb - ab) * num / den;
    return 0xFF000000u | ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
}

void gfx_draw_slider_aero(draw_target_t *t, int x, int y, int w, int h,
                          int pos_permil, uint32_t accent) {
    if (!t || w < 8 || h < 6) return;
    if (pos_permil < 0) pos_permil = 0;
    if (pos_permil > 1000) pos_permil = 1000;
    int tr_h  = 6;
    int tr_y  = y + (h - tr_h) / 2;
    int thumb = h;                              /* round thumb diameter = h    */
    int span  = w - thumb;                      /* travel for the thumb centre */
    int fillw = (span * pos_permil) / 1000 + thumb / 2;
    /* Sunken glass groove. */
    gfx_fill_round_rect(t, x + thumb / 2, tr_y, w - thumb, tr_h, tr_h / 2, 0x55203040u);
    gfx_blend_rect(t, x + thumb / 2, tr_y, w - thumb, 1, GLASS_EDGE_DARK);
    /* Filled portion in the accent. */
    if (fillw > thumb / 2)
        gfx_fill_round_rect(t, x + thumb / 2, tr_y, fillw - thumb / 2, tr_h, tr_h / 2, accent);

    /* Glossy round thumb — a clean glass bead that sits ON the rail.  Drawn
     * as a single anti-aliased circle with a top->bottom glass gradient, a
     * thin slate rim and a soft top specular.  No opaque white square /
     * halo behind it (the old r/3 highlight rendered as a hard box). */
    int cx = x + thumb / 2 + (span * pos_permil) / 1000;
    int cy = y + h / 2;
    int r  = thumb / 2;
    if (r < 3) r = 3;
    int rim_r2 = r * r;
    int in_r   = r - 1;            /* inside the rim band */
    int in_r2  = in_r * in_r;
    for (int dy = -r; dy <= r; dy++) {
        int rem = rim_r2 - dy * dy;
        if (rem < 0) continue;
        int sp = isqrt_i(rem);
        /* Glass body gradient: light top -> cooler bottom. */
        uint32_t body = gfx_argb_mix(0xFFEFF4FBu, 0xFFAEC2D8u, dy + r, 2 * r);
        for (int dx = -sp; dx <= sp; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= in_r2) {
                gfx_putpixel(t, cx + dx, cy + dy, body);
            } else {
                /* Rim band: blend a slate edge, AA against the outside. */
                int dist = isqrt_i(d2);
                int a = 255 - (dist - in_r) * 255;
                if (a < 0) a = 0; if (a > 255) a = 255;
                gfx_set_or_blend(t, cx + dx, cy + dy, 0xFF54708Cu, (uint8_t)a);
            }
        }
    }
    /* Soft top specular: a small radial highlight, blended (never a box). */
    int hlr = r / 2; if (hlr < 1) hlr = 1;
    int hlcy = cy - r / 3;
    int hl2  = hlr * hlr;
    for (int dy = -hlr; dy <= hlr; dy++) {
        for (int dx = -hlr; dx <= hlr; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > hl2) continue;
            int a = 150 - d2 * 150 / (hl2 + 1);
            if (a <= 0) continue;
            gfx_blend_pixel(t, cx + dx, hlcy + dy,
                            ((uint32_t)(uint8_t)a << 24) | 0x00FFFFFFu);
        }
    }
}

/* ============================================================================
 * TASK 5: rounded primitives, drop shadows, splines, TTF-style rasteriser
 * ============================================================================ */

/* Integer square root via Newton iteration (good enough for radii). */
static int isqrt_i(int v) {
    if (v <= 0) return 0;
    int x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

/* Set or blend a pixel without clipping branches in the inner loop. */
static inline void gfx_set_or_blend(draw_target_t *t, int x, int y,
                                    uint32_t color, uint8_t alpha) {
    if (alpha == 0) return;
    if (alpha == 255) { gfx_putpixel(t, x, y, color); return; }
    uint32_t c = (color & 0x00FFFFFFu) | ((uint32_t)alpha << 24);
    gfx_blend_pixel(t, x, y, c);
}

void gfx_fill_round_rect(draw_target_t *t, int x, int y, int w, int h,
                         int radius, uint32_t color) {
    if (!t || w <= 0 || h <= 0) return;
    if (radius <= 0) { gfx_fill_rect(t, x, y, w, h, color); return; }
    int maxr = (w < h ? w : h) / 2;
    if (radius > maxr) radius = maxr;
    /* Middle band: full-width rows above + below the rounded caps. */
    gfx_fill_rect(t, x, y + radius, w, h - 2 * radius, color);
    /* Side fills bordering the top/bottom rounded caps. */
    gfx_fill_rect(t, x + radius, y,                 w - 2 * radius, radius, color);
    gfx_fill_rect(t, x + radius, y + h - radius,    w - 2 * radius, radius, color);
    /* Corner caps: 4x4 coverage sampling of the quarter circle. */
    int r2 = radius * radius;
    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            /* Sample 4x4 sub-pixels. */
            int hits = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    int px = dx * 4 + sx + 2;
                    int py = dy * 4 + sy + 2;
                    int rx = (radius * 4 - px);
                    int ry = (radius * 4 - py);
                    if (rx * rx + ry * ry <= r2 * 16) hits++;
                }
            }
            if (hits == 0) continue;
            uint8_t a = (uint8_t)((hits * 255) / 16);
            /* Top-left */
            gfx_set_or_blend(t, x + dx,             y + dy,             color, a);
            /* Top-right */
            gfx_set_or_blend(t, x + w - 1 - dx,     y + dy,             color, a);
            /* Bottom-left */
            gfx_set_or_blend(t, x + dx,             y + h - 1 - dy,     color, a);
            /* Bottom-right */
            gfx_set_or_blend(t, x + w - 1 - dx,     y + h - 1 - dy,     color, a);
        }
    }
}

void gfx_draw_round_rect(draw_target_t *t, int x, int y, int w, int h,
                         int radius, uint32_t color) {
    if (!t || w <= 0 || h <= 0) return;
    if (radius <= 0) { gfx_draw_rect(t, x, y, w, h, color); return; }
    int maxr = (w < h ? w : h) / 2;
    if (radius > maxr) radius = maxr;
    /* Straight edges. */
    gfx_draw_hline(t, x + radius, y,             w - 2 * radius, color);
    gfx_draw_hline(t, x + radius, y + h - 1,     w - 2 * radius, color);
    gfx_draw_vline(t, x,             y + radius, h - 2 * radius, color);
    gfx_draw_vline(t, x + w - 1,     y + radius, h - 2 * radius, color);
    /* Corner arc: midpoint algorithm with simple AA via fractional residual. */
    int rr = radius;
    int dx = rr - 1, dy = 0;
    int err = dx - 2 * rr;
    while (dx >= dy) {
        /* Four corners. */
        gfx_putpixel(t, x + rr - dx,     y + rr - dy,     color);
        gfx_putpixel(t, x + rr - dy,     y + rr - dx,     color);
        gfx_putpixel(t, x + w - rr + dx, y + rr - dy,     color);
        gfx_putpixel(t, x + w - rr + dy, y + rr - dx,     color);
        gfx_putpixel(t, x + rr - dx,     y + h - rr + dy, color);
        gfx_putpixel(t, x + rr - dy,     y + h - rr + dx, color);
        gfx_putpixel(t, x + w - rr + dx, y + h - rr + dy, color);
        gfx_putpixel(t, x + w - rr + dy, y + h - rr + dx, color);
        if (err <= 0) { dy++; err += 2 * dy + 1; }
        if (err  > 0) { dx--; err -= 2 * dx + 1; }
    }
}

/* Soft drop shadow that clips against the same rounded rectangle. */
void gfx_drop_shadow_round(draw_target_t *t, int x, int y, int w, int h,
                           int radius, int blur) {
    if (!t || w <= 0 || h <= 0 || blur <= 0) return;
    if (radius < 0) radius = 0;
    /* True per-pixel BLENDED shadow.  The previous implementation drew the
     * blur rings through gfx_draw_round_rect, which pokes RAW pixels — the
     * alpha byte was ignored and every ring landed as solid black, giving
     * popups a hard black outline instead of a shadow.  Now each pixel of
     * the band computes its distance outside the rounded silhouette and
     * blends a smoothly decaying alpha (quadratic falloff, slight downward
     * bias).  Cost is O((w+h)*blur) per popup — negligible. */
    const int oy = 2;                                 /* downward bias */
    int x0 = x - blur, x1 = x + w + blur;
    int y0 = y - blur + oy, y1 = y + h + blur + oy;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            /* Distance from (px,py) to the rounded-rect silhouette. */
            int qy = py - oy;
            int cx = px, cy = qy;
            if (cx < x + radius)              cx = x + radius;
            else if (cx > x + w - 1 - radius) cx = x + w - 1 - radius;
            if (cy < y + radius)              cy = y + radius;
            else if (cy > y + h - 1 - radius) cy = y + h - 1 - radius;
            int dx = px - cx, dy = qy - cy;
            int d;
            if (dx == 0 && dy == 0) continue;         /* inside the core   */
            d = isqrt_i(dx * dx + dy * dy) - radius;
            if (d <= 0) continue;                     /* under the panel   */
            if (d > blur) continue;
            int t_n = blur - d;                        /* blur-1 .. 0       */
            int a = (26 * t_n * t_n) / (blur * blur);
            if (a < 2) a = 2;
            gfx_blend_pixel(t, px, py, (uint32_t)a << 24);
        }
    }
}

/* ============================================================================
 * Liquid-Glass compositing primitives
 * ============================================================================ */

/* Scratch line buffer for the separable blur (>= any screen dimension). */
#define GFX_BLUR_SCRATCH 2048
static uint32_t g_blur_tmp[GFX_BLUR_SCRATCH];

/* Box-blur one strip of `n` pixels at base[i*stride] (stride in uint32
 * units).  A private copy is taken first so the in-place writes never
 * corrupt a neighbour's still-needed window.  Running-sum -> O(n). */
static void blur_strip(uint32_t *base, int n, int stride, int radius) {
    if (n <= 1 || radius < 1) return;
    if (n > GFX_BLUR_SCRATCH) n = GFX_BLUR_SCRATCH;
    for (int i = 0; i < n; i++) g_blur_tmp[i] = base[(size_t)i * (size_t)stride];
    int win = 2 * radius + 1;
    int sr = 0, sg = 0, sb = 0;
    for (int k = -radius; k <= radius; k++) {
        int idx = k < 0 ? 0 : (k >= n ? n - 1 : k);
        uint32_t p = g_blur_tmp[idx];
        sr += (int)((p >> 16) & 0xFF);
        sg += (int)((p >>  8) & 0xFF);
        sb += (int)( p        & 0xFF);
    }
    for (int i = 0; i < n; i++) {
        uint32_t r = (uint32_t)(sr / win);
        uint32_t g = (uint32_t)(sg / win);
        uint32_t b = (uint32_t)(sb / win);
        base[(size_t)i * (size_t)stride] = 0xFF000000u | (r << 16) | (g << 8) | b;
        int li = i - radius;     if (li < 0)  li = 0;
        int ri = i + radius + 1; if (ri >= n) ri = n - 1;
        uint32_t pl = g_blur_tmp[li], pr = g_blur_tmp[ri];
        sr += (int)((pr >> 16) & 0xFF) - (int)((pl >> 16) & 0xFF);
        sg += (int)((pr >>  8) & 0xFF) - (int)((pl >>  8) & 0xFF);
        sb += (int)( pr        & 0xFF) - (int)( pl        & 0xFF);
    }
}

void gfx_box_blur(draw_target_t *t, int x, int y, int w, int h, int radius) {
    if (!t || !t->fb || radius < 1) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((uint32_t)(x + w) > t->width)  w = (int)t->width  - x;
    if ((uint32_t)(y + h) > t->height) h = (int)t->height - y;
    if (!clip_box(t, &x, &y, &w, &h)) return;   /* region-based: stay in scissor */
    int stride = (int)(t->pitch / 4u);
    for (int row = 0; row < h; row++) {       /* horizontal pass */
        uint32_t *base = (uint32_t *)(t->fb + (size_t)(y + row) * t->pitch) + x;
        blur_strip(base, w, 1, radius);
    }
    for (int col = 0; col < w; col++) {        /* vertical pass */
        uint32_t *base = (uint32_t *)(t->fb + (size_t)y * t->pitch) + (x + col);
        blur_strip(base, h, stride, radius);
    }
}

void gfx_blend_round_rect(draw_target_t *t, int x, int y, int w, int h,
                          int radius, uint32_t argb) {
    if (!t || w <= 0 || h <= 0) return;
    uint32_t sa = (argb >> 24) & 0xFFu;
    if (sa == 0) return;
    if (radius <= 0) { gfx_blend_rect(t, x, y, w, h, argb); return; }
    int maxr = (w < h ? w : h) / 2;
    if (radius > maxr) radius = maxr;
    /* Straight bands (same decomposition as gfx_fill_round_rect). */
    gfx_blend_rect(t, x, y + radius, w, h - 2 * radius, argb);
    gfx_blend_rect(t, x + radius, y,              w - 2 * radius, radius, argb);
    gfx_blend_rect(t, x + radius, y + h - radius, w - 2 * radius, radius, argb);
    /* Corner caps: 4x4 coverage folded into the source alpha. */
    int r2 = radius * radius;
    uint32_t rgb = argb & 0x00FFFFFFu;
    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int px = dx * 4 + sx + 2;
                    int py = dy * 4 + sy + 2;
                    int rx = radius * 4 - px;
                    int ry = radius * 4 - py;
                    if (rx * rx + ry * ry <= r2 * 16) hits++;
                }
            if (hits == 0) continue;
            uint32_t a = ((uint32_t)hits * sa) / 16u;
            if (a == 0) continue;
            uint32_t c = rgb | (a << 24);
            gfx_blend_pixel(t, x + dx,         y + dy,         c);
            gfx_blend_pixel(t, x + w - 1 - dx, y + dy,         c);
            gfx_blend_pixel(t, x + dx,         y + h - 1 - dy, c);
            gfx_blend_pixel(t, x + w - 1 - dx, y + h - 1 - dy, c);
        }
    }
}

void gfx_glass_bevel(draw_target_t *t, int x, int y, int w, int h,
                     int radius, uint32_t light, uint32_t dark) {
    if (!t || w <= 2 || h <= 2) return;
    int r = radius < 0 ? 0 : radius;
    if (r > (w < h ? w : h) / 2) r = (w < h ? w : h) / 2;
    gfx_blend_rect(t, x + r, y,         w - 2 * r, 1, light);   /* top    */
    gfx_blend_rect(t, x,     y + r,     1, h - 2 * r, light);   /* left   */
    gfx_blend_rect(t, x + r, y + h - 1, w - 2 * r, 1, dark);    /* bottom */
    gfx_blend_rect(t, x + w - 1, y + r, 1, h - 2 * r, dark);    /* right  */
    if (r > 0) gfx_draw_round_rect(t, x, y, w, h, r, light);    /* arcs   */
}

void gfx_glass_panel(draw_target_t *t, int x, int y, int w, int h,
                     int radius, uint32_t fill) {
    if (!t || !t->fb || w <= 0 || h <= 0) return;
    gfx_box_blur(t, x, y, w, h, GLASS_BLUR_R);
    gfx_blend_round_rect(t, x, y, w, h, radius, fill);
    /* Top gloss sweep — light catching the upper edge of the glass. */
    int gloss_h = h / 3;
    if (gloss_h < 2)  gloss_h = 2;
    if (gloss_h > 22) gloss_h = 22;
    gfx_blend_round_rect(t, x + 1, y + 1, w - 2, gloss_h,
                         radius > 1 ? radius - 1 : 0, GLASS_GLOSS);
    gfx_glass_bevel(t, x, y, w, h, radius, GLASS_EDGE_LIGHT, GLASS_EDGE_DARK);
}

/* ---- Anti-aliased line (Wu's algorithm) ------------------------------- */
static int iabs(int v) { return v < 0 ? -v : v; }

static void gfx_line_aa(draw_target_t *t, int x0, int y0, int x1, int y1,
                        uint32_t color) {
    int steep = iabs(y1 - y0) > iabs(x1 - x0);
    if (steep) { int tmp = x0; x0 = y0; y0 = tmp; tmp = x1; x1 = y1; y1 = tmp; }
    if (x0 > x1) {
        int tmp = x0; x0 = x1; x1 = tmp;
        tmp = y0; y0 = y1; y1 = tmp;
    }
    int dx = x1 - x0;
    int dy = y1 - y0;
    if (dx == 0) {
        if (steep) gfx_draw_hline(t, y0, x0, 1, color);
        else       gfx_draw_vline(t, x0, y0, 1, color);
        return;
    }
    int gradient_q = (dy * 256) / dx;
    int intery_q   = y0 * 256;
    for (int x = x0; x <= x1; x++) {
        int yi = intery_q >> 8;
        int frac = intery_q & 0xFF;
        uint8_t a1 = (uint8_t)(255 - frac);
        uint8_t a2 = (uint8_t)frac;
        if (steep) {
            gfx_set_or_blend(t, yi,     x, color, a1);
            gfx_set_or_blend(t, yi + 1, x, color, a2);
        } else {
            gfx_set_or_blend(t, x, yi,     color, a1);
            gfx_set_or_blend(t, x, yi + 1, color, a2);
        }
        intery_q += gradient_q;
    }
}

void gfx_polyline_aa(draw_target_t *t, const int *xs, const int *ys,
                     int count, uint32_t color) {
    if (!t || count < 2 || !xs || !ys) return;
    for (int i = 0; i + 1 < count; i++) {
        gfx_line_aa(t, xs[i], ys[i], xs[i + 1], ys[i + 1], color);
    }
}

/* Catmull-Rom / cardinal-spline tessellation.  Returns coordinates per
 * segment as straight-line approximations.  `tension` is 0..255 where
 * 128 is the canonical Catmull-Rom value. */
void gfx_spline_cardinal_aa(draw_target_t *t, const int *xs, const int *ys,
                            int count, int segments, int tension,
                            uint32_t color) {
    if (!t || count < 2 || segments < 1 || segments > 64) return;
    if (tension < 0)   tension = 0;
    if (tension > 255) tension = 255;
    int s_q = (255 - tension);     /* scale (0..255) */
    /* The spline needs the two outer "ghost" points; duplicate the
     * endpoints if missing. */
    int px[64], py[64];
    int n = 0;
    for (int i = 0; i < count - 1 && n + 1 < 64; i++) {
        int p0x = (i == 0) ? xs[0] : xs[i - 1];
        int p0y = (i == 0) ? ys[0] : ys[i - 1];
        int p1x = xs[i],         p1y = ys[i];
        int p2x = xs[i + 1],     p2y = ys[i + 1];
        int p3x = (i + 2 < count) ? xs[i + 2] : xs[count - 1];
        int p3y = (i + 2 < count) ? ys[i + 2] : ys[count - 1];
        for (int k = 0; k < segments && n < 64; k++) {
            int t_q = (k * 256) / segments;     /* 0..255 */
            int tt  = (t_q * t_q) >> 8;
            int ttt = (tt * t_q) >> 8;
            /* Catmull-Rom basis with tension `s_q`. */
            int s = s_q;
            int b1 = -s * ttt + 2 * s * tt - s * t_q;
            int b2 = (2 * 256 - s) * ttt + (s - 3 * 256) * tt + 256 * 256 / 256;
            int b3 = (s - 2 * 256) * ttt + (3 * 256 - 2 * s) * tt + s * t_q;
            int b4 = s * ttt - s * tt;
            int x = (b1 * p0x + b2 * p1x + b3 * p2x + b4 * p3x) >> 8;
            int y = (b1 * p0y + b2 * p1y + b3 * p2y + b4 * p3y) >> 8;
            px[n] = x / 256; py[n] = y / 256;
            n++;
        }
    }
    if (n + 1 < 64) {
        px[n] = xs[count - 1];
        py[n] = ys[count - 1];
        n++;
    }
    gfx_polyline_aa(t, px, py, n, color);
}

void gfx_fill_circle(draw_target_t *t, int cx, int cy, int r, uint32_t color) {
    if (!t || r <= 0) return;
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int dy2 = dy * dy;
        int span = isqrt_i(r2 - dy2);
        for (int dx = -span; dx <= span; dx++) {
            /* Distance to circle edge for boundary blending. */
            int d2 = dx * dx + dy2;
            if (d2 <= (r - 1) * (r - 1)) {
                gfx_putpixel(t, cx + dx, cy + dy, color);
            } else {
                /* Boundary band: 0..255 alpha */
                int dist = isqrt_i(d2);
                int a = 255 - (dist - (r - 1)) * 255;
                if (a < 0)   a = 0;
                if (a > 255) a = 255;
                gfx_set_or_blend(t, cx + dx, cy + dy, color, (uint8_t)a);
            }
        }
    }
}

/* ---- TTF-style outline rasteriser ------------------------------------- *
 * Bitmap glyphs are 8x8.  We upsample to (scale*8) x (scale*8) by mapping
 * each (sub) pixel back into the source glyph and computing coverage as
 * the count of "on" 4-neighbour bits.  This gives smooth diagonals at any
 * scale without requiring a real Bezier outline. */
extern const uint8_t font8x8[256][8];   /* defined in font.c */

int gfx_ttf_advance(int scale) {
    if (scale < 1) scale = 1;
    return 8 * scale;
}

void gfx_draw_char_ttf(draw_target_t *t, int x, int y, char c,
                       int scale, uint32_t fg, uint32_t bg) {
    if (!t) return;
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    const uint8_t *glyph = font8x8[(uint8_t)c];
    /* Pre-render the 10x10 padded glyph (padding for neighbour sampling). */
    uint8_t pad[10][10];
    for (int row = 0; row < 10; row++)
        for (int col = 0; col < 10; col++)
            pad[row][col] = 0;
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            pad[row + 1][col + 1] = (glyph[row] >> col) & 1;

    int W = 8 * scale, H = 8 * scale;
    for (int py = 0; py < H; py++) {
        int srcy_q = (py * 8 * 256) / H;
        int sy_int = srcy_q >> 8;
        int sy_frac = srcy_q & 0xFF;
        for (int px = 0; px < W; px++) {
            int srcx_q = (px * 8 * 256) / W;
            int sx_int = srcx_q >> 8;
            int sx_frac = srcx_q & 0xFF;
            /* Bilinear sample of the binary glyph using the padded grid. */
            uint8_t a = pad[sy_int + 1][sx_int + 1];
            uint8_t b = (sx_int < 7) ? pad[sy_int + 1][sx_int + 2] : 0;
            uint8_t cc = (sy_int < 7) ? pad[sy_int + 2][sx_int + 1] : 0;
            uint8_t dd = (sx_int < 7 && sy_int < 7)
                          ? pad[sy_int + 2][sx_int + 2] : 0;
            int top    = a * (256 - sx_frac) + b * sx_frac;
            int bottom = cc * (256 - sx_frac) + dd * sx_frac;
            int cov    = (top * (256 - sy_frac) + bottom * sy_frac) >> 16;
            if (cov >= 255) {
                gfx_putpixel(t, x + px, y + py, fg);
            } else if (cov == 0) {
                if ((bg & 0xFF000000u) != 0)
                    gfx_putpixel(t, x + px, y + py, bg);
            } else {
                uint32_t mix = (fg & 0x00FFFFFFu) | ((uint32_t)cov << 24);
                gfx_blend_pixel(t, x + px, y + py, mix);
            }
        }
    }
}

void gfx_draw_string_ttf(draw_target_t *t, int x, int y, const char *s,
                         int scale, uint32_t fg, uint32_t bg) {
    if (!t || !s) return;
    int adv = gfx_ttf_advance(scale);
    int cx = x;
    /* Decode UTF-8 to a glyph index just like gfx_draw_string so that
     * scaled text (spreadsheet cells, etc.) renders Hungarian accented
     * letters as one glyph instead of two stray bytes. */
    while (*s) {
        uint32_t cp = utf8_next_cp(&s);
        uint8_t  ix = glyph_index_for(cp);
        gfx_draw_char_ttf(t, cx, y, (char)ix, scale, fg, bg);
        cx += adv;
    }
}
