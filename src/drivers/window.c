/* ============================================================================
 * NexxoN OS - Window manager + compositor  (v6.0)
 * ----------------------------------------------------------------------------
 * Memory layout:
 *
 *      g_fb_pool          6 MiB BSS block, 4 KiB aligned.  Managed as a
 *                         linked list of fb_block_hdr_t blocks; allocations
 *                         walk first-fit, frees coalesce adjacent free
 *                         neighbours so the pool never fragments past the
 *                         point of "one big free block + permanent slots".
 *
 *      g_windows[8]       fixed table of window descriptors.
 *
 *      g_back             screen-sized back buffer; compositor blits here then
 *                         memcpy's to the real VESA FB to eliminate tearing.
 *
 * v6.0 changes over v5.0:
 *   - Rewrote the framebuffer pool as a linked-list allocator with
 *     coalescing.  The old bump+freelist combo leaked when dialogs of
 *     varying sizes were opened/closed repeatedly (BUG 6).
 *   - Added window resize support via a bottom-right grip handle (BUG 5).
 *     Resizable windows fire an optional resize callback after each drag
 *     step so the client repaints into the freshly-resized framebuffer.
 *   - UI facelift (BUG 7): gradient title bars, sharper 3D bevels on
 *     window chrome and close button.
 * ============================================================================ */
#include "window.h"
#include "gfx.h"
#include "theme.h"   /* GLASS_BLUR_R + glass tokens for frosted chrome */
#include "font.h"
#include "vga.h"
#include "mouse.h"
#include "string.h"
#include "debug.h"
#include "pit.h"
#include "desktop.h"
#include "ctxmenu.h"
#include "keyboard.h"
#include "tray.h"

/* ---------- Colour palette (Windows 7 Aero Glass) ----------------------- *
 *
 * True software alpha-blending:
 *   FinalPixel = (SrcRGB * Alpha + DstRGB * (255-Alpha)) / 255
 *
 * Border (8 px L/R/Bottom): #7096BA alpha 160 — glass tint over desktop.
 * Title bar (30 px): frosted blue gradient (focused) / grey (unfocused).
 * Title text: BLACK (#000000) with a 1 px white shadow — spec requirement.
 * Close button: gradient #ECA59E → #C44B43, border #8A1E18, white X.
 */
#define DESKTOP_BG          0xFF1A2840u
/* Glass border — #7096BA at alpha 160 (0xA0) */
#define AERO_BORDER         0xA07096BAu
/* Outer edge: 1 px white at alpha 100 */
#define AERO_OUTER_LINE     0x64FFFFFFu
/* Inner edge: 1 px white at alpha 150 (where border meets content) */
#define AERO_INNER_LINE     0x96FFFFFFu
/* Focused title-bar: light → mid → deep blue (frosted glass).
 * Alpha gradient (220 → 180 → 150) so the underlying #7096BA glass
 * border shows through and the title strip stays translucent rather
 * than reading as a solid painted bar. */
#define TITLE_GRAD_TOP_F    0xDCE4F0FCu
#define TITLE_GRAD_MID_F    0xB4A8CCEAu
#define TITLE_GRAD_BOT_F    0x966498C8u
/* Unfocused title-bar: cool greys with the same translucency profile */
#define TITLE_GRAD_TOP_U    0xC8D4DAE0u
#define TITLE_GRAD_BOT_U    0x789298A0u
/* 1 px gloss line just under the top edge of the title bar */
#define TITLE_HIGHLIGHT     0x60FFFFFFu
/* Title text: spec mandates BLACK with WHITE shadow for readability */
#define TITLE_FG            0xFF000000u
#define TITLE_SHADOW_C      0xFFFFFFFFu
/* Close button: gradient #ECA59E → #C44B43, hot state brighter */
#define CLOSE_GRAD_TOP      0xFFECA59Eu
#define CLOSE_GRAD_BOT      0xFFC44B43u
#define CLOSE_GRAD_TOP_HOT  0xFFFF9898u
#define CLOSE_GRAD_BOT_HOT  0xFFD83030u
#define CLOSE_BORDER_C      0xFF8A1E18u
#define CLOSE_FG_C          0xFFFFFFFFu
/* Minimize button: neutral glass tones */
#define MIN_GRAD_TOP        0xFFECF2FAu
#define MIN_GRAD_BOT        0xFFB4C4D8u
#define MIN_GRAD_TOP_HOT    0xFFD8E8FFu
#define MIN_GRAD_BOT_HOT    0xFF8098C8u
#define MIN_BORDER_C        0xFF6888A8u
#define MIN_FG_C            0xFF202830u
#define GRIP_FG             0xFF6080A0u
#define GRIP_HI             0xFFFFFFFFu

/* Max composites per second.  At 60 fps each frame is ~16 ms; at 1024x768x4
 * that's one 3 MiB memcpy per frame which is well within budget. */
#define WM_MAX_FPS          60u

/* ---------- Framebuffer pool -------------------------------------------- */
/* 6 MiB hosts: console (~2 MB) + editor (~2.3 MB) + taskmgr (~0.35 MB) +
 * up to ~3 transient dialog windows simultaneously.  The new linked-list
 * allocator coalesces on free so the same pool now handles unlimited
 * open/close cycles without fragmentation. */
#define FB_POOL_BYTES   (6u * 1024u * 1024u)
ALIGNED(4096) static uint8_t g_fb_pool[FB_POOL_BYTES];

/* Each block in the pool is prefixed by a 16-byte header.  size excludes
 * the header itself; in_use is 1 (allocated) or 0 (free); next walks the
 * pool in physical order (NULL terminates the list).  _reserved keeps the
 * payload that follows 16-byte aligned so SSE/2-pixel rect fills stay
 * happy on i386 (we don't enable SSE in kernel but the alignment doesn't
 * cost anything). */
typedef struct fb_block_hdr {
    uint32_t            size;
    uint32_t            in_use;
    struct fb_block_hdr *next;
    uint32_t            _reserved;
} fb_block_hdr_t;

#define FB_HDR_SZ ((uint32_t)sizeof(fb_block_hdr_t))

static fb_block_hdr_t *g_pool_head = NULL;

static void pool_init(void) {
    g_pool_head = (fb_block_hdr_t *)g_fb_pool;
    g_pool_head->size      = FB_POOL_BYTES - FB_HDR_SZ;
    g_pool_head->in_use    = 0;
    g_pool_head->next      = NULL;
    g_pool_head->_reserved = 0;
}

/* Walk the entire pool looking for adjacent free pairs and merge them.
 * Restart from head every time we merge so we catch chains of 3+ free
 * blocks in a single pass. */
static void pool_coalesce(void) {
    bool merged;
    do {
        merged = false;
        for (fb_block_hdr_t *p = g_pool_head; p && p->next; p = p->next) {
            if (!p->in_use && !p->next->in_use) {
                p->size += FB_HDR_SZ + p->next->size;
                p->next  = p->next->next;
                merged   = true;
                break;
            }
        }
    } while (merged);
}

static uint8_t *pool_alloc(uint32_t bytes) {
    if (bytes == 0) return NULL;
    uint32_t need = (bytes + 15u) & ~15u;
    for (fb_block_hdr_t *b = g_pool_head; b; b = b->next) {
        if (b->in_use || b->size < need) continue;
        /* Split if the leftover would be worth keeping (header + 256 B). */
        if (b->size >= need + FB_HDR_SZ + 256u) {
            fb_block_hdr_t *nb = (fb_block_hdr_t *)((uint8_t *)b + FB_HDR_SZ + need);
            nb->size      = b->size - need - FB_HDR_SZ;
            nb->in_use    = 0;
            nb->next      = b->next;
            nb->_reserved = 0;
            b->size = need;
            b->next = nb;
        }
        b->in_use = 1;
        return (uint8_t *)b + FB_HDR_SZ;
    }
    debug_printf("[wm] pool_alloc(%u) FAILED - pool exhausted\n", bytes);
    return NULL;
}

static void pool_free(uint8_t *ptr) {
    if (!ptr) return;
    fb_block_hdr_t *b = (fb_block_hdr_t *)(ptr - FB_HDR_SZ);
    b->in_use = 0;
    pool_coalesce();
}

/* Resize a block in place when possible; otherwise allocate a new one,
 * copy the surviving prefix, free the old.  Returns NULL on OOM and
 * leaves the original block intact in that case (the caller can keep
 * using it). */
static uint8_t *pool_realloc(uint8_t *ptr, uint32_t new_bytes) {
    if (!ptr) return pool_alloc(new_bytes);
    if (new_bytes == 0) { pool_free(ptr); return NULL; }

    fb_block_hdr_t *b = (fb_block_hdr_t *)(ptr - FB_HDR_SZ);
    uint32_t need = (new_bytes + 15u) & ~15u;

    /* Shrink in place. */
    if (need <= b->size) {
        if (b->size >= need + FB_HDR_SZ + 256u) {
            fb_block_hdr_t *nb = (fb_block_hdr_t *)((uint8_t *)b + FB_HDR_SZ + need);
            nb->size      = b->size - need - FB_HDR_SZ;
            nb->in_use    = 0;
            nb->next      = b->next;
            nb->_reserved = 0;
            b->size = need;
            b->next = nb;
            pool_coalesce();
        }
        return ptr;
    }

    /* Grow by absorbing next free block. */
    if (b->next && !b->next->in_use &&
        b->size + FB_HDR_SZ + b->next->size >= need) {
        b->size += FB_HDR_SZ + b->next->size;
        b->next  = b->next->next;
        /* Possibly split off the remainder. */
        if (b->size >= need + FB_HDR_SZ + 256u) {
            fb_block_hdr_t *nb = (fb_block_hdr_t *)((uint8_t *)b + FB_HDR_SZ + need);
            nb->size      = b->size - need - FB_HDR_SZ;
            nb->in_use    = 0;
            nb->next      = b->next;
            nb->_reserved = 0;
            b->size = need;
            b->next = nb;
        }
        return ptr;
    }

    /* Worst case: alloc fresh, copy, free old. */
    uint8_t *np = pool_alloc(new_bytes);
    if (!np) return NULL;
    uint32_t cp = (b->size < need) ? b->size : need;
    memcpy(np, ptr, cp);
    pool_free(ptr);
    return np;
}

static void pool_stats(uint32_t *used_out, uint32_t *total_out) {
    uint32_t used = 0;
    for (fb_block_hdr_t *b = g_pool_head; b; b = b->next) {
        if (b->in_use) used += FB_HDR_SZ + b->size;
    }
    if (used_out)  *used_out  = used;
    if (total_out) *total_out = FB_POOL_BYTES;
}

/* ---------- Back buffer (screen-sized, max 1920x1080) -------------------- */
#define BACK_MAX_W 1920u
#define BACK_MAX_H 1080u
ALIGNED(4096) static uint8_t g_back_storage[BACK_MAX_W * BACK_MAX_H * 4];

/* Damage-tracking shadow: a cached-RAM copy of what we last pushed to the
 * hardware framebuffer.  wm_blit_backbuffer() compares g_back to this shadow
 * ROW BY ROW (cheap cached-RAM memcmp) and only copies changed rows out to the
 * LFB.  On bare metal the LFB is an uncached / write-combining MMIO surface
 * where every byte written is expensive, so a full 1024x768x4 = 3 MiB blit per
 * keystroke is the dominant source of input lag.  Typing only changes a few
 * text rows, so this turns a 3 MiB blit into a ~64 KiB one -- a large, glitch-
 * free win independent of whether MTRR write-combining is active. */
ALIGNED(4096) static uint8_t g_shadow_storage[BACK_MAX_W * BACK_MAX_H * 4];
static bool g_shadow_valid = false;
static draw_target_t g_back = {
    .fb = g_back_storage, .pitch = BACK_MAX_W * 4,
    .width = 1024u, .height = 768u,
};

/* ---------- Window table ------------------------------------------------ */
static window_t g_windows[WM_MAX_WINDOWS];
static int      g_next_id   = 1;
static int      g_next_z    = 1;
static window_t *g_focused  = NULL;
static volatile bool g_dirty = false;

/* Shell window pointer.  Set once by the kernel after wm_create_window for
 * the console finishes.  Used by wm_is_shell_focused() so the keyboard ISR
 * can guard Ctrl+C / abort behaviour by focus. */
static window_t *g_shell_window = NULL;

/* ---------- Position memory (ISSUE 9) ----------------------------------- *
 * Remember where each window was when it was closed (keyed by its initial
 * title) and reopen it there.  Lives in RAM only, so the table resets on
 * restart — exactly the requested "remember until reboot" behaviour. */
#define WM_POS_MEM_MAX 24
typedef struct {
    bool used;
    char key[WM_TITLE_MAX];
    int  x, y;
} wm_pos_mem_t;
static wm_pos_mem_t g_pos_mem[WM_POS_MEM_MAX];

static wm_pos_mem_t *pos_mem_find(const char *key) {
    if (!key || !key[0]) return NULL;
    for (int i = 0; i < WM_POS_MEM_MAX; i++)
        if (g_pos_mem[i].used && strcmp(g_pos_mem[i].key, key) == 0)
            return &g_pos_mem[i];
    return NULL;
}
static void pos_mem_save(const char *key, int x, int y) {
    if (!key || !key[0]) return;
    wm_pos_mem_t *m = pos_mem_find(key);
    if (!m) {
        for (int i = 0; i < WM_POS_MEM_MAX; i++)
            if (!g_pos_mem[i].used) { m = &g_pos_mem[i]; break; }
    }
    if (!m) return;                      /* table full — silently skip */
    m->used = true;
    strncpy(m->key, key, WM_TITLE_MAX - 1);
    m->key[WM_TITLE_MAX - 1] = 0;
    m->x = x; m->y = y;
}

/* ---------- Open / close fade animation (ISSUE 13) ---------------------- *
 * Every window fades in on open and fades out on close, driven globally by
 * the compositor so the whole desktop is consistent.  Two window-sized
 * scratch buffers hold (a) the background under an opening window so it can
 * be blended back as the window fades up, and (b) a snapshot of a closing
 * window's pixels so it can keep fading after the slot is freed.  Windows
 * larger than the scratch simply appear/disappear instantly (no crash). */
#define WM_ANIM_MS        ANIM_WIN_MS    /* unified liquid timing (theme.h) */
#define WM_ANIM_MAX_W     1024
#define WM_ANIM_MAX_H     800
ALIGNED(4096) static uint8_t g_anim_bg   [WM_ANIM_MAX_W * WM_ANIM_MAX_H * 4];
ALIGNED(4096) static uint8_t g_anim_ghost[WM_ANIM_MAX_W * WM_ANIM_MAX_H * 4];
static struct {
    bool     active;
    int      x, y, w, h;
    uint32_t start_ms;
} g_close_anim;

static bool anim_fits(const window_t *w) {
    return w->w > 0 && w->h > 0 &&
           w->w <= WM_ANIM_MAX_W && w->h <= WM_ANIM_MAX_H;
}

/* Copy a (clipped) rectangle out of a draw target into a tightly-packed
 * w*h ARGB buffer.  Pixels outside the target read back as 0. */
static void anim_snapshot(const draw_target_t *t, int x, int y, int w, int h,
                          uint8_t *buf) {
    for (int row = 0; row < h; row++) {
        uint32_t *dst = (uint32_t *)(buf + (uint32_t)row * (uint32_t)w * 4u);
        int sy = y + row;
        if (sy < 0 || (uint32_t)sy >= t->height) {
            for (int c = 0; c < w; c++) dst[c] = 0;
            continue;
        }
        const uint32_t *src = (const uint32_t *)(t->fb + (uint32_t)sy * t->pitch);
        for (int col = 0; col < w; col++) {
            int sx = x + col;
            dst[col] = (sx < 0 || (uint32_t)sx >= t->width) ? 0 : src[sx];
        }
    }
}

/* Alpha-blend a tightly-packed w*h ARGB buffer onto a draw target at
 * (x,y).  `alpha` is the buffer's opacity (0..255). */
static void anim_blend(draw_target_t *t, int x, int y, int w, int h,
                       const uint8_t *buf, int alpha) {
    if (alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    uint32_t a = (uint32_t)alpha, ia = 255u - a;
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || (uint32_t)dy >= t->height) continue;
        uint32_t *dp = (uint32_t *)(t->fb + (uint32_t)dy * t->pitch);
        const uint32_t *sp = (const uint32_t *)(buf + (uint32_t)row * (uint32_t)w * 4u);
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || (uint32_t)dx >= t->width) continue;
            uint32_t s = sp[col], d = dp[dx];
            uint32_t r = (((s >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia) / 255u;
            uint32_t g = (((s >>  8) & 0xFF) * a + ((d >>  8) & 0xFF) * ia) / 255u;
            uint32_t b = (((s)       & 0xFF) * a + ((d)       & 0xFF) * ia) / 255u;
            dp[dx] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/* ---------- Drag / resize state ----------------------------------------- */
static window_t *g_drag_win    = NULL;
static int       g_drag_off_x  = 0;
static int       g_drag_off_y  = 0;

static window_t *g_resize_win  = NULL;
static int       g_resize_start_w = 0;
static int       g_resize_start_h = 0;
static int       g_resize_start_mx = 0;
static int       g_resize_start_my = 0;

static uint8_t   g_prev_btn    = 0;

/* Title-bar double-click tracking (toggle maximize). */
#define WM_TITLE_DBLCLICK_MS 450u
static window_t *g_title_click_win = NULL;
static uint32_t  g_title_click_ms  = 0;

/* ---------- Dirty-rect compositing ------------------------------------- *
 * A frame recomposes+presents only its damaged box.  Two kinds of damage:
 *   - g_full_dirty  : a change that can touch anything (geometry, focus,
 *                     menu, app content marked via wm_mark_dirty) -> full
 *                     screen.  This is the SAFE DEFAULT.
 *   - region damage : self-contained pixel changes that report their own
 *                     rect (wm_damage_rect / wm_shell_content_damage, e.g.
 *                     the 12.5 Hz idle spinner cell) + the cursor cells.
 * A mis-judged box is at worst a self-healing 1-frame stale patch, never a
 * crash.  Counted on COM1 (see [wm/perf] "partial N/120"). */
static int  g_prev_cur_x = -1, g_prev_cur_y = -1;  /* cursor at last present */
static bool g_full_dirty  = true;     /* non-region change pending -> full   */
static bool g_dmg_valid   = false;    /* a region damage is pending          */
static int  g_dmg_x0, g_dmg_y0, g_dmg_x1, g_dmg_y1;
static bool g_was_partial = false;    /* last frame presented a sub-rect     */

static inline void wm_mark_full(void) { g_full_dirty = true; g_dirty = true; }

static void dmg_add(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (!g_dmg_valid) {
        g_dmg_x0 = x; g_dmg_y0 = y; g_dmg_x1 = x1; g_dmg_y1 = y1; g_dmg_valid = true;
    } else {
        if (x  < g_dmg_x0) g_dmg_x0 = x;
        if (y  < g_dmg_y0) g_dmg_y0 = y;
        if (x1 > g_dmg_x1) g_dmg_x1 = x1;
        if (y1 > g_dmg_y1) g_dmg_y1 = y1;
    }
}

static bool any_window_opening(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_windows[i].in_use && g_windows[i].anim_open_ms != 0) return true;
    return false;
}

/* Forward decls used by the damage helper below (both defined later). */
static window_t *hit_test(int px, int py);
bool wm_modal_active(void);

/* True when a cursor move from g_prev_cur -> (mx,my) can change pixels beyond
 * the cursor cells (hover/animation surfaces) and therefore needs a full
 * recompose; a move over the static desktop does not. */
static bool mouse_over_hover_surface(int mx, int my) {
    extern int notify_count(void);
    if (desktop_menu_open() || ctxmenu_active() || wm_modal_active()) return true;
    if (g_drag_win || g_resize_win || g_close_anim.active)            return true;
    if (any_window_opening() || notify_count() > 0)                   return true;
    if (my >= desktop_panel_top() || g_prev_cur_y >= desktop_panel_top()) return true;
    if (hit_test(mx, my) || hit_test(g_prev_cur_x, g_prev_cur_y))     return true;
    return false;
}

/* ---------- Frame-rate throttle ---------------------------------------- */
static bool      g_has_composed    = false;
static uint32_t  g_last_compose_ms = 0;

/* ---------- Arrow cursor (16 x 16) -------------------------------------- */
static const char *g_cursor[16] = {
    "X...............",
    "XX..............",
    "X#X.............",
    "X##X............",
    "X###X...........",
    "X####X..........",
    "X#####X.........",
    "X######X........",
    "X#######X.......",
    "X########X......",
    "X#####XXXX......",
    "X##X##X.........",
    "X#X.X##X........",
    "XX..X##X........",
    "X....X##X.......",
    ".....X##X.......",
};

/* ---------- Helpers ----------------------------------------------------- */
static draw_target_t *screen_target(void) { return gfx_screen(); }

static void promote_to_top(window_t *w) {
    if (!w) return;
    w->z = ++g_next_z;
    g_focused = w;
}

static int max_int(int a, int b) { return a > b ? a : b; }
static int min_int(int a, int b) { return a < b ? a : b; }

/* Hit test: returns the topmost window under (px, py), NULL if none. */
static window_t *hit_test(int px, int py) {
    window_t *best = NULL;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        window_t *w = &g_windows[i];
        if (!w->in_use || !w->visible) continue;
        if (px >= w->x && px < w->x + w->w &&
            py >= w->y && py < w->y + w->h) {
            if (!best || w->z > best->z) best = w;
        }
    }
    return best;
}

/* True if (px, py) is in the resize grip of `w`. */
static bool hit_resize_grip(const window_t *w, int px, int py) {
    if (!w->resizable) return false;
    int gx = w->x + w->w - WM_RESIZE_GRIP - 1;
    int gy = w->y + w->h - WM_RESIZE_GRIP - 1;
    return px >= gx && px < gx + WM_RESIZE_GRIP &&
           py >= gy && py < gy + WM_RESIZE_GRIP;
}

/* Allocate / size the content framebuffer for a given window dimension.
 * Returns true on success, false on pool OOM (the existing framebuffer
 * is left intact in that case).
 *
 * Black-flash fix: previously this memset the whole new buffer to 0
 * (pure black ARGB) before the client's resize_cb ran.  When the
 * resize_cb was slow (or absent), the user saw the window turn
 * solid black during a drag.  We now pre-fill with the chrome face
 * colour, so even windows without a resize_cb show a neutral grey
 * (matching the window chrome) rather than a black hole during drag. */
static bool window_resize_framebuffer(window_t *w, int new_w, int new_h) {
    int content_w = new_w - 2 * WM_BORDER;
    int content_h = new_h - WM_TITLE_H - 2 * WM_BORDER;
    if (content_w < 32 || content_h < 16) return false;
    uint32_t need = (uint32_t)content_w * (uint32_t)content_h * 4u;
    uint8_t *nf = pool_realloc(w->content.fb, need);
    if (!nf) return false;
    /* Fill with the standard chrome face so an absent resize_cb still
     * shows a sane background — pure 0x00 ARGB read as black on the
     * VGA surface.  A 32-bit splat is fast enough that even resize-
     * spam mid-drag stays within the 60 Hz budget. */
    uint32_t *px = (uint32_t *)nf;
    uint32_t  n  = need / 4u;
    for (uint32_t i = 0; i < n; i++) px[i] = 0xFFC8C8C8u;
    w->content.fb     = nf;
    w->content.pitch  = (uint32_t)content_w * 4u;
    w->content.width  = (uint32_t)content_w;
    w->content.height = (uint32_t)content_h;
    w->content_bytes  = need;
    w->w = new_w;
    w->h = new_h;
    return true;
}

/* ---------- Init / create / destroy ------------------------------------- */
void wm_init(void) {
    debug_step("wm: initialising window manager + compositor v6.0");
    memset(g_windows, 0, sizeof(g_windows));
    g_focused    = NULL;
    wm_mark_full();
    g_drag_win   = NULL;
    g_resize_win = NULL;
    g_prev_btn   = 0;

    pool_init();

    g_back.width  = vga_width()  ? vga_width()  : 1024u;
    g_back.height = vga_height() ? vga_height() : 768u;
    if (g_back.width  > BACK_MAX_W) g_back.width  = BACK_MAX_W;
    if (g_back.height > BACK_MAX_H) g_back.height = BACK_MAX_H;
    g_back.pitch  = g_back.width * 4;

    gfx_clear(&g_back, DESKTOP_BG);
    mouse_set_bounds((int)vga_width() - 1, (int)vga_height() - 1);
    debug_printf("[wm] 8 MiB back buffer ready (%ux%u)\n", g_back.width, g_back.height);
}

void wm_handle_resolution_change(uint32_t new_w, uint32_t new_h) {
    if (new_w > BACK_MAX_W) new_w = BACK_MAX_W;
    if (new_h > BACK_MAX_H) new_h = BACK_MAX_H;

    g_back.width  = new_w;
    g_back.height = new_h;
    g_back.pitch  = new_w * 4;
    g_shadow_valid = false;   /* geometry changed -> next present is full */

    mouse_set_bounds((int)new_w - 1, (int)new_h - 1);

    int sw = (int)new_w;
    int sh = (int)new_h;
    int panel_top = sh - 40;

    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        window_t *w = &g_windows[i];
        if (!w->in_use) continue;
        /* Maximized windows refit the new desktop exactly. */
        if (w->maximized && panel_top > 0) {
            if (window_resize_framebuffer(w, sw, panel_top)) {
                w->x = 0; w->y = 0;
                if (w->resize_cb) w->resize_cb(w);
            }
            continue;
        }
        if (w->w > sw) {
            window_resize_framebuffer(w, sw, w->h);
        }
        if (w->h > panel_top && panel_top > 0) {
            window_resize_framebuffer(w, w->w, panel_top);
        }
        if (w->x + w->w > sw) w->x = sw - w->w;
        if (w->x < 0) w->x = 0;
        if (w->y + w->h > panel_top) w->y = panel_top - w->h;
        if (w->y < 0) w->y = 0;
    }

    gfx_clear(&g_back, DESKTOP_BG);
    wm_mark_full();
    debug_printf("[wm] resolution changed to %ux%u\n", new_w, new_h);
}

window_t *wm_create_window(int x, int y, int w, int h, const char *title) {
    int min_w = 64;
    int min_h = WM_TITLE_H + 2 * WM_BORDER + 16;
    if (w < min_w || h < min_h) {
        debug_printf("[wm] wm_create_window rejected: %dx%d below minimum %dx%d\n",
                     w, h, min_w, min_h);
        return NULL;
    }
    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_windows[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        debug_printf("[wm] CreateWindow: out of window slots "
                     "(WM_MAX_WINDOWS=%d, all in use)\n", WM_MAX_WINDOWS);
        return NULL;
    }

    int content_w = w - 2 * WM_BORDER;
    int content_h = h - WM_TITLE_H - 2 * WM_BORDER;
    uint32_t bytes = (uint32_t)content_w * (uint32_t)content_h * 4u;
    uint8_t *fb = pool_alloc(bytes);
    if (!fb) {
        debug_fail("wm", "out of framebuffer pool");
        return NULL;
    }
    memset(fb, 0, bytes);

    /* ISSUE 9: if this title was closed somewhere before, reopen it there.
     * Slot 0 (the console) is created once and never participates. */
    if (slot != 0) {
        wm_pos_mem_t *pm = pos_mem_find(title);
        if (pm) { x = pm->x; y = pm->y; }
    }
    /* Clamp into the visible desktop so a remembered (or caller-supplied)
     * position can never strand a window off-screen. */
    {
        int sw = (int)g_back.width, sh = (int)g_back.height;
        if (x > sw - 48) x = sw - 48;
        if (y > sh - 48) y = sh - 48;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
    }

    window_t *win = &g_windows[slot];
    win->in_use    = true;
    win->visible   = true;
    win->minimized = false;
    win->protected = (slot == 0);   /* slot 0 = console, cannot be closed */
    win->resizable = true;          /* default on; dialogs opt-out        */
    win->maximized = false;         /* clear stale state on slot reuse    */
    win->restore_x = x; win->restore_y = y;
    win->restore_w = w; win->restore_h = h;
    win->x = x; win->y = y;
    win->w = w; win->h = h;
    win->anim_open_ms = pit_ms();   /* ISSUE 13: start the open fade       */
    win->min_w = min_w;
    win->min_h = min_h;
    win->z = ++g_next_z;
    win->id = g_next_id++;
    win->icon = 0;              /* ICON_NONE until the app claims one */
    strncpy(win->title, title ? title : "Untitled", WM_TITLE_MAX - 1);
    win->title[WM_TITLE_MAX - 1] = 0;
    /* Stable position-memory key — the creation title, frozen here so a
     * later wm_set_title() (filename, language switch) can't change it. */
    strncpy(win->pos_key, win->title, WM_TITLE_MAX - 1);
    win->pos_key[WM_TITLE_MAX - 1] = 0;

    win->content.fb     = fb;
    win->content.pitch  = (uint32_t)content_w * 4;
    win->content.width  = (uint32_t)content_w;
    win->content.height = (uint32_t)content_h;
    win->content_bytes  = bytes;
    win->content_click  = NULL;
    win->content_user   = NULL;
    win->resize_cb      = NULL;
    win->destroy_cb     = NULL;
    win->key_handler    = NULL;

    g_focused = win;
    wm_mark_full();
    debug_printf("[wm] window %d created: \"%s\" %dx%d @ (%d,%d), content %dx%d, slot %d\n",
                 win->id, win->title, w, h, x, y, content_w, content_h, slot);
    return win;
}

void wm_destroy_window(window_t *w) {
    if (!w || !w->in_use || w->protected) return;
    if (g_drag_win        == w) g_drag_win        = NULL;
    if (g_resize_win      == w) g_resize_win      = NULL;
    if (g_title_click_win == w) g_title_click_win = NULL;

    /* ISSUE 9: remember where this window was when it closed.  For a
     * maximized window save the floating geometry, not the full-screen one. */
    if (w->maximized) pos_mem_save(w->pos_key, w->restore_x, w->restore_y);
    else              pos_mem_save(w->pos_key, w->x, w->y);

    /* ISSUE 13: snapshot the window (as last composited into the back
     * buffer) so it can keep fading out after the slot is freed.  Modal
     * dialogs (no_anim) close instantly to match their instant open. */
    if (w->visible && !w->no_anim && anim_fits(w)) {
        anim_snapshot(&g_back, w->x, w->y, w->w, w->h, g_anim_ghost);
        g_close_anim.active   = true;
        g_close_anim.x        = w->x;
        g_close_anim.y        = w->y;
        g_close_anim.w        = w->w;
        g_close_anim.h        = w->h;
        g_close_anim.start_ms = pit_ms();
    }
    /* ISSUE 1: notify the owning app FIRST so it can null its cached
     * pointer before another wm_create_window can reuse this slot.
     * The callback runs while the framebuffer is still mapped — apps
     * must not touch the framebuffer here, just clear their state. */
    if (w->destroy_cb) {
        win_destroy_cb_t cb = w->destroy_cb;
        w->destroy_cb = NULL;            /* prevent re-entry */
        cb(w);
    }
    if (w->content.fb) {
        pool_free(w->content.fb);
        w->content.fb = NULL;
    }
    w->content_bytes = 0;
    w->content_click = NULL;
    w->content_user  = NULL;
    w->resize_cb     = NULL;
    w->key_handler   = NULL;
    w->in_use        = false;
    if (g_focused == w) {
        g_focused = NULL;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (g_windows[i].in_use &&
                (!g_focused || g_windows[i].z > g_focused->z)) {
                g_focused = &g_windows[i];
            }
        }
    }
    debug_printf("[wm] window %d destroyed, slot freed\n", w->id);
    wm_mark_full();
}

void wm_set_focus(window_t *w) { promote_to_top(w); wm_mark_full(); }

void wm_set_title(window_t *w, const char *title) {
    if (!w || !w->in_use) return;
    strncpy(w->title, title ? title : "", WM_TITLE_MAX - 1);
    w->title[WM_TITLE_MAX - 1] = 0;
    wm_mark_full();
}

void wm_set_icon(window_t *w, int icon) {
    if (!w || !w->in_use) return;
    w->icon = icon;
    wm_mark_full();
}

void wm_minimize(window_t *w) {
    if (!w || !w->in_use || w->protected) return;
    w->minimized = true;
    w->visible   = false;
    if (g_focused == w) {
        g_focused = NULL;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (g_windows[i].in_use && g_windows[i].visible &&
                (!g_focused || g_windows[i].z > g_focused->z)) {
                g_focused = &g_windows[i];
            }
        }
    }
    wm_mark_full();
}

void wm_restore(window_t *w) {
    if (!w || !w->in_use) return;
    w->minimized = false;
    w->visible   = true;
    promote_to_top(w);
    wm_mark_full();
}

bool wm_can_maximize(const window_t *w) {
    /* Every app window can maximize; only protected (shell) windows and
     * windows with no resize_cb (modal dialogs — nothing would repaint the
     * enlarged area) are excluded. */
    return w && w->in_use && !w->protected && w->resize_cb != NULL;
}

void wm_toggle_maximize(window_t *w) {
    if (!wm_can_maximize(w)) return;
    int sw = (int)g_back.width;
    int panel_top = desktop_panel_top();
    if (panel_top <= 0 || panel_top > (int)g_back.height) panel_top = (int)g_back.height;

    if (w->maximized) {
        /* Restore previous floating geometry (clamped to the screen). */
        int rw = w->restore_w, rh = w->restore_h;
        if (rw < w->min_w) rw = w->min_w;
        if (rh < w->min_h) rh = w->min_h;
        if (rw > sw) rw = sw;
        if (rh > panel_top) rh = panel_top;
        if (window_resize_framebuffer(w, rw, rh)) {
            w->x = w->restore_x; w->y = w->restore_y;
            if (w->x + w->w > sw) w->x = sw - w->w;
            if (w->x < 0) w->x = 0;
            if (w->y + w->h > panel_top) w->y = panel_top - w->h;
            if (w->y < 0) w->y = 0;
            w->maximized = false;
            if (w->resize_cb) w->resize_cb(w);
        }
    } else {
        /* Remember where we were, then fill the desktop above the taskbar. */
        w->restore_x = w->x; w->restore_y = w->y;
        w->restore_w = w->w; w->restore_h = w->h;
        if (window_resize_framebuffer(w, sw, panel_top)) {
            w->x = 0; w->y = 0;
            w->maximized = true;
            if (w->resize_cb) w->resize_cb(w);
            debug_printf("[wm] window %d maximized -> %dx%d\n", w->id, sw, panel_top);
        }
    }
    promote_to_top(w);
    wm_mark_full();
}

window_t *wm_focused(void)     { return g_focused; }
int  wm_focused_id(void)       { return (g_focused && g_focused->in_use) ? g_focused->id : 0; }
void wm_mark_dirty(void)       { wm_mark_full(); }

/* Region damage in SCREEN coords (additive; does NOT force a full frame). */
void wm_damage_rect(int x, int y, int w, int h) { dmg_add(x, y, w, h); g_dirty = true; }

/* Region damage in shell-content-relative coords (e.g. the terminal idle
 * spinner cell).  Mapped to the shell window's on-screen content origin; if
 * the shell window isn't a plain visible window the safe full path is taken. */
void wm_shell_content_damage(int cx, int cy, int cw, int ch) {
    if (!g_shell_window || !g_shell_window->in_use ||
        !g_shell_window->visible || g_shell_window->minimized) {
        wm_mark_full();
        return;
    }
    int sx = g_shell_window->x + WM_BORDER + cx;
    int sy = g_shell_window->y + WM_BORDER + WM_TITLE_H + 2 + cy;
    wm_damage_rect(sx, sy, cw, ch);
}

void wm_set_shell_window(window_t *w) {
    g_shell_window = w;
    debug_printf("[wm] shell window registered: id=%d\n",
                 (w && w->in_use) ? w->id : 0);
}

bool wm_is_shell_focused(void) {
    if (!g_shell_window || !g_shell_window->in_use) return false;
    return g_focused == g_shell_window;
}

void wm_set_content_click(window_t *w, win_content_click_t cb, void *user) {
    if (!w) return;
    w->content_click = cb;
    w->content_user  = user;
}

void wm_set_resizable(window_t *w, bool resizable, int min_w, int min_h) {
    if (!w) return;
    w->resizable = resizable;
    if (min_w > 64) w->min_w = min_w;
    if (min_h > WM_TITLE_H + 2 * WM_BORDER + 16) w->min_h = min_h;
    wm_mark_full();
}

void wm_set_resize_cb(window_t *w, win_resize_cb_t cb) {
    if (!w) return;
    w->resize_cb = cb;
}

void wm_set_destroy_cb(window_t *w, win_destroy_cb_t cb) {
    if (!w) return;
    w->destroy_cb = cb;
}

void wm_set_key_handler(window_t *w, win_key_handler_t cb) {
    if (!w) return;
    w->key_handler = cb;
}

void wm_set_scroll_handler(window_t *w, win_scroll_handler_t cb) {
    if (!w) return;
    w->scroll_handler = cb;
}

/* ISSUE 4: trigger a fresh paint on every live window.  Apps that have
 * a resize_cb registered get it fired — which by convention also serves
 * as their "redraw everything from scratch" entry point — so an i18n
 * switch lights up new translated strings without per-app glue. */
void wm_force_global_repaint(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        window_t *w = &g_windows[i];
        if (!w->in_use) continue;
        if (w->resize_cb) w->resize_cb(w);
    }
    wm_mark_full();
}

/* ISSUE 3: route buffered keystrokes to the focused window's
 * key_handler.  Returns true if at least one key was consumed by a
 * window callback (read_line uses this to know it shouldn't read
 * the same bytes again itself). */
bool wm_dispatch_keys(void) {
    bool consumed_any = false;
    window_t *f = g_focused;
    if (!f || !f->in_use || !f->key_handler) return false;
    while (keyboard_has_data()) {
        int k = keyboard_getc();
        if (!f->key_handler(f, k)) {
            /* The handler refused — but we already pulled the byte
             * out of the ring.  Burn it rather than re-injecting:
             * apps that opt into a key handler are expected to be
             * exhaustive for their own focus state. */
        }
        consumed_any = true;
        if (!f->in_use) break;            /* handler may have destroyed */
    }
    return consumed_any;
}

/* Find the shell window (slot 0 is the only one flagged protected) and
 * raise it to the top.  Used by child-window owners as a single call to
 * "give input back to the user" after they close. */
void wm_refocus_shell(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (g_windows[i].in_use && g_windows[i].protected) {
            promote_to_top(&g_windows[i]);
            wm_mark_full();
            return;
        }
    }
}

/* ---------- Modal stack ------------------------------------------------- */
#define WM_MODAL_MAX  4
static window_t *g_modal_stack[WM_MODAL_MAX];
static int       g_modal_top = 0;

void wm_push_modal(window_t *w) {
    if (!w) return;
    if (g_modal_top >= WM_MODAL_MAX) {
        debug_printf("[wm] WARN modal stack full, ignoring push for win %d\n", w->id);
        return;
    }
    g_modal_stack[g_modal_top++] = w;
    debug_printf("[wm] modal push: win %d (depth=%d)\n", w->id, g_modal_top);
}

void wm_pop_modal(window_t *w) {
    for (int i = g_modal_top - 1; i >= 0; i--) {
        if (g_modal_stack[i] == w) {
            for (int j = i; j < g_modal_top - 1; j++) {
                g_modal_stack[j] = g_modal_stack[j + 1];
            }
            g_modal_top--;
            debug_printf("[wm] modal pop: win %d (depth=%d)\n",
                         w ? w->id : 0, g_modal_top);
            return;
        }
    }
}

bool wm_modal_active(void) {
    while (g_modal_top > 0 && !g_modal_stack[g_modal_top - 1]->in_use) {
        g_modal_top--;
    }
    return g_modal_top > 0;
}

window_t *wm_top_modal(void) {
    return wm_modal_active() ? g_modal_stack[g_modal_top - 1] : NULL;
}

/* ---------- Introspection for taskmgr ----------------------------------- */
void wm_pool_stats(uint32_t *used_out, uint32_t *total_out) {
    pool_stats(used_out, total_out);
}

int wm_get_windows(window_t **buf, int max) {
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS && n < max; i++) {
        if (g_windows[i].in_use) buf[n++] = &g_windows[i];
    }
    return n;
}

/* ---------- Compositor primitives --------------------------------------- */
/* Vertical-gradient fill (top-to-bottom interpolation between two ARGB
 * colours).  Used for the modernised title bars and start button. */
static uint32_t lerp_channel(uint32_t a, uint32_t b, int num, int den) {
    if (den <= 0) return a;
    return (uint32_t)((int32_t)a + (int32_t)(b - a) * num / den);
}

static uint32_t lerp_color(uint32_t a, uint32_t b, int num, int den) {
    uint32_t aa = (a >> 24) & 0xFF, ab = (b >> 24) & 0xFF;
    uint32_t ra = (a >> 16) & 0xFF, rb = (b >> 16) & 0xFF;
    uint32_t ga = (a >>  8) & 0xFF, gb = (b >>  8) & 0xFF;
    uint32_t ba = (a      ) & 0xFF, bb = (b      ) & 0xFF;
    uint32_t A = lerp_channel(aa, ab, num, den);
    uint32_t R = lerp_channel(ra, rb, num, den);
    uint32_t G = lerp_channel(ga, gb, num, den);
    uint32_t B = lerp_channel(ba, bb, num, den);
    return (A << 24) | (R << 16) | (G << 8) | B;
}

static void fill_vgrad(draw_target_t *t, int x, int y, int w, int h,
                       uint32_t top, uint32_t bot) {
    if (h <= 0) return;
    for (int r = 0; r < h; r++) {
        uint32_t c = lerp_color(top, bot, r, h - 1);
        gfx_draw_hline(t, x, y + r, w, c);
    }
}

/* Alpha-blended vertical gradient — each row honours the alpha byte of
 * the interpolated colour, so translucent gradients let the layer below
 * (window glass border, desktop) show through.  Used for the Aero title
 * bar so it reads as glass, not a solid painted band. */
static void fill_vgrad_blend(draw_target_t *t, int x, int y, int w, int h,
                             uint32_t top, uint32_t bot) {
    if (h <= 0) return;
    for (int r = 0; r < h; r++) {
        uint32_t c = lerp_color(top, bot, r, h - 1);
        gfx_blend_rect(t, x, y + r, w, 1, c);
    }
}

/* Aero Glass close button: glossy rounded rectangle per spec.
 * Gradient #ECA59E → #C44B43, 1 px #8A1E18 border, white X. */
static void draw_close_button(draw_target_t *t, int x, int y, bool hot) {
    uint32_t gtop = hot ? CLOSE_GRAD_TOP_HOT : CLOSE_GRAD_TOP;
    uint32_t gbot = hot ? CLOSE_GRAD_BOT_HOT : CLOSE_GRAD_BOT;
    /* Glossy body */
    gfx_fill_round_rect(t, x, y, WM_BTN_W, WM_BTN_H, 3, gbot);
    int half = WM_BTN_H / 2;
    /* Gradient rows follow the corner arc (no square-corner artefacts). */
    for (int r = 1; r <= half; r++) {
        int inset = gfx_round_row_inset(r, WM_BTN_H, 3);
        uint32_t c = lerp_color(gtop, gbot, r - 1, half);
        gfx_fill_rect(t, x + 1 + inset, y + r, WM_BTN_W - 2 - 2 * inset, 1, c);
    }
    /* 1 px border */
    gfx_draw_round_rect(t, x, y, WM_BTN_W, WM_BTN_H, 3, CLOSE_BORDER_C);
    /* Gloss line */
    {
        int in1 = gfx_round_row_inset(1, WM_BTN_H, 3);
        gfx_blend_rect(t, x + 1 + in1, y + 1, WM_BTN_W - 2 - 2 * in1, 1, 0x70FFFFFFu);
    }
    /* White X — 2-px thick diagonal cross */
    int gx = x + (WM_BTN_W - 8) / 2;
    int gy = y + (WM_BTN_H - 8) / 2;
    for (int i = 0; i < 8; i++) {
        gfx_putpixel(t, gx + i,     gy + i,     CLOSE_FG_C);
        gfx_putpixel(t, gx + i + 1, gy + i,     CLOSE_FG_C);
        gfx_putpixel(t, gx + 7 - i, gy + i,     CLOSE_FG_C);
        gfx_putpixel(t, gx + 8 - i, gy + i,     CLOSE_FG_C);
    }
}

/* Aero Glass minimize button: neutral glass tones, rounded rect. */
static void draw_minimize_button(draw_target_t *t, int x, int y, bool hot) {
    uint32_t gtop = hot ? MIN_GRAD_TOP_HOT : MIN_GRAD_TOP;
    uint32_t gbot = hot ? MIN_GRAD_BOT_HOT : MIN_GRAD_BOT;
    gfx_fill_round_rect(t, x, y, WM_BTN_W, WM_BTN_H, 3, gbot);
    int half = WM_BTN_H / 2;
    for (int r = 1; r <= half; r++) {
        int inset = gfx_round_row_inset(r, WM_BTN_H, 3);
        uint32_t c = lerp_color(gtop, gbot, r - 1, half);
        gfx_fill_rect(t, x + 1 + inset, y + r, WM_BTN_W - 2 - 2 * inset, 1, c);
    }
    gfx_draw_round_rect(t, x, y, WM_BTN_W, WM_BTN_H, 3, MIN_BORDER_C);
    {
        int in1 = gfx_round_row_inset(1, WM_BTN_H, 3);
        gfx_blend_rect(t, x + 1 + in1, y + 1, WM_BTN_W - 2 - 2 * in1, 1, 0x50FFFFFFu);
    }
    /* Underscore glyph (minimise) */
    int gy = y + WM_BTN_H - 6;
    gfx_fill_rect(t, x + 6, gy, WM_BTN_W - 12, 2, MIN_FG_C);
}

/* Aero Glass maximize / restore button: same neutral glass body; the glyph
 * is a single window outline (maximize) or two offset outlines (restore). */
static void draw_maximize_button(draw_target_t *t, int x, int y, bool hot,
                                 bool is_restore) {
    uint32_t gtop = hot ? MIN_GRAD_TOP_HOT : MIN_GRAD_TOP;
    uint32_t gbot = hot ? MIN_GRAD_BOT_HOT : MIN_GRAD_BOT;
    gfx_fill_round_rect(t, x, y, WM_BTN_W, WM_BTN_H, 3, gbot);
    int half = WM_BTN_H / 2;
    for (int r = 1; r <= half; r++) {
        int inset = gfx_round_row_inset(r, WM_BTN_H, 3);
        uint32_t c = lerp_color(gtop, gbot, r - 1, half);
        gfx_fill_rect(t, x + 1 + inset, y + r, WM_BTN_W - 2 - 2 * inset, 1, c);
    }
    gfx_draw_round_rect(t, x, y, WM_BTN_W, WM_BTN_H, 3, MIN_BORDER_C);
    {
        int in1 = gfx_round_row_inset(1, WM_BTN_H, 3);
        gfx_blend_rect(t, x + 1 + in1, y + 1, WM_BTN_W - 2 - 2 * in1, 1, 0x50FFFFFFu);
    }

    int bw = 9, bh = 8;
    int gx = x + (WM_BTN_W - bw) / 2;
    int gy = y + (WM_BTN_H - bh) / 2;
    if (is_restore) {
        /* Back square (offset up-right) + front square overlapping it. */
        gfx_draw_rect(t, gx + 2, gy - 2, bw - 2, bh - 2, MIN_FG_C);
        gfx_fill_rect(t, gx, gy + 2, bw - 2, 1, MIN_FG_C);   /* clear seam */
        gfx_draw_rect(t, gx, gy + 1, bw - 2, bh - 1, MIN_FG_C);
        /* thicker title edge on the front square */
        gfx_fill_rect(t, gx, gy + 1, bw - 2, 2, MIN_FG_C);
    } else {
        gfx_draw_rect(t, gx, gy, bw, bh, MIN_FG_C);
        /* thicker top edge so it reads as a window, not a plain box */
        gfx_fill_rect(t, gx, gy, bw, 2, MIN_FG_C);
    }
}

static void draw_resize_grip(draw_target_t *t, int x, int y) {
    /* Three short diagonal hash marks at 45 degrees, classic Win 9x grip. */
    for (int i = 0; i < WM_RESIZE_GRIP - 2; i += 3) {
        for (int j = 0; j < WM_RESIZE_GRIP - 2 - i; j += 1) {
            gfx_putpixel(t, x + WM_RESIZE_GRIP - 2 - j,
                         y + WM_RESIZE_GRIP - 2 - i - j, GRIP_HI);
            gfx_putpixel(t, x + WM_RESIZE_GRIP - 1 - j,
                         y + WM_RESIZE_GRIP - 1 - i - j, GRIP_FG);
        }
    }
}

static void draw_window_chrome(draw_target_t *t, const window_t *w, bool focused) {
    int mx = mouse_x(), my = mouse_y();

    /* ---- Drop shadow -------------------------------------------------- */
    gfx_drop_shadow(t, w->x, w->y, w->w, w->h, focused ? 12 : 6);

    /* ---- Frosted backdrop: blur whatever is behind the translucent glass
     * title bar so an overlapping window or the desktop reads as true
     * frosted glass under the chrome.  Only the title band is blurred — the
     * 8 px side/bottom borders are too thin for the frost to register, so
     * blurring them is wasted compositor time (measured on COM1). -------- */
    gfx_box_blur(t, w->x, w->y, w->w, WM_BORDER + WM_TITLE_H + 3, GLASS_BLUR_R);

    /* ---- Glass border: alpha-blend #7096BA @ alpha 160 over desktop --- *
     * This is the "source over" alpha composite that makes the border
     * semi-transparent against whatever desktop content is behind it. */
    gfx_blend_rect(t, w->x, w->y, w->w, w->h, AERO_BORDER);

    /* ---- Rounded top corners (radius ~6 px) --------------------------- *
     * Clip the corner pixels back to the desktop background so the
     * window silhouette has smooth rounded top-left / top-right arcs.
     * Bottom corners are square per the reference screenshot. */
    static const struct { int c, r; } clip6[] = {
        {0,0},{1,0},{2,0},{3,0},
        {0,1},{1,1},
        {0,2},{0,3},
    };
    for (int i = 0; i < 8; i++) {
        int c = clip6[i].c, r = clip6[i].r;
        gfx_putpixel(t, w->x + c,            w->y + r, DESKTOP_BG);
        gfx_putpixel(t, w->x + w->w - 1 - c, w->y + r, DESKTOP_BG);
    }
    /* Soft AA transition at the rounding boundary */
    gfx_blend_pixel(t, w->x + 4,            w->y,     0x80000000u);
    gfx_blend_pixel(t, w->x + 2,            w->y + 1, 0x80000000u);
    gfx_blend_pixel(t, w->x + 1,            w->y + 2, 0x80000000u);
    gfx_blend_pixel(t, w->x + w->w - 5,     w->y,     0x80000000u);
    gfx_blend_pixel(t, w->x + w->w - 3,     w->y + 1, 0x80000000u);
    gfx_blend_pixel(t, w->x + w->w - 2,     w->y + 2, 0x80000000u);

    /* ---- Title bar: 30 px frosted glass — same alpha-blend treatment
     * as the outer border.  Each gradient stop carries its own alpha so
     * the underlying #7096BA glass + desktop bleed through, giving the
     * title strip a true Aero look instead of a painted slab. */
    int tbar_x = w->x + WM_BORDER;
    int tbar_y = w->y + WM_BORDER;
    int tbar_w = w->w - 2 * WM_BORDER;
    if (focused) {
        int third = WM_TITLE_H / 3;
        fill_vgrad_blend(t, tbar_x, tbar_y,             tbar_w, third + 1,
                         TITLE_GRAD_TOP_F, TITLE_GRAD_MID_F);
        fill_vgrad_blend(t, tbar_x, tbar_y + third + 1, tbar_w, WM_TITLE_H - third - 1,
                         TITLE_GRAD_MID_F, TITLE_GRAD_BOT_F);
    } else {
        fill_vgrad_blend(t, tbar_x, tbar_y, tbar_w, WM_TITLE_H,
                         TITLE_GRAD_TOP_U, TITLE_GRAD_BOT_U);
    }
    /* 1 px gloss line at top of title bar ("shiny chrome" feel) */
    gfx_blend_rect(t, tbar_x + 1, tbar_y + 1, tbar_w - 2, 1, TITLE_HIGHLIGHT);

    /* ---- Content area: pure white ------------------------------------- */
    int cont_x = w->x + WM_BORDER;
    int cont_y = w->y + WM_BORDER + WM_TITLE_H;
    int cont_w = w->w - 2 * WM_BORDER;
    int cont_h = w->h - 2 * WM_BORDER - WM_TITLE_H;
    if (cont_w > 0 && cont_h > 0)
        gfx_fill_rect(t, cont_x, cont_y, cont_w, cont_h, 0xFFFFFFFFu);

    /* ---- Outer glass edge: 1 px #FFFFFF alpha 100 --------------------- *
     * The absolute outermost pixel of the window frame on each side,
     * inside the already-clipped rounded corners. */
    gfx_blend_rect(t, w->x + 5, w->y,          w->w - 10,  1, AERO_OUTER_LINE);
    gfx_blend_rect(t, w->x,     w->y + 5,       1, w->h - 6, AERO_OUTER_LINE);
    gfx_blend_rect(t, w->x + w->w - 1, w->y + 5, 1, w->h - 6, AERO_OUTER_LINE);
    gfx_blend_rect(t, w->x + 2, w->y + w->h - 1, w->w - 4, 1, AERO_OUTER_LINE);

    /* ---- Inner glass edge: 1 px #FFFFFF alpha 150 --------------------- *
     * Just inside the 8 px glass border — where glass meets content. */
    if (cont_w > 0 && cont_h > 0) {
        gfx_blend_rect(t, cont_x - 1, cont_y,             1, cont_h, AERO_INNER_LINE);
        gfx_blend_rect(t, cont_x + cont_w, cont_y,        1, cont_h, AERO_INNER_LINE);
        gfx_blend_rect(t, cont_x, cont_y + cont_h,        cont_w, 1, AERO_INNER_LINE);
        /* Title / content junction */
        gfx_blend_rect(t, cont_x, cont_y - 1,             cont_w, 1, AERO_INNER_LINE);
    }

    /* ---- Hairline separator at title → content boundary --------------- */
    gfx_blend_rect(t, tbar_x, tbar_y + WM_TITLE_H, tbar_w, 1, 0x40000000u);

    /* ---- Title text: 16 px chrome face (EPX-upscaled AA), black with a
     * 1 px white under-shadow.  The 8 px face was unreadably small inside
     * the 30 px glass bar; the 2x face matches the bar and the rest of
     * the refreshed chrome.  Clipped so long titles never run under the
     * window buttons. */
    int tx = tbar_x + 10;
    int ty = tbar_y + (WM_TITLE_H - 2 * FONT_GLYPH_H) / 2;
    int title_max = tbar_w - 10 - (3 * (WM_BTN_W + 3)) - 8;
    if (title_max > 16) {
        gfx_draw_string_aa2x_clipped(t, tx + 1, ty + 1, title_max,
                                     w->title, TITLE_SHADOW_C, 0x00000000u);
        gfx_draw_string_aa2x_clipped(t, tx, ty, title_max,
                                     w->title, TITLE_FG, 0x00000000u);
    }

    /* ---- Window control buttons (anchored top-right, 1 px padding) ---- *
     * Layout right-to-left: [close] [maximize] [minimize].  The maximize
     * button only appears on windows that can actually relayout (resize_cb). */
    if (!w->protected) {
        int bx = w->x + w->w - WM_BORDER - WM_BTN_W - 2;
        int by = tbar_y + (WM_TITLE_H - WM_BTN_H) / 2;
        bool hot = (mx >= bx && mx < bx + WM_BTN_W &&
                    my >= by && my < by + WM_BTN_H);
        draw_close_button(t, bx, by, hot);

        int xbx = bx;
        if (wm_can_maximize(w)) {
            xbx = bx - WM_BTN_W - 3;
            bool xhot = (mx >= xbx && mx < xbx + WM_BTN_W &&
                         my >= by && my < by + WM_BTN_H);
            draw_maximize_button(t, xbx, by, xhot, w->maximized);
        }

        int mbx = xbx - WM_BTN_W - 3;
        bool mhot = (mx >= mbx && mx < mbx + WM_BTN_W &&
                     my >= by && my < by + WM_BTN_H);
        draw_minimize_button(t, mbx, by, mhot);
    }
}

static void draw_resize_grip_overlay(draw_target_t *t, const window_t *w) {
    if (!w->resizable) return;
    int gx = w->x + w->w - WM_RESIZE_GRIP - 1;
    int gy = w->y + w->h - WM_RESIZE_GRIP - 1;
    /* Repaint a small white patch so the grip stands out. */
    gfx_fill_rect(t, gx - 1, gy - 1, WM_RESIZE_GRIP + 2, WM_RESIZE_GRIP + 2,
                  0xFFFFFFFFu);
    draw_resize_grip(t, gx, gy);
}

static void blit_window_content(draw_target_t *t, const window_t *w) {
    int dx = w->x + WM_BORDER;
    int dy = w->y + WM_BORDER + WM_TITLE_H + 2;
    draw_target_t src = w->content;
    gfx_blit(t, dx, dy, &src);
}

static void draw_cursor_overlay(draw_target_t *t, int mx, int my) {
    for (int y = 0; y < 16; y++) {
        const char *row = g_cursor[y];
        for (int x = 0; x < 16; x++) {
            uint32_t c;
            switch (row[x]) {
                case 'X': c = 0xFFFFFFFFu; break;
                case '#': c = 0xFF000000u; break;
                default : continue;
            }
            gfx_putpixel(t, mx + x, my + y, c);
        }
    }
}

/* ---------- Mouse handler: hit-test, drag, resize, focus, close, ctxmenu */
static void wm_handle_mouse(int mx, int my, uint8_t btn) {
    uint8_t pressed  = (uint8_t)(btn  & ~g_prev_btn);
    uint8_t released = (uint8_t)(~btn &  g_prev_btn);

    /* Step 0: open context menu owns ALL clicks first - both buttons. */
    if (ctxmenu_active() &&
        (pressed & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT))) {
        ctxmenu_handle_click(mx, my, pressed);
        wm_mark_full();
        g_prev_btn = btn;
        return;
    }

    /* ---- Drag-tracking for the system tray volume slider --------------
     * While the left button is held over the slider track, every mouse
     * motion event updates the master volume in proportion to the
     * cursor's X position.  We do this BEFORE any window hit-tests so
     * a quick drag never accidentally promotes a window underneath. */
    if ((btn & MOUSE_BTN_LEFT) && my >= desktop_panel_top()) {
        if (tray_handle_drag(mx, my, true)) {
            wm_mark_full();
            g_prev_btn = btn;
            return;
        }
    }

    /* ---- Left button: panel/window/icon dispatch ---------------------- */
    if (pressed & MOUSE_BTN_LEFT) {
        if (desktop_handle_left_click_panel(mx, my)) {
            wm_mark_full();
            g_prev_btn = btn;
            return;
        }

        window_t *hit = hit_test(mx, my);
        if (wm_modal_active() && hit != wm_top_modal()) {
            hit = NULL;
        }
        if (hit) {
            promote_to_top(hit);
            wm_mark_full();

            /* Resize grip first (bottom-right corner of the chrome). */
            if (hit_resize_grip(hit, mx, my)) {
                g_resize_win      = hit;
                g_resize_start_w  = hit->w;
                g_resize_start_h  = hit->h;
                g_resize_start_mx = mx;
                g_resize_start_my = my;
                g_prev_btn = btn;
                return;
            }

            /* Title bar zone? */
            int title_top = hit->y + WM_BORDER;
            int title_bot = title_top + WM_TITLE_H;
            if (my >= title_top && my < title_bot) {
                /* Hit-test geometry mirrors draw_window_chrome - both
                 * use WM_BTN_W / WM_BTN_H so resizing the buttons in
                 * the future is a single-define change. */
                if (!hit->protected) {
                    int bx = hit->x + hit->w - WM_BORDER - WM_BTN_W - 2;
                    int by = title_top + (WM_TITLE_H - WM_BTN_H) / 2;
                    if (mx >= bx && mx < bx + WM_BTN_W &&
                        my >= by && my < by + WM_BTN_H) {
                        wm_destroy_window(hit);
                        g_prev_btn = btn;
                        return;
                    }
                    int xbx = bx;
                    if (wm_can_maximize(hit)) {
                        xbx = bx - WM_BTN_W - 3;
                        if (mx >= xbx && mx < xbx + WM_BTN_W &&
                            my >= by && my < by + WM_BTN_H) {
                            wm_toggle_maximize(hit);
                            g_prev_btn = btn;
                            return;
                        }
                    }
                    int mbx = xbx - WM_BTN_W - 3;
                    if (mx >= mbx && mx < mbx + WM_BTN_W &&
                        my >= by && my < by + WM_BTN_H) {
                        wm_minimize(hit);
                        g_prev_btn = btn;
                        return;
                    }
                }
                /* Title-bar double-click toggles maximize (Windows-style). */
                uint32_t now = pit_ms();
                if (hit == g_title_click_win &&
                    (now - g_title_click_ms) <= WM_TITLE_DBLCLICK_MS &&
                    wm_can_maximize(hit)) {
                    wm_toggle_maximize(hit);
                    g_title_click_win = NULL;
                    g_prev_btn = btn;
                    return;
                }
                g_title_click_win = hit;
                g_title_click_ms  = now;
                /* A maximized window that the user starts dragging snaps back
                 * to its floating size first (drag-to-restore). */
                if (hit->maximized) {
                    wm_toggle_maximize(hit);
                    /* Re-centre the title under the cursor after the shrink. */
                    int off = mx - hit->x;
                    if (off > hit->w - 40) off = hit->w / 2;
                    g_drag_off_x = off;
                    g_drag_off_y = my - hit->y;
                    g_drag_win   = hit;
                } else {
                    g_drag_win   = hit;
                    g_drag_off_x = mx - hit->x;
                    g_drag_off_y = my - hit->y;
                }
            } else if (hit->content_click) {
                int cx = mx - (hit->x + WM_BORDER);
                int cy = my - (hit->y + WM_BORDER + WM_TITLE_H + 2);
                hit->content_click(hit, cx, cy, pressed, btn);
                wm_mark_full();
            }
        } else {
            /* No window under cursor - desktop interaction. */
            (void)desktop_handle_left_click_icon(mx, my, pit_ms());
        }
    }

    /* ---- Right button: window content OR desktop / taskbar dispatch ---- */
    if (pressed & MOUSE_BTN_RIGHT) {
        /* Taskbar tile right-click (TASK 9): give the desktop a chance
         * to open its "Bezárás" context menu before falling through to
         * the regular window hit-test. */
        if (my >= desktop_panel_top()) {
            if (desktop_handle_right_click_panel(mx, my)) {
                wm_mark_full();
            }
            g_prev_btn = btn;
            return;
        }
        window_t *hit = hit_test(mx, my);
        if (wm_modal_active() && hit != wm_top_modal()) hit = NULL;
        if (hit && hit->content_click) {
            int title_top = hit->y + WM_BORDER;
            int title_bot = title_top + WM_TITLE_H;
            if (my >= title_bot) {
                promote_to_top(hit);
                int cx = mx - (hit->x + WM_BORDER);
                int cy = my - (hit->y + WM_BORDER + WM_TITLE_H + 2);
                hit->content_click(hit, cx, cy, pressed, btn);
                wm_mark_full();
            }
        } else if (!hit) {
            /* Right-click on the desktop (no window).  Let the desktop
             * spawn its own context menu for icon / wallpaper actions. */
            if (desktop_handle_right_click(mx, my)) {
                wm_mark_full();
            }
        }
    }

    /* Button release: stop drag / resize. */
    if (released & MOUSE_BTN_LEFT) {
        g_drag_win   = NULL;
        g_resize_win = NULL;
    }

    /* Active resize: rebuild framebuffer + fire callback on each delta. */
    if (g_resize_win && g_resize_win->in_use && (btn & MOUSE_BTN_LEFT)) {
        int dw = mx - g_resize_start_mx;
        int dh = my - g_resize_start_my;
        int new_w = g_resize_start_w + dw;
        int new_h = g_resize_start_h + dh;
        if (new_w < g_resize_win->min_w) new_w = g_resize_win->min_w;
        if (new_h < g_resize_win->min_h) new_h = g_resize_win->min_h;
        /* Clamp to screen so the grip stays reachable. */
        int max_w = (int)g_back.width - g_resize_win->x;
        int max_h = desktop_panel_top() - g_resize_win->y;
        if (new_w > max_w) new_w = max_w;
        if (new_h > max_h) new_h = max_h;
        if (new_w != g_resize_win->w || new_h != g_resize_win->h) {
            int ow = g_resize_win->w, oh = g_resize_win->h;
            if (window_resize_framebuffer(g_resize_win, new_w, new_h)) {
                g_resize_win->maximized = false;   /* manual resize un-maximizes */
                if (g_resize_win->resize_cb) {
                    g_resize_win->resize_cb(g_resize_win);
                }
                /* Damage only the old U new footprint (+ shadow margin), not
                 * the whole screen — the slow LFB write is the real-HW cost. */
                int m = 20;
                int mw = (ow > new_w ? ow : new_w), mh = (oh > new_h ? oh : new_h);
                wm_damage_rect(g_resize_win->x - m, g_resize_win->y - m,
                               mw + 2 * m, mh + 2 * m);
            }
        }
    }

    /* Active drag: update window position. */
    if (g_drag_win && g_drag_win->in_use && (btn & MOUSE_BTN_LEFT)) {
        int max_y = desktop_panel_top() - WM_TITLE_H - WM_BORDER;
        if (max_y < 0) max_y = 0;
        int nx = mx - g_drag_off_x;
        int ny = my - g_drag_off_y;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx + g_drag_win->w > (int)g_back.width) nx = (int)g_back.width - g_drag_win->w;
        if (ny > max_y)                       ny = max_y;
        if (nx != g_drag_win->x || ny != g_drag_win->y) {
            int ox = g_drag_win->x, oy = g_drag_win->y;
            g_drag_win->x = nx;
            g_drag_win->y = ny;
            /* Damage the union of the old + new footprints (incl. drop shadow)
             * so a drag touches only that band — the window now keeps up with
             * the cursor instead of forcing a full-screen recompose+present. */
            int m = 20;
            int x0 = (ox < nx ? ox : nx) - m;
            int y0 = (oy < ny ? oy : ny) - m;
            int x1 = (ox > nx ? ox : nx) + g_drag_win->w + m;
            int y1 = (oy > ny ? oy : ny) + g_drag_win->h + m;
            wm_damage_rect(x0, y0, x1 - x0, y1 - y0);
        }
    }

    g_prev_btn = btn;
}

/* Region-limited present: push only the [x,y,w,h] box of g_back to the LFB
 * (still row-damage-tracked inside the box).  Returns bytes actually written
 * to the hardware framebuffer.  Defined after wm_blit_backbuffer. */
static uint32_t wm_blit_rect(int x, int y, int w, int h);

/* ---------- Main compositor pass ---------------------------------------- */
static void wm_recompose(int mx, int my) {
    uint32_t _perf_t0 = pit_now_us();

    /* Region-based compositing: recompose + present only the damaged box.
     * Full frames (the safe default) take the whole screen; otherwise the box
     * is the union of the pending region damage (e.g. the spinner cell) and
     * the old+new cursor cells, so the cursor is always repainted inside it. */
    int rx, ry, rw, rh;
    g_was_partial = false;
    if (g_full_dirty) {
        rx = 0; ry = 0; rw = (int)g_back.width; rh = (int)g_back.height;
    } else {
        int  x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool have = false;
        if (g_dmg_valid) {                 /* region damage (e.g. spinner) */
            x0 = g_dmg_x0; y0 = g_dmg_y0; x1 = g_dmg_x1; y1 = g_dmg_y1; have = true;
        }
        /* The cursor cells (old+new) only need repainting when the cursor
         * moved, when there is no other damage, or when the damage box would
         * overlap (and thus erase) the resting cursor.  This keeps a stationary
         * cursor from inflating the spinner box across the whole screen. */
        bool cursor_moved = (g_prev_cur_x != mx || g_prev_cur_y != my);
        bool need_cursor  = cursor_moved || !have;
        if (have && !need_cursor &&
            !(mx + 16 <= x0 || mx >= x1 || my + 16 <= y0 || my >= y1))
            need_cursor = true;
        if (need_cursor) {
            int px = g_prev_cur_x < 0 ? mx : g_prev_cur_x;
            int py = g_prev_cur_y < 0 ? my : g_prev_cur_y;
            int cx0 = (px < mx ? px : mx),      cy0 = (py < my ? py : my);
            int cx1 = (px > mx ? px : mx) + 16, cy1 = (py > my ? py : my) + 16;
            if (!have) { x0 = cx0; y0 = cy0; x1 = cx1; y1 = cy1; have = true; }
            else { if (cx0<x0) x0=cx0; if (cy0<y0) y0=cy0;
                   if (cx1>x1) x1=cx1; if (cy1>y1) y1=cy1; }
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        rx = x0; ry = y0; rw = x1 - x0; rh = y1 - y0;
        if (!have || rw <= 0 || rh <= 0) {   /* safety: degenerate -> full */
            rx = 0; ry = 0; rw = (int)g_back.width; rh = (int)g_back.height;
        } else {
            g_was_partial = true;
        }
    }
    gfx_set_clip(&g_back, rx, ry, rw, rh);

    /* Step 1: wallpaper + desktop icons (lowest z). */
    desktop_draw_background(&g_back);
    uint32_t _perf_bg = pit_now_us();

    /* Step 2: window decorations + content, back-to-front.  Resize grip
     * is painted AFTER the content blit so it sits on top of the
     * client's framebuffer (otherwise the content would obscure it).
     * ISSUE 13: a freshly-created window fades in — we snapshot the
     * background beneath it, paint it opaque, then blend the background
     * back over it at (1-progress) opacity. */
    uint32_t anim_now = pit_ms();
    for (int z = 1; z <= g_next_z; z++) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            window_t *w = &g_windows[i];
            if (!w->in_use || !w->visible) continue;
            if (w->z != z) continue;

            bool opening = w->anim_open_ms != 0;
            int  prog    = 255;
            if (opening) {
                uint32_t dt = anim_now - w->anim_open_ms;
                if (dt >= WM_ANIM_MS || !anim_fits(w)) {
                    w->anim_open_ms = 0;       /* animation finished */
                    opening = false;
                } else {
                    /* Ease-out cubic: the window decelerates as it settles in. */
                    prog = (int)anim_ease_out_cubic(dt, WM_ANIM_MS);
                }
            }

            if (opening)
                anim_snapshot(&g_back, w->x, w->y, w->w, w->h, g_anim_bg);

            draw_window_chrome(&g_back, w, w == g_focused);
            blit_window_content(&g_back, w);
            draw_resize_grip_overlay(&g_back, w);

            if (opening) {
                anim_blend(&g_back, w->x, w->y, w->w, w->h, g_anim_bg, 255 - prog);
                /* Keep the fade animating by damaging only the window footprint
                 * (+ shadow), not the whole screen. */
                wm_damage_rect(w->x - 16, w->y - 16, w->w + 32, w->h + 32);
            }
        }
    }

    /* ISSUE 13: a closing window keeps fading out from its last-composited
     * snapshot, above the desktop/windows but below the panel. */
    if (g_close_anim.active) {
        uint32_t dt = anim_now - g_close_anim.start_ms;
        if (dt >= WM_ANIM_MS) {
            g_close_anim.active = false;
            /* Final clean redraw of the region so the last faint ghost frame
             * is wiped immediately (not left until the next full frame). */
            wm_damage_rect(g_close_anim.x - 16, g_close_anim.y - 16,
                           g_close_anim.w + 32, g_close_anim.h + 32);
        } else {
            int prog = (int)anim_ease_out_cubic(dt, WM_ANIM_MS);
            anim_blend(&g_back, g_close_anim.x, g_close_anim.y,
                       g_close_anim.w, g_close_anim.h, g_anim_ghost, 255 - prog);
            wm_damage_rect(g_close_anim.x - 16, g_close_anim.y - 16,
                           g_close_anim.w + 32, g_close_anim.h + 32);
        }
    }

    /* Step 3: start menu UNDER the panel + calendar popup (TASK 33).
     * Drawing the menu before the panel lets the Start-menu open/close
     * slide emerge from / sink behind the taskbar cleanly (ISSUE 13). */
    desktop_draw_menu    (&g_back);
    desktop_draw_panel   (&g_back);
    desktop_draw_calendar(&g_back);

    /* Step 4: context menu (above panel + menu). */
    ctxmenu_draw(&g_back);

    /* Step 4b: system-tray applets + notification toasts. */
    {
        extern void tray_draw   (draw_target_t *t, int taskbar_top, int sw);
        extern void notify_tick (void);
        extern void notify_draw (draw_target_t *t);
        notify_tick();
        tray_draw(&g_back, desktop_panel_top(), (int)g_back.width);
        notify_draw(&g_back);
    }

    /* Step 5: mouse cursor on top of everything. */
    draw_cursor_overlay(&g_back, mx, my);
    uint32_t _perf_mid = pit_now_us();

    /* Step 6: present only the damaged box to the VESA framebuffer. */
    gfx_reset_clip(&g_back);
    uint32_t _present_bytes = wm_blit_rect(rx, ry, rw, rh);
    g_prev_cur_x = mx; g_prev_cur_y = my;
    g_dirty = false;
    g_full_dirty = false;     /* consumed: next frame is partial unless re-marked */
    g_dmg_valid  = false;

    /* Perf telemetry (spec: MEASURE ms/frame on COM1, never guess).  The
     * glass blur runs in cached RAM here; this proves the budget is kept.
     * Rolling average + worst case logged every 120 composed frames. */
    {
        uint32_t t_end = pit_now_us();
        uint32_t dt   = t_end     - _perf_t0;       /* u32 sub handles wrap  */
        uint32_t bg   = _perf_bg  - _perf_t0;       /* background pass       */
        uint32_t mid  = _perf_mid - _perf_bg;       /* windows+menu+panel    */
        uint32_t blit = t_end     - _perf_mid;      /* present (memcmp/copy) */
        static uint32_t acc = 0, abg = 0, amid = 0, abl = 0, n = 0, mx_us = 0;
        static uint32_t apres = 0, aco = 0; /* avg LFB bytes/f, cursor-only n */
        acc += dt; abg += bg; amid += mid; abl += blit; apres += _present_bytes; n++;
        if (g_was_partial) aco++;
        if (dt > mx_us) mx_us = dt;
        if (n >= 120) {
            debug_printf("[wm/perf] avg %u.%03u ms (bg %u.%03u | ui %u.%03u | "
                         "blit %u.%03u) max %u.%03u ms | present %u KiB/f | "
                         "partial %u/%u\n",
                         (acc/n)/1000u, (acc/n)%1000u, (abg/n)/1000u, (abg/n)%1000u,
                         (amid/n)/1000u, (amid/n)%1000u, (abl/n)/1000u, (abl/n)%1000u,
                         mx_us/1000u, mx_us%1000u, (apres/n) / 1024u, aco, n);
            acc = abg = amid = abl = apres = n = mx_us = aco = 0;
        }
    }
}

/* ---------- Off-screen back buffer accessors --------------------------- *
 * Exposed so the pre-desktop login modal can render its alpha-blended
 * panel into cached RAM instead of the uncached hardware framebuffer, then
 * present it with a single write-only blit (see window.h).  Safe to borrow
 * during login because the WM is not compositing yet -- wm_present() at the
 * end of boot overwrites g_back with the real desktop frame. */
draw_target_t *wm_backbuffer(void) { return &g_back; }

void wm_blit_backbuffer(void) {
    draw_target_t *s = screen_target();
    if (!s || !s->fb) return;
    uint32_t rows         = (s->height < g_back.height) ? s->height : g_back.height;
    uint32_t bytes_per_row = (s->width  < g_back.width  ? s->width  : g_back.width) * 4u;

    /* Damage-tracked path: only push rows that actually changed since the last
     * present out to the (slow, uncached) hardware framebuffer.  When the
     * shadow is stale (first frame, resolution change, or after some other
     * surface owned the LFB) fall back to a full blit and rebuild the shadow. */
    if (!g_shadow_valid) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t *src = g_back.fb       + y * g_back.pitch;
            memcpy(s->fb              + y * s->pitch,      src, bytes_per_row);
            memcpy(g_shadow_storage   + y * g_back.pitch,  src, bytes_per_row);
        }
        g_shadow_valid = true;
        return;
    }
    for (uint32_t y = 0; y < rows; y++) {
        uint8_t *src = g_back.fb         + y * g_back.pitch;
        uint8_t *shd = g_shadow_storage  + y * g_back.pitch;
        if (memcmp(src, shd, bytes_per_row) == 0) continue;  /* row unchanged */
        memcpy(s->fb + y * s->pitch, src, bytes_per_row);
        memcpy(shd, src, bytes_per_row);
    }
}

/* Region-limited present: push only the [x,y,w,h] box of g_back to the LFB,
 * still row-damage-tracked WITHIN the box (so an over-estimated dirty box
 * never re-writes pixels that didn't change).  Returns bytes written to the
 * (slow, uncached) hardware framebuffer — the metric that matters on real HW.
 * Falls back to a full blit when the shadow is stale. */
static uint32_t wm_blit_rect(int x, int y, int w, int h) {
    draw_target_t *s = screen_target();
    if (!s || !s->fb) return 0;
    if (!g_shadow_valid) {
        wm_blit_backbuffer();                /* full rebuild + revalidate */
        return s->width * s->height * 4u;
    }
    /* Clamp to both surfaces. */
    int sw = (int)((s->width  < g_back.width)  ? s->width  : g_back.width);
    int sh = (int)((s->height < g_back.height) ? s->height : g_back.height);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return 0;

    size_t off   = (size_t)x * 4u;
    size_t bytes = (size_t)w * 4u;
    uint32_t written = 0;
    for (int row = 0; row < h; row++) {
        uint32_t yy = (uint32_t)(y + row);
        uint8_t *src = g_back.fb        + (size_t)yy * g_back.pitch + off;
        uint8_t *shd = g_shadow_storage + (size_t)yy * g_back.pitch + off;
        if (memcmp(src, shd, bytes) == 0) continue;  /* span unchanged */
        memcpy(s->fb + (size_t)yy * s->pitch + off, src, bytes);
        memcpy(shd, src, bytes);
        written += (uint32_t)bytes;
    }
    return written;
}

/* Force the next present to be a full blit (used when something other than the
 * compositor may have written the LFB, e.g. the screensaver, or after a
 * resolution change). */
void wm_invalidate_shadow(void) { g_shadow_valid = false; }

/* ---------- Public immediate compose (boot-time only) ------------------- */
void wm_present(void) {
    int mx = mouse_x(), my = mouse_y();
    wm_recompose(mx, my);
}

/* ---------- Public tick (called from shell idle loop) ------------------- */
void wm_tick(void) {
    /* Audio rides on the WM tick: every loop in the system that follows
     * the "modal loops must pump wm_tick()" stability rule automatically
     * keeps the HDA DMA ring fed too (one MMIO read when idle).  Without
     * this, sounds queued right before a blocking phase (boot jingle ->
     * login screen) audibly looped the cyclic buffer. */
    extern void audio_tick(void);
    audio_tick();

    /* While the screensaver owns the framebuffer the WM must NOT also
     * write to it.  The two used to run back-to-back in the idle loop and
     * fought over the same surface, so the live desktop bled through the
     * saver in glitchy horizontal bands.  Input IRQs call screensaver_kick()
     * which wakes the saver AND marks the desktop dirty, so the very next
     * tick after the saver releases repaints the whole desktop cleanly. */
    extern bool screensaver_active(void);
    if (screensaver_active()) {
        /* The saver draws straight to the LFB behind our back, so our shadow
         * no longer reflects the screen; force a full blit on the next frame. */
        g_shadow_valid = false;
        return;
    }

    /* Keep the tray clock (and any other 1 Hz UI) live even when every other
     * frame is a tiny partial spinner update: force exactly one full recompose
     * per wall-clock second.  ~1 full frame/s is negligible. */
    {
        static uint32_t last_sec = 0xFFFFFFFFu;
        uint32_t sec = pit_ms() / 1000u;
        if (sec != last_sec) { last_sec = sec; wm_mark_full(); }
    }

    int mx, my; uint8_t btn;
    bool mouse_moved = mouse_poll(&mx, &my, &btn);
    wm_handle_mouse(mx, my, btn);
    if (mouse_moved) {
        g_dirty = true;
        /* During an active drag/resize the handler already emits region damage
         * for the window footprint, so don't force full there.  Otherwise a
         * move over a hover/animation surface can change pixels beyond the
         * cursor cells -> full; over the static desktop the cursor cells are
         * enough. */
        if (!g_drag_win && !g_resize_win && mouse_over_hover_surface(mx, my))
            wm_mark_full();
    }

    /* TASK UI: pump scroll-wheel delta into the focused window. */
    if (mouse_has_wheel()) {
        int dz = mouse_scroll_delta();
        if (dz != 0) {
            window_t *focused = wm_focused();
            if (focused && focused->scroll_handler) {
                focused->scroll_handler(focused, dz);
                wm_mark_full();
            }
        }
    }

    if (!g_dirty) return;

    uint32_t now = pit_ms();
    bool active = (g_drag_win != NULL) || (g_resize_win != NULL);
    if (g_has_composed && !active &&
        (now - g_last_compose_ms < (1000u / WM_MAX_FPS))) {
        return;
    }
    g_has_composed    = true;
    g_last_compose_ms = now;

    wm_recompose(mx, my);
}

/* Silence unused-function diagnostics for helpers that may be unused by
 * static analysis but are kept for future use. */
static inline void wm_unused_silencer(void) {
    (void)max_int(0, 0);
    (void)min_int(0, 0);
}
__attribute__((unused)) static void (*const _wm_silence_keep)(void) = wm_unused_silencer;
