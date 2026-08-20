/* ============================================================================
 * NexxoN OS - "Programs" manager
 * ----------------------------------------------------------------------------
 * Lists, runs and removes user programs installed under /programs (NXFS).
 * A program is any file there; "Run" hands it to the Linux compat layer
 * (linux_run_path), so both native NexxoN ELFs and static Linux ELFs work.
 * "Install demo" writes the embedded demo Linux ELF into /programs so there
 * is always something to try.  Program stdout goes to the shell terminal.
 * ============================================================================ */
#include "programs.h"
#include "window.h"
#include "gfx.h"
#include "font.h"
#include "icons.h"
#include "string.h"
#include "i18n.h"
#include "nxfs.h"
#include "mouse.h"
#include "debug.h"
#include "theme.h"
#include "linuxsys.h"
#include "notify.h"

#define PG_W       520
#define PG_H       380
#define PG_HDR_H   48
#define PG_ROW_H   26
#define PG_PAD     14
#define PG_BTN_W   104
#define PG_BTN_H   26
#define PG_MAX     64

extern const uint8_t _binary_build_userland_linuxdemo_elf_start[];
extern const uint8_t _binary_build_userland_linuxdemo_elf_end[];
extern const uint8_t _binary_build_userland_linuxexec_elf_start[];
extern const uint8_t _binary_build_userland_linuxexec_elf_end[];
extern const uint8_t _binary_build_userland_linuxdyn_elf_start[];
extern const uint8_t _binary_build_userland_linuxdyn_elf_end[];
extern const uint8_t _binary_build_userland_linuxsh_elf_start[];
extern const uint8_t _binary_build_userland_linuxsh_elf_end[];
extern const uint8_t _binary_build_userland_musl_libc_so_start[];
extern const uint8_t _binary_build_userland_musl_libc_so_end[];

typedef struct { char name[NXFS_NAME_MAX]; uint32_t inode; uint32_t size; } pg_entry_t;

static window_t  *g_pg_win = NULL;
static pg_entry_t g_pg[PG_MAX];
static int        g_pg_n   = 0;
static int        g_pg_sel = -1;
static char       g_pg_status[96] = {0};

static void pg_redraw(void);

/* Ensure /programs exists; return its inode (or (uint32_t)-1). */
static uint32_t programs_dir(void) {
    uint32_t ino;
    if (nxfs_resolve(0, "programs", &ino) == NXFS_OK) return ino;
    if (nxfs_create_dir(0, "programs", &ino) == NXFS_OK) return ino;
    return (uint32_t)-1;
}

static void pg_scan_cb(const nxfs_inode_t *node, void *user) {
    (void)user;
    if (g_pg_n >= PG_MAX) return;
    if (node->type != NXFS_TYPE_FILE) return;
    pg_entry_t *e = &g_pg[g_pg_n++];
    strncpy(e->name, node->name, NXFS_NAME_MAX - 1);
    e->name[NXFS_NAME_MAX - 1] = 0;
    e->inode = 0;                 /* resolved on demand via path */
    e->size  = node->size;
}

static void pg_rescan(void) {
    g_pg_n = 0;
    uint32_t d = programs_dir();
    if (d != (uint32_t)-1) nxfs_list(d, pg_scan_cb, NULL);
    if (g_pg_sel >= g_pg_n) g_pg_sel = g_pg_n - 1;
}

static bool pg_install_one(uint32_t d, const char *name,
                           const uint8_t *start, const uint8_t *end) {
    uint32_t ino;
    if (nxfs_resolve(d, name, &ino) != NXFS_OK &&
        nxfs_create_file(d, name, &ino) != NXFS_OK)
        return false;
    return nxfs_write_file(ino, start, (uint32_t)(end - start)) == NXFS_OK;
}

static uint32_t lib_dir(void) {
    uint32_t ino;
    if (nxfs_resolve(0, "lib", &ino) == NXFS_OK) return ino;
    if (nxfs_create_dir(0, "lib", &ino) == NXFS_OK) return ino;
    return (uint32_t)-1;
}

static void pg_install_demo(void) {
    uint32_t d = programs_dir();
    if (d == (uint32_t)-1) {
        strcpy(g_pg_status, L(STR_PROG_DIR_FAILED));
        return;
    }
    uint32_t total = 0;
    uint32_t demo_len =
        (uint32_t)(_binary_build_userland_linuxdemo_elf_end -
                   _binary_build_userland_linuxdemo_elf_start);
    uint32_t exec_len =
        (uint32_t)(_binary_build_userland_linuxexec_elf_end -
                   _binary_build_userland_linuxexec_elf_start);
    uint32_t dyn_len =
        (uint32_t)(_binary_build_userland_linuxdyn_elf_end -
                   _binary_build_userland_linuxdyn_elf_start);
    uint32_t rt_len =
        (uint32_t)(_binary_build_userland_musl_libc_so_end -
                   _binary_build_userland_musl_libc_so_start);
    bool demo_ok = pg_install_one(d, "linuxdemo",
                                  _binary_build_userland_linuxdemo_elf_start,
                                  _binary_build_userland_linuxdemo_elf_end);
    bool exec_ok = pg_install_one(d, "linuxexec",
                                  _binary_build_userland_linuxexec_elf_start,
                                  _binary_build_userland_linuxexec_elf_end);
    bool dyn_ok = pg_install_one(d, "linuxdyn",
                                 _binary_build_userland_linuxdyn_elf_start,
                                 _binary_build_userland_linuxdyn_elf_end);
    bool sh_ok = pg_install_one(d, "linuxsh",
                                _binary_build_userland_linuxsh_elf_start,
                                _binary_build_userland_linuxsh_elf_end);
    uint32_t ld = lib_dir();
    bool ld_ok = false, libc_ok = false, binsh_ok = false;
    if (ld != (uint32_t)-1) {
        ld_ok = pg_install_one(ld, "ld-musl-x86_64.so.1",
                               _binary_build_userland_musl_libc_so_start,
                               _binary_build_userland_musl_libc_so_end);
        libc_ok = pg_install_one(ld, "libc.so",
                                 _binary_build_userland_musl_libc_so_start,
                                 _binary_build_userland_musl_libc_so_end);
    }
    uint32_t bin_ino;
    if (nxfs_resolve(0, "bin", &bin_ino) != NXFS_OK &&
        nxfs_create_dir(0, "bin", &bin_ino) == NXFS_OK)
        binsh_ok = pg_install_one(bin_ino, "sh",
                                  _binary_build_userland_linuxsh_elf_start,
                                  _binary_build_userland_linuxsh_elf_end);
    if (demo_ok) total += demo_len;
    if (exec_ok) total += exec_len;
    if (dyn_ok) total += dyn_len;
    if (sh_ok) total += (uint32_t)(_binary_build_userland_linuxsh_elf_end -
                                   _binary_build_userland_linuxsh_elf_start);
    if (ld_ok) total += rt_len;
    if (libc_ok) total += rt_len;
    if (demo_ok && exec_ok && dyn_ok && sh_ok && ld_ok && libc_ok && binsh_ok)
        ksnprintf(g_pg_status, sizeof(g_pg_status),
                  L(STR_PROG_INSTALLED_FULL_FMT), total);
    else if (demo_ok || exec_ok || dyn_ok)
        ksnprintf(g_pg_status, sizeof(g_pg_status),
                  L(STR_PROG_INSTALLED_TESTS_FMT), total);
    else
        strcpy(g_pg_status, L(STR_PROG_CREATE_FAILED));
    pg_rescan();
}

static void pg_run_sel(void) {
    if (g_pg_sel < 0 || g_pg_sel >= g_pg_n) return;
    char path[96];
    ksnprintf(path, sizeof(path), "/programs/%s", g_pg[g_pg_sel].name);
    char msg[96] = {0};
    /* Program stdout is written to the shell terminal; surface it there. */
    extern void wm_refocus_shell(void);
    wm_refocus_shell();
    int rc = linux_run_path(path, msg, sizeof(msg));
    ksnprintf(g_pg_status, sizeof(g_pg_status), L(STR_PROG_RESULT_FMT),
              g_pg[g_pg_sel].name,
              msg[0] ? msg : L(rc >= 0 ? STR_LINUX_OK : STR_LINUX_FAILED));
    notify_post(rc >= 0 ? NOTIFY_INFO : NOTIFY_WARNING,
                L(STR_APP_PROGRAMS), g_pg_status);
}

static void pg_remove_sel(void) {
    if (g_pg_sel < 0 || g_pg_sel >= g_pg_n) return;
    uint32_t d = programs_dir();
    if (d == (uint32_t)-1) return;
    if (nxfs_delete_file(d, g_pg[g_pg_sel].name) == NXFS_OK)
        ksnprintf(g_pg_status, sizeof(g_pg_status), L(STR_PROG_REMOVED_FMT),
                  g_pg[g_pg_sel].name);
    else
        strcpy(g_pg_status, L(STR_PROG_REMOVE_FAILED));
    pg_rescan();
}

/* ---- layout helpers ---------------------------------------------------- */
static void pg_btn_rects(int cw, int ch, int *bx, int *by) {
    (void)cw;
    *by = ch - PG_BTN_H - PG_PAD;
    *bx = PG_PAD;
}
static bool pg_hot(int bx, int by) {
    if (!g_pg_win) return false;
    int mx = mouse_x(), my = mouse_y();
    int ax = g_pg_win->x + WM_BORDER + bx;
    int ay = g_pg_win->y + WM_BORDER + WM_TITLE_H + 2 + by;
    return mx >= ax && mx < ax + PG_BTN_W && my >= ay && my < ay + PG_BTN_H;
}

static void pg_redraw(void) {
    if (!g_pg_win || !g_pg_win->in_use) { g_pg_win = NULL; return; }
    draw_target_t *t = &g_pg_win->content;
    int cw = (int)t->width, ch = (int)t->height;

    /* Frost gradient body. */
    for (int y = 0; y < ch; y++) {
        int num = ch > 1 ? y * 256 / (ch - 1) : 0;
        int r = 0xF4 + (0xE4 - 0xF4) * num / 256;
        int g = 0xF6 + (0xE9 - 0xF6) * num / 256;
        int b = 0xFA + (0xF1 - 0xFA) * num / 256;
        gfx_fill_rect(t, 0, y, cw, 1,
                      0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    }

    /* Glass navy header. */
    for (int y = 0; y < PG_HDR_H; y++) {
        int num = y * 256 / (PG_HDR_H - 1);
        int r = 0x10 + (0x1E - 0x10) * num / 256;
        int g = 0x20 + (0x50 - 0x20) * num / 256;
        int b = 0x58 + (0xA8 - 0x58) * num / 256;
        gfx_fill_rect(t, 0, y, cw, 1,
                      0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    }
    gfx_blend_rect(t, 0, 0, cw, PG_HDR_H / 2, 0x1EFFFFFFu);
    icon_draw(t, PG_PAD, 8, 30, ICON_PROGRAMS);
    gfx_draw_string_aa(t, PG_PAD + 38, 10, L(STR_PROG_HDR), 0xFFF2F6FBu, 0x00000000u);
    char cnt[64];
    ksnprintf(cnt, sizeof(cnt), L(STR_PROG_COUNT_FMT), g_pg_n);
    gfx_draw_string(t, PG_PAD + 38, 30, cnt, 0xFFA9BCE8u, 0x00000000u);

    /* List card. */
    int lx = PG_PAD, ly = PG_HDR_H + PG_PAD;
    int lw = cw - 2 * PG_PAD, lh = ch - PG_HDR_H - 3 * PG_PAD - PG_BTN_H;
    gfx_fill_round_rect(t, lx, ly, lw, lh, 6, 0xFFFFFFFFu);
    gfx_draw_round_rect(t, lx, ly, lw, lh, 6, 0xFFD9DFE8u);

    if (g_pg_n == 0) {
        gfx_draw_string_clipped(t, lx + 10, ly + 12, lw - 20,
                                L(STR_PROG_EMPTY), 0xFF66707Eu, 0x00000000u);
    } else {
        for (int i = 0; i < g_pg_n; i++) {
            int ry = ly + 4 + i * PG_ROW_H;
            if (ry + PG_ROW_H > ly + lh) break;
            if (i == g_pg_sel) {
                gfx_fill_round_rect(t, lx + 3, ry, lw - 6, PG_ROW_H - 2, 4, GLASS_ACCENT);
                gfx_blend_round_rect(t, lx + 4, ry + 1, lw - 8, (PG_ROW_H - 2) / 2, 4, 0x40FFFFFF);
            }
            uint32_t fg = (i == g_pg_sel) ? 0xFFFFFFFFu : 0xFF1C2836u;
            gfx_draw_string(t, lx + 12, ry + (PG_ROW_H - FONT_GLYPH_H) / 2 - 1,
                            g_pg[i].name, fg, 0x00000000u);
            char sz[24];
            ksnprintf(sz, sizeof(sz), L(STR_PROG_SIZE_BYTES_FMT), g_pg[i].size);
            int sw = (int)strlen(sz) * FONT_GLYPH_W;
            gfx_draw_string(t, lx + lw - sw - 12, ry + (PG_ROW_H - FONT_GLYPH_H) / 2 - 1,
                            sz, (i == g_pg_sel) ? 0xFFD8E4F4u : 0xFF8A94A2u, 0x00000000u);
        }
    }

    /* Button row: Run | Remove | Install demo | Refresh. */
    int bx, by;
    pg_btn_rects(cw, ch, &bx, &by);
    struct { lang_id_t s; uint32_t c; } btn[4] = {
        { STR_PROG_RUN,          GLASS_ACCENT },
        { STR_PROG_REMOVE,       0xFFB0463Cu  },
        { STR_PROG_INSTALL_DEMO, 0xFF2E9B57u  },
        { STR_PROG_REFRESH,      0xFF5A6B7Cu  },
    };
    for (int i = 0; i < 4; i++) {
        int x = bx + i * (PG_BTN_W + 8);
        gfx_draw_button_aero(t, x, by, PG_BTN_W, PG_BTN_H, L(btn[i].s),
                             btn[i].c, pg_hot(x, by));
    }
    if (g_pg_status[0])
        gfx_draw_string_clipped(t, PG_PAD, by - 18, cw - 2 * PG_PAD,
                                g_pg_status, 0xFF44506Cu, 0x00000000u);
    wm_mark_dirty();
}

static bool pg_click(window_t *w, int cx, int cy, uint8_t pressed, uint8_t btn) {
    (void)btn;
    if (w != g_pg_win || !(pressed & MOUSE_BTN_LEFT)) return false;
    int cw = (int)w->content.width, ch = (int)w->content.height;

    /* Buttons. */
    int bx, by;
    pg_btn_rects(cw, ch, &bx, &by);
    if (cy >= by && cy < by + PG_BTN_H) {
        for (int i = 0; i < 4; i++) {
            int x = bx + i * (PG_BTN_W + 8);
            if (cx >= x && cx < x + PG_BTN_W) {
                if      (i == 0) pg_run_sel();
                else if (i == 1) pg_remove_sel();
                else if (i == 2) pg_install_demo();
                else             pg_rescan();
                pg_redraw();
                return true;
            }
        }
    }

    /* List rows. */
    int ly = PG_HDR_H + PG_PAD;
    int lh = ch - PG_HDR_H - 3 * PG_PAD - PG_BTN_H;
    if (cy >= ly + 4 && cy < ly + lh) {
        int idx = (cy - ly - 4) / PG_ROW_H;
        if (idx >= 0 && idx < g_pg_n) { g_pg_sel = idx; pg_redraw(); return true; }
    }
    return false;
}

static void pg_resize_cb(window_t *w) { (void)w; pg_redraw(); }
static void pg_destroy_cb(window_t *w) { if (w == g_pg_win) g_pg_win = NULL; }
static void pg_lang_cb(lang_t l) {
    (void)l;
    g_pg_status[0] = 0;
    if (g_pg_win) pg_redraw();
}

bool programs_open(void) {
    if (g_pg_win && g_pg_win->in_use) {
        wm_set_focus(g_pg_win);
        pg_rescan();
        pg_redraw();
        return true;
    }
    g_pg_win = wm_create_window(120, 80, PG_W, PG_H, L(STR_APP_PROGRAMS));
    if (!g_pg_win) return false;
    wm_set_content_click(g_pg_win, pg_click, NULL);
    wm_set_resizable(g_pg_win, true, 420, 300);
    wm_set_resize_cb(g_pg_win, pg_resize_cb);
    wm_set_destroy_cb(g_pg_win, pg_destroy_cb);
    wm_set_icon(g_pg_win, ICON_PROGRAMS);
    static bool lang_reg = false;
    if (!lang_reg) { lang_register_cb(pg_lang_cb); lang_reg = true; }
    pg_rescan();
    pg_redraw();
    wm_set_focus(g_pg_win);
    return true;
}
