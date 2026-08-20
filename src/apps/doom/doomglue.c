/* ============================================================================
 * NexxoN OS - DOOM (PureDOOM core) application glue
 * ----------------------------------------------------------------------------
 * Runs id Software's DOOM (shareware IWAD embedded in the kernel image)
 * inside a NexxoN WM window:
 *
 *   * PureDOOM.h is the dependency-free single-file DOOM core; every
 *     platform service is injected below (print -> COM1, malloc -> a
 *     private 24 MiB arena, file IO -> an in-memory stream over the
 *     embedded doom1.wad, gettime -> the PIT, exit -> close the window).
 *   * The 320x200 RGBA frame is converted + integer-scaled into the
 *     window content every doomapp_tick() (pumped from the shell idle
 *     loop, like the browser/pong).  doom_update() self-paces to 35 fps.
 *   * Key input arrives through the keyboard driver's raw make/break tap
 *     (set-1 normalised), so DOOM gets real key-down/key-up pairs; when
 *     the window loses focus every held key is released.
 * ============================================================================ */
#include "types.h"
#include "gfx.h"
#include "window.h"
#include "icons.h"
#include "string.h"
#include "debug.h"
#include "pit.h"
#include "keyboard.h"
#include "i18n.h"
#include "doomapp.h"

/* PureDOOM: no DOOM_IMPLEMENT_* defines -> zero libc dependencies; all
 * platform services are runtime-injected via doom_set_*().
 *
 * The kernel's types.h makes true/false ENUMERATORS (not macros), so
 * PureDOOM's `#if !defined(false)` probe would redeclare them.  Define
 * self-referential macros so the probe sees them defined and PureDOOM
 * falls back to `typedef int doom_boolean`. */
#define false false
#define true  true
#define DOOM_IMPLEMENTATION
#include "PureDOOM.h"

/* ---------- private allocator arena -------------------------------------- */
/* DOOM wants ~12 MiB of zone memory on 64-bit plus WAD directory + screen
 * buffers.  First-fit free-list over a static arena (same pattern as the
 * WM framebuffer pool). */
#define DOOM_ARENA_BYTES (24u * 1024u * 1024u)
typedef struct dblk {
    uint32_t     size;
    uint32_t     in_use;
    struct dblk *next;
} dblk_t;
#define DBLK_SZ ((uint32_t)((sizeof(dblk_t) + 15u) & ~15u))

static uint8_t ALIGNED(16) g_doom_arena[DOOM_ARENA_BYTES];
static dblk_t *g_doom_pool = NULL;

static void arena_init(void) {
    g_doom_pool = (dblk_t *)g_doom_arena;
    g_doom_pool->size   = DOOM_ARENA_BYTES - DBLK_SZ;
    g_doom_pool->in_use = 0;
    g_doom_pool->next   = NULL;
}
static void *arena_malloc(int size) {
    if (size <= 0) return NULL;
    uint32_t need = ((uint32_t)size + 15u) & ~15u;
    for (dblk_t *b = g_doom_pool; b; b = b->next) {
        if (b->in_use || b->size < need) continue;
        if (b->size >= need + DBLK_SZ + 64u) {          /* split */
            dblk_t *nb = (dblk_t *)((uint8_t *)b + DBLK_SZ + need);
            nb->size   = b->size - need - DBLK_SZ;
            nb->in_use = 0;
            nb->next   = b->next;
            b->size = need;
            b->next = nb;
        }
        b->in_use = 1;
        return (uint8_t *)b + DBLK_SZ;
    }
    debug_printf("[doom] arena OOM (%d bytes)\n", size);
    return NULL;
}
static void arena_free(void *p) {
    if (!p) return;
    dblk_t *b = (dblk_t *)((uint8_t *)p - DBLK_SZ);
    b->in_use = 0;
    /* coalesce forward */
    for (dblk_t *c = g_doom_pool; c; c = c->next) {
        while (!c->in_use && c->next && !c->next->in_use) {
            c->size += DBLK_SZ + c->next->size;
            c->next  = c->next->next;
        }
    }
}

/* ---------- in-memory WAD file ------------------------------------------- */
extern const uint8_t _binary_assets_doom1_wad_start[];
extern const uint8_t _binary_assets_doom1_wad_end[];

typedef struct { int pos; int open; } memfile_t;
static memfile_t g_wad_handles[4];

static int wad_size(void) {
    return (int)(_binary_assets_doom1_wad_end - _binary_assets_doom1_wad_start);
}

static bool str_ends_with_wad(const char *fn) {
    /* match "doom1.wad" case-insensitively at the end of any path */
    int n = (int)strlen(fn);
    if (n < 9) return false;
    const char *tail = fn + n - 9;
    const char *ref  = "doom1.wad";
    for (int i = 0; i < 9; i++) {
        char c = tail[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != ref[i]) return false;
    }
    return true;
}

static void *shim_open(const char *filename, const char *mode) {
    if (!filename || !mode) return NULL;
    if (mode[0] != 'r') return NULL;          /* no writes (config/saves) */
    if (!str_ends_with_wad(filename)) return NULL;
    for (int i = 0; i < 4; i++) {
        if (!g_wad_handles[i].open) {
            g_wad_handles[i].open = 1;
            g_wad_handles[i].pos  = 0;
            return &g_wad_handles[i];
        }
    }
    return NULL;
}
static void shim_close(void *h) {
    if (h) ((memfile_t *)h)->open = 0;
}
static int shim_read(void *h, void *buf, int count) {
    memfile_t *f = (memfile_t *)h;
    if (!f || count <= 0) return 0;
    int left = wad_size() - f->pos;
    if (count > left) count = left;
    if (count <= 0) return 0;
    memcpy(buf, _binary_assets_doom1_wad_start + f->pos, (size_t)count);
    f->pos += count;
    return count;
}
static int shim_write(void *h, const void *buf, int count) {
    (void)h; (void)buf; (void)count;
    return -1;
}
static int shim_seek(void *h, int offset, doom_seek_t origin) {
    memfile_t *f = (memfile_t *)h;
    if (!f) return -1;
    int base = (origin == DOOM_SEEK_SET) ? 0
             : (origin == DOOM_SEEK_CUR) ? f->pos : wad_size();
    int np = base + offset;
    if (np < 0) np = 0;
    if (np > wad_size()) np = wad_size();
    f->pos = np;
    return 0;
}
static int shim_tell(void *h) {
    memfile_t *f = (memfile_t *)h;
    return f ? f->pos : -1;
}
static int shim_eof(void *h) {
    memfile_t *f = (memfile_t *)h;
    return (!f || f->pos >= wad_size()) ? 1 : 0;
}

/* ---------- misc shims ---------------------------------------------------- */
static void shim_print(const char *s) { debug_printf("[doom] %s", s); }

static void shim_gettime(int *sec, int *usec) {
    uint32_t ms = pit_ms();
    if (sec)  *sec  = (int)(ms / 1000u);
    if (usec) *usec = (int)((ms % 1000u) * 1000u);
}

static bool g_doom_dead = false;          /* exit / fatal error latch */
static void shim_exit(int code) {
    debug_printf("[doom] exit(%d) requested\n", code);
    g_doom_dead = true;
}
static char *shim_getenv(const char *var) {
    /* DOOM I_Errors out without a HOME (config/savegame directory).  Any
     * path works — writes are rejected by the file shim anyway. */
    static char home[] = "/";
    if (var && strcmp(var, "HOME") == 0) return home;
    return NULL;
}

/* ---------- WM window ----------------------------------------------------- */
#define DOOM_FB_W 320
#define DOOM_FB_H 200
#define DOOM_SCALE_DEFAULT 2

static window_t *g_doom_win  = NULL;
static bool      g_doom_init = false;
static bool      g_had_focus = false;

/* Track which doom keys are held so focus loss can release them all. */
static uint8_t g_key_held[256];

static void doom_release_all(void) {
    for (int i = 0; i < 256; i++) {
        if (g_key_held[i]) {
            doom_key_up((doom_key_t)i);
            g_key_held[i] = 0;
        }
    }
}

/* Set-1 make code -> doom_key_t (unshifted US ASCII for letters/digits). */
static int scan_to_doom(uint8_t make, bool ext) {
    if (ext) {
        switch (make) {
            case 0x48: return DOOM_KEY_UP_ARROW;
            case 0x50: return DOOM_KEY_DOWN_ARROW;
            case 0x4B: return DOOM_KEY_LEFT_ARROW;
            case 0x4D: return DOOM_KEY_RIGHT_ARROW;
            case 0x1D: return DOOM_KEY_CTRL;
            case 0x38: return DOOM_KEY_ALT;
            case 0x1C: return DOOM_KEY_ENTER;
            default:   return -1;
        }
    }
    static const char set1_ascii[0x40] = {
        0,   27, '1','2','3','4','5','6','7','8','9','0','-','=', 127, 9,
        'q','w','e','r','t','y','u','i','o','p','[',']', 13,  0, 'a','s',
        'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
        'b','n','m',',','.','/', 0, '*', 0, ' ', 0,  0,   0,  0,  0,  0,
    };
    switch (make) {
        case 0x1D: return DOOM_KEY_CTRL;
        case 0x2A: case 0x36: return DOOM_KEY_SHIFT;
        case 0x38: return DOOM_KEY_ALT;
        case 0x3B: return DOOM_KEY_F1;
        case 0x3C: return DOOM_KEY_F2;
        case 0x3D: return DOOM_KEY_F3;
        case 0x3E: return DOOM_KEY_F4;
        case 0x3F: return DOOM_KEY_F5;
        case 0x40: return DOOM_KEY_F6;
        case 0x41: return DOOM_KEY_F7;
        case 0x42: return DOOM_KEY_F8;
        case 0x43: return DOOM_KEY_F9;
        case 0x44: return DOOM_KEY_F10;
        default:
            if (make < 0x40 && set1_ascii[make]) return set1_ascii[make];
            return -1;
    }
}

/* Raw keyboard tap: fed by the IRQ decoder for every make/break.  Only
 * consumed while the DOOM window has focus. */
static void doom_raw_key(uint8_t make, bool ext, bool down) {
    if (!g_doom_win || !g_doom_win->in_use) return;
    if (wm_focused_id() != g_doom_win->id)  return;
    int k = scan_to_doom(make, ext);
    if (k < 0 || k > 255) return;
    if (down) {
        if (!g_key_held[k]) { doom_key_down((doom_key_t)k); g_key_held[k] = 1; }
    } else {
        if (g_key_held[k])  { doom_key_up((doom_key_t)k);   g_key_held[k] = 0; }
    }
}

/* Swallow the WM's cooked keystrokes while focused so they don't leak. */
static bool doom_wm_key(window_t *w, int k) {
    (void)w; (void)k;
    return true;
}

static void doom_destroy_cb(window_t *w) {
    if (w == g_doom_win) {
        g_doom_win = NULL;
        doom_release_all();
    }
}
static void doom_resize_cb(window_t *w) { (void)w; }

/* Blit the current DOOM frame into the window content: RGBA -> ARGB with
 * the largest integer scale that fits, centred, black bars around. */
static void doom_present(void) {
    draw_target_t *t = &g_doom_win->content;
    const unsigned char *fb = doom_get_framebuffer(4);
    if (!fb) return;
    int sx = (int)t->width  / DOOM_FB_W;
    int sy = (int)t->height / DOOM_FB_H;
    int s  = sx < sy ? sx : sy;
    if (s < 1) s = 1;
    int dw = DOOM_FB_W * s, dh = DOOM_FB_H * s;
    int ox = ((int)t->width - dw) / 2;
    int oy = ((int)t->height - dh) / 2;
    static int last_s = 0;
    if (s != last_s) {              /* scale changed -> clear the bars once */
        gfx_clear(t, 0xFF000000u);
        last_s = s;
    }
    for (int y = 0; y < DOOM_FB_H; y++) {
        uint32_t *row0 = (uint32_t *)(t->fb +
                         (uint32_t)(oy + y * s) * t->pitch) + ox;
        const unsigned char *src = fb + (size_t)y * DOOM_FB_W * 4;
        uint32_t *d = row0;
        for (int x = 0; x < DOOM_FB_W; x++) {
            uint32_t px = 0xFF000000u | ((uint32_t)src[0] << 16) |
                          ((uint32_t)src[1] << 8) | (uint32_t)src[2];
            for (int r = 0; r < s; r++) *d++ = px;
            src += 4;
        }
        /* duplicate the scaled row for the remaining s-1 lines */
        for (int r = 1; r < s; r++) {
            memcpy((uint8_t *)row0 + (uint32_t)r * t->pitch, row0,
                   (size_t)dw * 4u);
        }
    }
    wm_mark_dirty();
}

/* ---------- public API ----------------------------------------------------- */
bool doomapp_open(void) {
    if (g_doom_win && g_doom_win->in_use) {
        wm_set_focus(g_doom_win);
        return true;
    }
    if (g_doom_dead) {
        debug_printf("[doom] previous instance exited; not restarting\n");
        return false;
    }

    int cw = DOOM_FB_W * DOOM_SCALE_DEFAULT;
    int ch = DOOM_FB_H * DOOM_SCALE_DEFAULT;
    g_doom_win = wm_create_window(60, 40,
                                  cw + 2 * WM_BORDER,
                                  ch + WM_TITLE_H + 2 * WM_BORDER + 2,
                                  "DOOM");
    if (!g_doom_win) return false;
    wm_set_icon(g_doom_win, ICON_DOOM);
    wm_set_destroy_cb(g_doom_win, doom_destroy_cb);
    wm_set_key_handler(g_doom_win, doom_wm_key);
    wm_set_resizable(g_doom_win, true,
                     DOOM_FB_W + 2 * WM_BORDER,
                     DOOM_FB_H + WM_TITLE_H + 2 * WM_BORDER + 2);
    wm_set_resize_cb(g_doom_win, doom_resize_cb);
    gfx_clear(&g_doom_win->content, 0xFF000000u);

    if (!g_doom_init) {
        debug_printf("[doom] init: wad=%d bytes, arena=%u MiB\n",
                     wad_size(), DOOM_ARENA_BYTES / (1024u * 1024u));
        arena_init();
        memset(g_wad_handles, 0, sizeof(g_wad_handles));
        memset(g_key_held, 0, sizeof(g_key_held));

        doom_set_print(shim_print);
        doom_set_malloc(arena_malloc, arena_free);
        doom_set_file_io(shim_open, shim_close, shim_read, shim_write,
                         shim_seek, shim_tell, shim_eof);
        doom_set_gettime(shim_gettime);
        doom_set_exit(shim_exit);
        doom_set_getenv(shim_getenv);

        keyboard_set_raw_hook(doom_raw_key);

        static char arg0[] = "doom";
        static char *argv[] = { arg0, NULL };
        doom_init(1, argv, DOOM_FLAG_MENU_DARKEN_BG |
                            DOOM_FLAG_HIDE_SOUND_OPTIONS |
                            DOOM_FLAG_HIDE_MUSIC_OPTIONS |
                            DOOM_FLAG_HIDE_MOUSE_OPTIONS);
        g_doom_init = true;
        debug_printf("[doom] init complete\n");
    }
    wm_set_focus(g_doom_win);
    return true;
}

void doomapp_tick(void) {
    if (!g_doom_win || !g_doom_win->in_use || g_doom_dead) return;

    /* Release all held keys the moment focus moves elsewhere. */
    bool focused = (wm_focused_id() == g_doom_win->id);
    if (g_had_focus && !focused) doom_release_all();
    g_had_focus = focused;

    if (g_doom_win->minimized) return;

    doom_update();
    if (g_doom_win && g_doom_win->in_use)   /* exit may have closed us */
        doom_present();
}
