/* ============================================================================
 * NexxoN OS - Image Viewer  (v1.0)
 * ----------------------------------------------------------------------------
 * Displays BMP, PNG, and JPEG images from the NXFS filesystem.
 * Toolbar: Open | Zoom+ | Zoom- | Fit | 1:1
 * Zoom is nearest-neighbour; Fit scales to the content area while
 * preserving the aspect ratio.
 * ============================================================================ */
#include "imgview.h"
#include "window.h"
#include "gfx.h"
#include "font.h"
#include "i18n.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "keyboard.h"
#include "nxfs.h"
#include "vfs.h"
#include "dialogs.h"
#include "theme.h"
#include "bmp.h"
#include "png.h"
#include "jpeg.h"

/* ---- Layout ------------------------------------------------------------ */
#define IV_WIN_W    640
#define IV_WIN_H    520
#define IV_WIN_X    80
#define IV_WIN_Y    40

#define IV_TOOLBAR_H  32
#define IV_BTN_W      60
#define IV_BTN_H      22
#define IV_BTN_PAD     6

/* ---- Colours ----------------------------------------------------------- */
#define IV_BG          0xFF1E1E2A
#define IV_TOOLBAR_BG  0xFF2C2C3C
#define IV_BTN_BG      0xFF3C3C50
#define IV_BTN_HOT     0xFF5060A0
#define IV_BTN_FG      0xFFE0E0F0
#define IV_TEXT_FG     0xFFCCCCDD
#define IV_PANEL_BG    0xFF141420

/* ---- Pixel buffer ------------------------------------------------------ */
/* 1024 x 768 x 4 bytes = 3 MiB — covers all common image sizes. */
#define IV_MAX_PIXELS  (1024u * 1024u * 4u)
static uint8_t g_pixels[IV_MAX_PIXELS];

/* ---- Viewer state ------------------------------------------------------ */
static struct {
    window_t *win;
    uint32_t  img_w;
    uint32_t  img_h;
    bool      has_image;
    char      filename[64];
    /* Scale as 8-bit fixed: scale_num/scale_den.
     * 1x = 256/256, 2x = 512/256, 0.5x = 128/256. */
    int       zoom_num;   /* multiplier * 256 */
    bool      fit_mode;   /* true = auto-fit to content area */
    int       scroll_x;
    int       scroll_y;
} g_iv;

static bool stale(void) {
    if (!g_iv.win) return true;
    if (!g_iv.win->in_use) { g_iv.win = NULL; return true; }
    return false;
}

/* ---- Nearest-neighbour scaled blit ------------------------------------ */
static void blit_image_scaled(draw_target_t *dst,
                               int dx, int dy, int dw, int dh,
                               const uint8_t *src_pixels,
                               uint32_t sw, uint32_t sh) {
    if (dw <= 0 || dh <= 0 || sw == 0 || sh == 0) return;
    for (int row = 0; row < dh; row++) {
        int sy = (int)(((uint32_t)row * (sh << 8)) / (uint32_t)dh) >> 8;
        if (sy < 0) sy = 0;
        if ((uint32_t)sy >= sh) sy = (int)sh - 1;
        for (int col = 0; col < dw; col++) {
            int sx = (int)(((uint32_t)col * (sw << 8)) / (uint32_t)dw) >> 8;
            if (sx < 0) sx = 0;
            if ((uint32_t)sx >= sw) sx = (int)sw - 1;
            uint32_t px = *(const uint32_t *)(src_pixels +
                           ((uint32_t)sy * sw + (uint32_t)sx) * 4);
            int ox = dx + col;
            int oy = dy + row;
            if (ox >= 0 && oy >= 0 &&
                (uint32_t)ox < dst->width && (uint32_t)oy < dst->height) {
                *(uint32_t *)(dst->fb + (uint32_t)oy * dst->pitch +
                              (uint32_t)ox * 4) = px | 0xFF000000u;
            }
        }
    }
}

/* ---- Compute draw rect preserving aspect ratio ------------------------- */
static void fit_rect(int area_w, int area_h,
                     uint32_t img_w, uint32_t img_h,
                     int *out_x, int *out_y,
                     int *out_w, int *out_h) {
    if (img_w == 0 || img_h == 0) { *out_w = *out_h = 0; return; }
    int dw = area_w;
    int dh = (int)((uint32_t)area_w * img_h / img_w);
    if (dh > area_h) {
        dh = area_h;
        dw = (int)((uint32_t)area_h * img_w / img_h);
    }
    *out_x = (area_w  - dw) / 2;
    *out_y = (area_h  - dh) / 2;
    *out_w = dw;
    *out_h = dh;
}

/* ---- Redraw ------------------------------------------------------------ */
static void redraw(void) {
    if (stale()) return;
    draw_target_t *t = &g_iv.win->content;
    int cw = (int)t->width;
    int ch = (int)t->height;

    gfx_clear(t, IV_BG);

    /* Toolbar */
    gfx_fill_rect(t, 0, 0, cw, IV_TOOLBAR_H, IV_TOOLBAR_BG);
    gfx_draw_hline(t, 0, IV_TOOLBAR_H - 1, cw, 0xFF404060);

    struct { const char *label; bool active; } btns[] = {
        { L(STR_IV_OPEN),     true               },
        { L(STR_IV_ZOOM_IN),  g_iv.has_image     },
        { L(STR_IV_ZOOM_OUT), g_iv.has_image     },
        { L(STR_IV_FIT),      g_iv.has_image     },
        { L(STR_IV_ACTUAL),   g_iv.has_image     },
    };
    int bx = IV_BTN_PAD;
    for (int i = 0; i < 5; i++) {
        uint32_t bg = btns[i].active ? IV_BTN_BG : 0xFF282830;
        gfx_fill_rect(t, bx, (IV_TOOLBAR_H - IV_BTN_H) / 2,
                      IV_BTN_W, IV_BTN_H, bg);
        gfx_draw_rect(t, bx, (IV_TOOLBAR_H - IV_BTN_H) / 2,
                      IV_BTN_W, IV_BTN_H, 0xFF505068);
        int tw = (int)strlen(btns[i].label) * FONT_GLYPH_W;
        int tx = bx + (IV_BTN_W - tw) / 2;
        int ty = (IV_TOOLBAR_H - FONT_GLYPH_H) / 2;
        gfx_draw_string(t, tx, ty, btns[i].label, IV_BTN_FG,
                        btns[i].active ? bg : 0xFF282830);
        bx += IV_BTN_W + IV_BTN_PAD;
    }

    /* Filename label */
    if (g_iv.has_image && g_iv.filename[0]) {
        char info[96];
        ksnprintf(info, sizeof(info), "%s  (%ux%u)",
                  g_iv.filename, g_iv.img_w, g_iv.img_h);
        gfx_draw_string_clipped(t, bx + 8,
                                (IV_TOOLBAR_H - FONT_GLYPH_H) / 2,
                                cw - bx - 16, info, IV_TEXT_FG, IV_TOOLBAR_BG);
    }

    /* Image area */
    int area_y = IV_TOOLBAR_H + 1;
    int area_h = ch - area_y;
    int area_w = cw;

    gfx_fill_rect(t, 0, area_y, area_w, area_h, IV_PANEL_BG);

    if (!g_iv.has_image) {
        const char *msg = L(STR_IV_NO_FILE);
        int mx = (area_w - (int)strlen(msg) * FONT_GLYPH_W) / 2;
        int my = area_y + (area_h - FONT_GLYPH_H) / 2;
        gfx_draw_string(t, mx, my, msg, 0xFF888899, IV_PANEL_BG);
    } else {
        int dx, dy, dw, dh;
        if (g_iv.fit_mode) {
            fit_rect(area_w, area_h, g_iv.img_w, g_iv.img_h,
                     &dx, &dy, &dw, &dh);
            dy += area_y;
        } else {
            dw = (int)(g_iv.img_w * (uint32_t)g_iv.zoom_num / 256u);
            dh = (int)(g_iv.img_h * (uint32_t)g_iv.zoom_num / 256u);
            dx = (area_w - dw) / 2 + g_iv.scroll_x;
            dy = area_y + (area_h - dh) / 2 + g_iv.scroll_y;
        }
        blit_image_scaled(t, dx, dy, dw, dh,
                          g_pixels, g_iv.img_w, g_iv.img_h);
    }

    wm_mark_dirty();
}

/* ---- File loading ------------------------------------------------------ */
static bool detect_and_decode(const uint8_t *src, uint32_t len) {
    if (len < 4) return false;

    /* BMP */
    if (src[0] == 'B' && src[1] == 'M') {
        bmp_image_t img;
        memset(&img, 0, sizeof(img));
        if (!bmp_decode(src, len, &img)) return false;
        uint32_t need = img.width * img.height * 4;
        if (need > IV_MAX_PIXELS) { bmp_free(&img); return false; }
        memcpy(g_pixels, img.pixels, need);
        g_iv.img_w = img.width;
        g_iv.img_h = img.height;
        bmp_free(&img);
        return true;
    }

    /* PNG */
    if (src[0] == 0x89 && src[1] == 'P' && src[2] == 'N' && src[3] == 'G') {
        png_info_t info;
        memset(&info, 0, sizeof(info));
        /* Peek dimensions via a throwaway decode first to check size. */
        if (!png_decode(src, len, g_pixels, IV_MAX_PIXELS, &info))
            return false;
        g_iv.img_w = info.width;
        g_iv.img_h = info.height;
        return true;
    }

    /* JPEG */
    if (src[0] == 0xFF && src[1] == 0xD8) {
        jpeg_info_t info;
        memset(&info, 0, sizeof(info));
        if (!jpeg_decode(src, len, g_pixels, IV_MAX_PIXELS, &info))
            return false;
        g_iv.img_w = info.width;
        g_iv.img_h = info.height;
        return true;
    }

    return false;
}

/* Resolve an NXFS path and load the image into g_pixels. */
#define IV_FILE_BUF  (512u * 1024u)   /* 512 KiB read buffer */
static uint8_t g_file_buf[IV_FILE_BUF];

static bool load_image(const char *path) {
    if (!path || !path[0]) return false;

    /* Mounted-volume paths ("/usb0/...") read through the VFS so images on
     * a pendrive open directly — no staging copy into NXFS. */
    uint32_t bytes_read = 0;
    {
        const char *q = path;
        while (*q == '/') q++;
        char first[VFS_NAME_MAX];
        int fi = 0;
        while (q[fi] && q[fi] != '/' && fi < VFS_NAME_MAX - 1) {
            first[fi] = q[fi]; fi++;
        }
        first[fi] = 0;
        if (vfs_find_mount(first)) {
            int got = vfs_read(path, g_file_buf, IV_FILE_BUF);
            if (got <= 0) return false;
            bytes_read = (uint32_t)got;
        }
    }

    if (bytes_read == 0) {
        uint32_t inode = 1;
        char tmp[256];
        int n = 0;
        const char *p = path;
        while (*p == '/') p++;

        while (*p) {
            if (*p == '/' || *(p + 1) == '\0') {
                if (*(p + 1) == '\0' && *p != '/') tmp[n++] = *p;
                tmp[n] = '\0';
                if (n > 0) {
                    uint32_t child = 0;
                    if (nxfs_resolve(inode, tmp, &child) != 0) return false;
                    inode = child;
                }
                n = 0;
            } else {
                if (n < (int)sizeof(tmp) - 1) tmp[n++] = *p;
            }
            p++;
        }

        if (nxfs_read_file(inode, g_file_buf, IV_FILE_BUF, &bytes_read) != 0)
            return false;
        if (bytes_read == 0) return false;
    }

    if (!detect_and_decode(g_file_buf, bytes_read)) return false;

    /* Capture basename. */
    const char *slash = path;
    const char *q = path;
    while (*q) { if (*q == '/') slash = q + 1; q++; }
    int fn = 0;
    while (*slash && fn < (int)sizeof(g_iv.filename) - 1)
        g_iv.filename[fn++] = *slash++;
    g_iv.filename[fn] = '\0';

    g_iv.has_image = true;
    g_iv.fit_mode  = true;
    g_iv.zoom_num  = 256;
    g_iv.scroll_x  = 0;
    g_iv.scroll_y  = 0;
    debug_printf("[imgview] loaded '%s' %ux%u\n",
                 g_iv.filename, g_iv.img_w, g_iv.img_h);
    return true;
}

/* ---- Button hit testing ----------------------------------------------- */
static int toolbar_btn_hit(int mx, int my) {
    if (my < (IV_TOOLBAR_H - IV_BTN_H) / 2 ||
        my >= (IV_TOOLBAR_H - IV_BTN_H) / 2 + IV_BTN_H) return -1;
    int bx = IV_BTN_PAD;
    for (int i = 0; i < 5; i++) {
        if (mx >= bx && mx < bx + IV_BTN_W) return i;
        bx += IV_BTN_W + IV_BTN_PAD;
    }
    return -1;
}

static bool on_click(window_t *w, int mx, int my,
                     uint8_t pressed, uint8_t btn) {
    (void)w; (void)btn;
    if (!pressed) return false;

    int b = toolbar_btn_hit(mx, my);
    if (b < 0) return false;

    switch (b) {
    case 0: { /* Open */
        char path[256] = "";
        if (!dialog_input(L(STR_IV_OPEN), "/images/photo.bmp", "", path, sizeof(path)))
            return true;
        if (!load_image(path)) {
            const char *lines[] = { L(STR_IV_LOAD_ERR), path };
            dialog_info(L(STR_APP_IMGVIEW), lines, 2);
        }
        redraw();
        return true;
    }
    case 1: /* Zoom+ */
        if (g_iv.has_image) {
            g_iv.fit_mode = false;
            g_iv.zoom_num = g_iv.zoom_num * 3 / 2;
            if (g_iv.zoom_num > 256 * 8) g_iv.zoom_num = 256 * 8;
            redraw();
        }
        return true;
    case 2: /* Zoom- */
        if (g_iv.has_image) {
            g_iv.fit_mode = false;
            g_iv.zoom_num = g_iv.zoom_num * 2 / 3;
            if (g_iv.zoom_num < 16) g_iv.zoom_num = 16;
            redraw();
        }
        return true;
    case 3: /* Fit */
        if (g_iv.has_image) {
            g_iv.fit_mode  = true;
            g_iv.scroll_x  = 0;
            g_iv.scroll_y  = 0;
            redraw();
        }
        return true;
    case 4: /* 1:1 */
        if (g_iv.has_image) {
            g_iv.fit_mode = false;
            g_iv.zoom_num = 256;
            g_iv.scroll_x = 0;
            g_iv.scroll_y = 0;
            redraw();
        }
        return true;
    }
    return false;
}

static void on_destroy(window_t *w) {
    (void)w;
    g_iv.win = NULL;
}

static void on_lang_change(lang_t l) {
    (void)l;
    redraw();
}

/* WM resized us (incl. maximize / restore): re-layout + re-centre the image
 * into the new content area.  redraw() reads the live content dimensions. */
static void iv_resize_cb(window_t *w) {
    if (w != g_iv.win || stale()) return;
    redraw();
}

/* ---- Public API -------------------------------------------------------- */
bool imgview_open(void) {
    if (!stale()) {
        wm_set_focus(g_iv.win);
        wm_mark_dirty();
        return true;
    }

    memset(&g_iv, 0, sizeof(g_iv));
    g_iv.zoom_num = 256;
    g_iv.fit_mode = true;

    window_t *w = wm_create_window(IV_WIN_X, IV_WIN_Y,
                                   IV_WIN_W, IV_WIN_H,
                                   L(STR_APP_IMGVIEW));
    if (!w) return false;
    g_iv.win = w;

    wm_set_content_click(w, on_click, NULL);
    wm_set_destroy_cb(w, on_destroy);
    wm_set_resize_cb(w, iv_resize_cb);
    lang_register_cb(on_lang_change);

    redraw();
    wm_set_focus(w);
    return true;
}

void imgview_tick(void) {
    /* Nothing to do; the viewer is fully event-driven. */
    (void)stale();
}

void imgview_open_file(const char *path) {
    if (!imgview_open()) return;
    if (stale()) return;
    if (load_image(path)) redraw();
    else {
        const char *lines[] = { L(STR_IV_LOAD_ERR), path };
        dialog_info(L(STR_APP_IMGVIEW), lines, 2);
    }
}
