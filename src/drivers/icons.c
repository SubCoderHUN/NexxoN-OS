/* ============================================================================
 * NexxoN OS - Vector pictogram icon set (Liquid Glass)
 * ----------------------------------------------------------------------------
 * Every icon is drawn from primitives (AA capsule strokes, AA rings/arcs,
 * AA triangles, the gfx round-rect/circle fills) — no bitmaps — so they
 * scale to any box size and stay crisp on any backdrop (frosted glass,
 * wallpaper, taskbar).  The palette leans on the Liquid Glass look: cool
 * blues and silvers with one saturated accent per icon.
 * ============================================================================ */
#include "icons.h"
#include "string.h"

/* ---------- shared AA helpers ------------------------------------------- */

/* Integer sqrt (Newton). */
static int isq(int v) {
    if (v <= 0) return 0;
    int x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

/* Anti-aliased capsule stroke from (x0,y0) to (x1,y1); half width in Q8. */
static void st(draw_target_t *t, int x0, int y0, int x1, int y1,
               int half_q8, uint32_t color) {
    int minx = (x0 < x1 ? x0 : x1) - 3, maxx = (x0 > x1 ? x0 : x1) + 3;
    int miny = (y0 < y1 ? y0 : y1) - 3, maxy = (y0 > y1 ? y0 : y1) + 3;
    int abx = x1 - x0, aby = y1 - y0;
    int len_sq = abx * abx + aby * aby;
    int ablen  = isq(len_sq); if (ablen < 1) ablen = 1;
    uint8_t ca = (uint8_t)((color >> 24) & 0xFFu);
    if (!ca) ca = 255;
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            int apx = px - x0, apy = py - y0;
            int dot = apx * abx + apy * aby;
            int dist_q8;
            if (dot <= 0) {
                dist_q8 = isq((apx * apx + apy * apy) * 65536);
            } else if (dot >= len_sq) {
                int bpx = px - x1, bpy = py - y1;
                dist_q8 = isq((bpx * bpx + bpy * bpy) * 65536);
            } else {
                int cross = abx * apy - aby * apx;
                if (cross < 0) cross = -cross;
                dist_q8 = (cross * 256) / ablen;
            }
            int cov = half_q8 + 128 - dist_q8;
            if (cov <= 0) continue;
            if (cov > 256) cov = 256;
            uint8_t al = (uint8_t)((cov >= 256 ? 255 : cov) * ca / 255);
            if (!al) continue;
            gfx_blend_pixel(t, px, py,
                            (color & 0x00FFFFFFu) | ((uint32_t)al << 24));
        }
    }
}

/* AA ring (donut) centred (cx,cy): outer radius r_q8, thickness th_q8 (Q8).
 * `arc` selects a segment: 0 = full ring; otherwise a predicate mask —
 *   bit0: include dx>=0,dy<0   (NE)   bit1: include dx>=0,dy>=0 (SE)
 *   bit2: include dx<0, dy>=0  (SW)   bit3: include dx<0, dy<0  (NW) */
static void ring(draw_target_t *t, int cx, int cy, int r_q8, int th_q8,
                 uint32_t color, int arc) {
    int r  = (r_q8 + 255) >> 8;
    int lo = r_q8 - th_q8;
    uint8_t ca = (uint8_t)((color >> 24) & 0xFFu);
    if (!ca) ca = 255;
    for (int dy = -r - 1; dy <= r + 1; dy++) {
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            if (arc) {
                int q = (dy < 0) ? ((dx >= 0) ? 0 : 3) : ((dx >= 0) ? 1 : 2);
                if (!(arc & (1 << q))) continue;
            }
            int d_q8 = isq((dx * dx + dy * dy) * 65536);
            int cov_out = r_q8 + 128 - d_q8;          /* inside outer edge */
            int cov_in  = d_q8 - lo + 128;            /* outside inner edge */
            int cov = cov_out < cov_in ? cov_out : cov_in;
            if (cov <= 0) continue;
            if (cov > 256) cov = 256;
            uint8_t al = (uint8_t)((cov >= 256 ? 255 : cov) * ca / 255);
            if (!al) continue;
            gfx_blend_pixel(t, cx + dx, cy + dy,
                            (color & 0x00FFFFFFu) | ((uint32_t)al << 24));
        }
    }
}

/* AA-filled triangle via edge functions (Q4 coords: pass x*16). */
static void tri(draw_target_t *t, int x0, int y0, int x1, int y1,
                int x2, int y2, uint32_t color) {
    int minx = x0, maxx = x0, miny = y0, maxy = y0;
    if (x1 < minx) minx = x1; if (x1 > maxx) maxx = x1;
    if (x2 < minx) minx = x2; if (x2 > maxx) maxx = x2;
    if (y1 < miny) miny = y1; if (y1 > maxy) maxy = y1;
    if (y2 < miny) miny = y2; if (y2 > maxy) maxy = y2;
    minx >>= 4; maxx = (maxx >> 4) + 1;
    miny >>= 4; maxy = (maxy >> 4) + 1;
    /* signed area (winding) */
    long area = (long)(x1 - x0) * (y2 - y0) - (long)(y1 - y0) * (x2 - x0);
    if (area == 0) return;
    int flip = area < 0 ? -1 : 1;
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            /* 2x2 supersample for the AA edge */
            int hits = 0;
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    int qx = px * 16 + 4 + sx * 8;
                    int qy = py * 16 + 4 + sy * 8;
                    long e0 = ((long)(x1 - x0) * (qy - y0) - (long)(y1 - y0) * (qx - x0)) * flip;
                    long e1 = ((long)(x2 - x1) * (qy - y1) - (long)(y2 - y1) * (qx - x1)) * flip;
                    long e2 = ((long)(x0 - x2) * (qy - y2) - (long)(y0 - y2) * (qx - x2)) * flip;
                    if (e0 >= 0 && e1 >= 0 && e2 >= 0) hits++;
                }
            }
            if (!hits) continue;
            uint8_t al = (uint8_t)(hits * 255 / 4);
            gfx_blend_pixel(t, px, py,
                            (color & 0x00FFFFFFu) | ((uint32_t)al << 24));
        }
    }
}

/* Small white paper sheet with a folded corner, used by editor / sheet. */
static void paper(draw_target_t *t, int x, int y, int w, int h) {
    gfx_fill_round_rect(t, x, y, w, h, 2, 0xFFF7F9FC);
    int fold = w / 4; if (fold < 3) fold = 3;
    gfx_fill_rect(t, x + w - fold, y, fold, fold, 0xFFD4DAE4);
    gfx_draw_round_rect(t, x, y, w, h, 2, 0xFF8B94A3);
}

/* ---------- the pictograms ---------------------------------------------- */

static void ic_taskmgr(draw_target_t *t, int x, int y, int s) {
    int mw = s - s / 8, mh = s * 5 / 8;
    int mx = x + (s - mw) / 2, my = y + s / 8;
    gfx_fill_round_rect(t, mx, my, mw, mh, s / 8, 0xFF23456E);      /* frame  */
    gfx_fill_rect(t, mx + 2, my + 2, mw - 4, mh - 4, 0xFFEAF2FB);   /* screen */
    int bw = (mw - 10) / 3;                                         /* bars   */
    int bh1 = mh / 3, bh2 = mh - 8, bh3 = mh / 2;
    int base = my + mh - 3;
    gfx_fill_rect(t, mx + 3,              base - bh1, bw, bh1, 0xFF3FB54A);
    gfx_fill_rect(t, mx + 5 + bw,         base - bh2 + 3, bw, bh2 - 3, 0xFF57CD62);
    gfx_fill_rect(t, mx + 7 + 2 * bw,     base - bh3, bw, bh3, 0xFF2E9B3A);
    /* stand */
    gfx_fill_rect(t, x + s / 2 - 1, my + mh, 3, s / 8, 0xFF3E5875);
    gfx_fill_round_rect(t, x + s / 4, my + mh + s / 8, s / 2, 2, 1, 0xFF3E5875);
}

static void ic_settings(draw_target_t *t, int x, int y, int s) {
    int cx = x + s / 2, cy = y + s / 2;
    int ro = s * 7 / 16;
    /* 8 teeth */
    static const int dirs[8][2] = {
        {256,0},{181,181},{0,256},{-181,181},{-256,0},{-181,-181},{0,-256},{181,-181}
    };
    for (int k = 0; k < 8; k++) {
        int x0 = cx + dirs[k][0] * (ro - s / 6) / 256;
        int y0 = cy + dirs[k][1] * (ro - s / 6) / 256;
        int x1 = cx + dirs[k][0] * ro / 256;
        int y1 = cy + dirs[k][1] * ro / 256;
        st(t, x0, y0, x1, y1, s * 26, 0xFF5F7089);
    }
    ring(t, cx, cy, (ro - s / 12) * 256, s * 56, 0xFF6E8098, 0);
    ring(t, cx, cy - 1, (ro - s / 8) * 256, s * 20, 0xFFBACAD9, 8 | 1); /* gloss */
    gfx_fill_circle(t, cx, cy, s / 7 > 2 ? s / 7 : 2, 0xFF3C4E66);   /* hub  */
}

static void ic_explorer(draw_target_t *t, int x, int y, int s) {
    /* magnifying glass over a subtle folder */
    int fw = s * 3 / 5, fh = s * 2 / 5;
    int fx = x + 1, fy = y + s - fh - 2;
    gfx_fill_round_rect(t, fx, fy, fw / 3, 3, 1, 0xFFE0A94E);        /* tab   */
    gfx_fill_round_rect(t, fx, fy + 2, fw, fh - 2, 2, 0xFFF2C063);   /* body  */
    gfx_draw_round_rect(t, fx, fy + 2, fw, fh - 2, 2, 0xFFB8860B);
    int lr = s * 9 / 32;                                             /* lens  */
    int lx = x + s - lr - 3, ly = y + lr + 2;
    gfx_fill_circle(t, lx, ly, lr, 0x66BEE1FF);
    ring(t, lx, ly, lr * 256, s * 34, 0xFF3E6FA8, 0);
    ring(t, lx - 1, ly - 1, (lr - 2) * 256, s * 12, 0xAAFFFFFF, 8);  /* shine */
    st(t, lx + lr * 60 / 100, ly + lr * 60 / 100,
       x + s - 2, y + s - 2, s * 26, 0xFF35608F);                    /* handle */
}

static void ic_browser(draw_target_t *t, int x, int y, int s) {
    int cx = x + s / 2, cy = y + s / 2, r = s * 7 / 16;
    gfx_fill_circle(t, cx, cy, r, 0xFF2F7FD4);
    gfx_fill_circle(t, cx - r / 4, cy - r / 4, r / 2, 0x502F9FFF);   /* sheen */
    ring(t, cx, cy, r * 256, s * 16, 0xFF1B5FA8, 0);                 /* rim   */
    /* meridians + equator */
    st(t, x + s / 2 - r + 1, cy, x + s / 2 + r - 1, cy, s * 11, 0xCCEAF5FF);
    ring(t, cx, cy, r * 256, s * 11, 0xAAEAF5FF, 0);
    /* central vertical ellipse approximated by a squeezed ring: draw as two
     * arcs of a smaller-radius ring shifted — cheap: vertical line + curves */
    st(t, cx, cy - r + 1, cx, cy + r - 1, s * 11, 0xAAEAF5FF);
    ring(t, cx - r / 2, cy, (r * 5 / 4) * 256, s * 10, 0x77EAF5FF, 1 | 2);
    ring(t, cx + r / 2, cy, (r * 5 / 4) * 256, s * 10, 0x77EAF5FF, 4 | 8);
}

static void ic_editor(draw_target_t *t, int x, int y, int s) {
    paper(t, x + 1, y + 1, s * 5 / 8, s - 2);
    int px0 = x + s * 5 / 16, py0 = y + s * 5 / 8;
    for (int i = 0; i < 3; i++)
        gfx_fill_rect(t, x + 4, y + 4 + i * (s / 6), s * 3 / 8, 1, 0xFFAAB4C2);
    /* pencil: body + tip */
    st(t, x + s - 3, y + 3, px0 + 2, py0 + 2, s * 22, 0xFFED9F2E);
    st(t, x + s - 3, y + 3, x + s - 5, y + 5, s * 22, 0xFFDD8511);  /* eraser end */
    tri(t, (px0 + 2) * 16, (py0 + 2) * 16,
           (px0 + 6) * 16, (py0 - 1) * 16,
           (px0 - 1) * 16, (py0 + 6) * 16, 0xFF6B4A23);            /* tip */
}

static void ic_nexsheet(draw_target_t *t, int x, int y, int s) {
    int w = s - 4, h = s - 2;
    gfx_fill_round_rect(t, x + 2, y + 1, w, h, 2, 0xFFF7FAF7);
    gfx_fill_rect(t, x + 2, y + 1, w, s / 5, 0xFF2E9B57);           /* header */
    for (int i = 1; i <= 2; i++) {                                   /* grid   */
        gfx_fill_rect(t, x + 2, y + 1 + s / 5 + i * (h - s / 5) / 3, w, 1, 0xFFB9CCC0);
        gfx_fill_rect(t, x + 2 + i * w / 3, y + 1 + s / 5, 1, h - s / 5, 0xFFB9CCC0);
    }
    gfx_draw_round_rect(t, x + 2, y + 1, w, h, 2, 0xFF52796A);
}

static void ic_users(draw_target_t *t, int x, int y, int s) {
    int cx = x + s / 2;
    int hr = s * 9 / 32; if (hr < 3) hr = 3;
    gfx_fill_circle(t, cx, y + hr + 1, hr, 0xFF4A76AC);                 /* head */
    gfx_fill_circle(t, cx - hr / 3, y + hr - hr / 4, hr / 3, 0x5CFFFFFF);
    /* shoulders: wide round rect clipped by the box */
    gfx_fill_round_rect(t, x + s / 10, y + s * 9 / 16, s * 4 / 5, s * 7 / 16,
                        s / 4, 0xFF39628F);
    gfx_blend_round_rect(t, x + s / 10 + 1, y + s * 9 / 16 + 1, s * 4 / 5 - 2,
                         s / 5, s / 5, 0x46FFFFFF);
}

static void ic_devmgr(draw_target_t *t, int x, int y, int s) {
    int b = s * 5 / 8;
    int bx = x + (s - b) / 2, by = y + (s - b) / 2;
    /* pins */
    for (int i = 0; i < 3; i++) {
        int o = b / 6 + i * b / 3;
        gfx_fill_rect(t, bx + o, y + 1,         2, (s - b) / 2, 0xFF8E99A8);
        gfx_fill_rect(t, bx + o, by + b,        2, (s - b) / 2, 0xFF8E99A8);
        gfx_fill_rect(t, x + 1,  by + o,        (s - b) / 2, 2, 0xFF8E99A8);
        gfx_fill_rect(t, bx + b, by + o,        (s - b) / 2, 2, 0xFF8E99A8);
    }
    gfx_fill_round_rect(t, bx, by, b, b, 2, 0xFF3C4C60);
    gfx_fill_round_rect(t, bx + b / 4, by + b / 4, b / 2, b / 2, 1, 0xFF77E0A0);
    gfx_blend_rect(t, bx + 1, by + 1, b - 2, b / 3, 0x30FFFFFF);
}

static void ic_store(draw_target_t *t, int x, int y, int s) {
    /* shopping bag with handle */
    ring(t, x + s / 2, y + s * 5 / 16, (s * 3 / 16) * 256, s * 16, 0xFF1F7A3D, 8 | 1);
    gfx_fill_round_rect(t, x + s / 6, y + s * 5 / 16, s * 2 / 3, s * 5 / 8,
                        s / 8, 0xFF32A852);
    gfx_blend_round_rect(t, x + s / 6 + 1, y + s * 5 / 16 + 1, s * 2 / 3 - 2,
                         s / 4, s / 8, 0x48FFFFFF);
    gfx_fill_circle(t, x + s * 5 / 14, y + s * 7 / 16, 1, 0xFF1F5A2D);
    gfx_fill_circle(t, x + s - s * 5 / 14, y + s * 7 / 16, 1, 0xFF1F5A2D);
}

static void ic_music(draw_target_t *t, int x, int y, int s) {
    int r = s * 3 / 16;
    int x1 = x + s / 4, y1 = y + s - r - 2;          /* left head  */
    int x2 = x + s * 3 / 4 - 1, y2 = y + s - r - 4;  /* right head */
    gfx_fill_circle(t, x1, y1, r, 0xFF8A46C8);
    gfx_fill_circle(t, x2, y2, r, 0xFF8A46C8);
    st(t, x1 + r - 1, y1 - 1, x1 + r - 1, y + 3, s * 14, 0xFF7A36B8);
    st(t, x2 + r - 1, y2 - 1, x2 + r - 1, y + 2, s * 14, 0xFF7A36B8);
    st(t, x1 + r - 1, y + 4, x2 + r - 1, y + 2, s * 24, 0xFF9A56D8);  /* beam */
}

static void ic_installer(draw_target_t *t, int x, int y, int s) {
    int cx = x + s * 7 / 16, cy = y + s * 7 / 16, r = s * 6 / 16;
    gfx_fill_circle(t, cx, cy, r, 0xFFC7D6E8);                     /* disc   */
    ring(t, cx, cy, r * 256, s * 14, 0xFF7B8DA5, 0);
    gfx_fill_circle(t, cx, cy, r / 3, 0xFFEFF4FA);                 /* hub    */
    gfx_fill_circle(t, cx, cy, 1, 0xFF7B8DA5);
    /* green down arrow, bottom right */
    int ax = x + s * 11 / 16, aw = s / 4;
    st(t, ax + aw / 2, y + s * 7 / 16, ax + aw / 2, y + s - 5, s * 22, 0xFF2FA84F);
    tri(t, (ax - 1) * 16,       (y + s - 7) * 16,
           (ax + aw + 1) * 16,  (y + s - 7) * 16,
           (ax + aw / 2) * 16,  (y + s - 1) * 16, 0xFF2FA84F);
}

static void ic_displays(draw_target_t *t, int x, int y, int s) {
    int mw = s - 2, mh = s * 9 / 16;
    int mx = x + 1, my = y + s / 8;
    gfx_fill_round_rect(t, mx, my, mw, mh, 2, 0xFF31517A);
    /* screen: little sky gradient */
    gfx_fill_rect(t, mx + 2, my + 2, mw - 4, mh - 4, 0xFF9CC8F0);
    gfx_fill_rect(t, mx + 2, my + 2, mw - 4, (mh - 4) / 2, 0xFF7FB4E8);
    gfx_blend_rect(t, mx + 2, my + 2, (mw - 4) / 3, mh - 4, 0x40FFFFFF);
    gfx_fill_rect(t, x + s / 2 - 1, my + mh, 3, s / 7, 0xFF41618A);
    gfx_fill_round_rect(t, x + s / 4, y + s - 3, s / 2, 2, 1, 0xFF41618A);
}

static void ic_wifi(draw_target_t *t, int x, int y, int s) {
    int cx = x + s / 2, cy = y + s - 4;
    gfx_fill_circle(t, cx, cy, s / 8 > 1 ? s / 8 : 1, 0xFF2F7FD4);
    ring(t, cx, cy, (s * 5 / 16) * 256, s * 22, 0xFF3F8FE0, 8 | 1);
    ring(t, cx, cy, (s * 8 / 16) * 256, s * 22, 0xFF4F9BE6, 8 | 1);
}

static void ic_bluetooth(draw_target_t *t, int x, int y, int s) {
    gfx_fill_round_rect(t, x + s / 6, y + 1, s * 2 / 3, s - 2, s / 5, 0xFF2455C8);
    gfx_blend_round_rect(t, x + s / 6 + 1, y + 2, s * 2 / 3 - 2, s / 3, s / 5,
                         0x46FFFFFF);
    int cx = x + s / 2;
    int top = y + 3, bot = y + s - 4, mid = y + s / 2;
    int wing = s / 5;
    st(t, cx, top, cx, bot, s * 12, 0xFFFFFFFF);                /* stem      */
    st(t, cx, top, cx + wing, top + wing, s * 12, 0xFFFFFFFF);  /* top hook  */
    st(t, cx + wing, top + wing, cx - wing, mid + wing / 2, s * 12, 0xFFFFFFFF);
    st(t, cx, bot, cx + wing, bot - wing, s * 12, 0xFFFFFFFF);  /* bottom    */
    st(t, cx + wing, bot - wing, cx - wing, mid - wing / 2, s * 12, 0xFFFFFFFF);
}

static void ic_trash(draw_target_t *t, int x, int y, int s) {
    int bw = s * 9 / 16, bx = x + (s - bw) / 2;
    gfx_fill_round_rect(t, bx, y + s / 4, bw, s * 11 / 16, 2, 0xFF74818F);
    gfx_blend_rect(t, bx + 1, y + s / 4 + 1, bw - 2, s / 6, 0x40FFFFFF);
    for (int i = 1; i <= 2; i++)                                   /* ridges */
        gfx_fill_rect(t, bx + i * bw / 3, y + s / 4 + 3, 1, s * 11 / 16 - 6,
                      0xFF55616E);
    gfx_fill_round_rect(t, bx - 2, y + s / 6, bw + 4, 3, 1, 0xFF55616E); /* lid */
    gfx_fill_round_rect(t, x + s / 2 - s / 8, y + s / 9, s / 4, 3, 1, 0xFF55616E);
}

static void ic_power(draw_target_t *t, int x, int y, int s) {
    int cx = x + s / 2, cy = y + s / 2 + 1;
    /* ring with a gap at the top (NE+SE+SW+NW minus top wedge ≈ leave out
     * nothing structural — draw full ring, the stem overlaps the top) */
    ring(t, cx, cy, (s * 6 / 16) * 256, s * 18, 0xFFF2F6FB, 1 | 2 | 4);
    ring(t, cx, cy, (s * 6 / 16) * 256, s * 18, 0x90F2F6FB, 8);
    st(t, cx, y + 2, cx, cy - 1, s * 18, 0xFFF2F6FB);
}

static void ic_restart(draw_target_t *t, int x, int y, int s) {
    int cx = x + s / 2, cy = y + s / 2;
    int r = s * 6 / 16;
    ring(t, cx, cy, r * 256, s * 18, 0xFFF2F6FB, 1 | 2 | 4);   /* 3/4 ring */
    /* arrowhead at the NW opening, pointing clockwise */
    int ax = cx - r, ay = cy - 2;
    tri(t, (ax - 3) * 16, (ay + 1) * 16,
           (ax + 4) * 16, (ay + 1) * 16,
           (ax) * 16 + 8, (ay - 6) * 16, 0xFFF2F6FB);
}

static void ic_doom(draw_target_t *t, int x, int y, int s) {
    /* dark plaque + the classic slanted DOOM 'skull-ish' mark: red gradient */
    gfx_fill_round_rect(t, x + 1, y + 1, s - 2, s - 2, s / 5, 0xFF3A2020);
    gfx_blend_round_rect(t, x + 2, y + 2, s - 4, s / 3, s / 5, 0x30FFFFFF);
    int cx = x + s / 2;
    gfx_fill_circle(t, cx, y + s / 2 - 1, s * 5 / 16, 0xFFE8E2D2);   /* skull */
    gfx_fill_rect(t, cx - s / 6, y + s / 2, s / 3, s / 4, 0xFFE8E2D2);
    gfx_fill_circle(t, cx - s / 8, y + s / 2 - 2, s / 10 > 1 ? s / 10 : 1, 0xFF7A1010);
    gfx_fill_circle(t, cx + s / 8, y + s / 2 - 2, s / 10 > 1 ? s / 10 : 1, 0xFF7A1010);
    gfx_fill_rect(t, cx - 1, y + s / 2 + 1, 2, s / 8, 0xFF9A9482);   /* nose  */
    gfx_draw_round_rect(t, x + 1, y + 1, s - 2, s - 2, s / 5, 0xFF802020);
}

static void ic_programs(draw_target_t *t, int x, int y, int s) {
    /* Four rounded app tiles in a 2x2 grid (a "programs" launcher). */
    static const uint32_t col[4] = {
        0xFF3E78C8, 0xFF32A852, 0xFFE0902E, 0xFF8A46C8
    };
    int g = s / 12; if (g < 1) g = 1;
    int tw = (s - 3 * g) / 2;
    for (int i = 0; i < 4; i++) {
        int cx = x + g + (i & 1) * (tw + g);
        int cy = y + g + (i >> 1) * (tw + g);
        gfx_fill_round_rect(t, cx, cy, tw, tw, tw / 4, col[i]);
        gfx_blend_round_rect(t, cx + 1, cy + 1, tw - 2, tw / 2, tw / 4, 0x40FFFFFF);
    }
}

/* ---------- public API --------------------------------------------------- */

void icon_draw(draw_target_t *t, int x, int y, int sz, icon_id_t id) {
    if (!t || sz < 10) return;
    switch (id) {
        case ICON_TASKMGR:   ic_taskmgr(t, x, y, sz);   break;
        case ICON_SETTINGS:  ic_settings(t, x, y, sz);  break;
        case ICON_EXPLORER:  ic_explorer(t, x, y, sz);  break;
        case ICON_BROWSER:   ic_browser(t, x, y, sz);   break;
        case ICON_EDITOR:    ic_editor(t, x, y, sz);    break;
        case ICON_NEXSHEET:  ic_nexsheet(t, x, y, sz);  break;
        case ICON_USERS:     ic_users(t, x, y, sz);     break;
        case ICON_DEVMGR:    ic_devmgr(t, x, y, sz);    break;
        case ICON_STORE:     ic_store(t, x, y, sz);     break;
        case ICON_MUSIC:     ic_music(t, x, y, sz);     break;
        case ICON_INSTALLER: ic_installer(t, x, y, sz); break;
        case ICON_DISPLAYS:  ic_displays(t, x, y, sz);  break;
        case ICON_WIFI:      ic_wifi(t, x, y, sz);      break;
        case ICON_BLUETOOTH: ic_bluetooth(t, x, y, sz); break;
        case ICON_TRASH:     ic_trash(t, x, y, sz);     break;
        case ICON_POWER:     ic_power(t, x, y, sz);     break;
        case ICON_RESTART:   ic_restart(t, x, y, sz);   break;
        case ICON_DOOM:      ic_doom(t, x, y, sz);      break;
        case ICON_PROGRAMS:  ic_programs(t, x, y, sz);  break;
        default: break;
    }
}

icon_id_t icon_for_cmd(const char *cmd) {
    if (!cmd) return ICON_NONE;
    while (*cmd == ' ') cmd++;
    static const struct { const char *k; icon_id_t id; } map[] = {
        { "taskmgr",     ICON_TASKMGR   },
        { "settings",    ICON_SETTINGS  },
        { "explorer",    ICON_EXPLORER  },
        { "browser",     ICON_BROWSER   },
        { "editor",      ICON_EDITOR    },
        { "nexsheet",    ICON_NEXSHEET  },
        { "usermgr",     ICON_USERS     },
        { "devmgr",      ICON_DEVMGR    },
        { "nexstore",    ICON_STORE     },
        { "audioplayer", ICON_MUSIC     },
        { "installer",   ICON_INSTALLER },
        { "displays",    ICON_DISPLAYS  },
        { "wifi",        ICON_WIFI      },
        { "bluetooth",   ICON_BLUETOOTH },
        { "trash",       ICON_TRASH     },
        { "doom",        ICON_DOOM      },
    };
    for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
        const char *k = map[i].k, *c = cmd;
        while (*k && *c && *c != ' ' && *k == *c) { k++; c++; }
        if (*k == 0 && (*c == 0 || *c == ' ')) return map[i].id;
    }
    return ICON_NONE;
}
