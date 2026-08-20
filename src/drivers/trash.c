/* ============================================================================
 * NexxoN OS - Recycle Bin ("Lomtar")  (v1.0)
 * ----------------------------------------------------------------------------
 * Backed by /sys/.trash/<inode>.bin + /sys/.trash/trashinfo.dat.  Standard
 * delete syscalls (sys_trash_file) intercept files instead of unlinking
 * them; sys_restore_file moves them back, sys_empty_trash purges.
 *
 * Because the NXFS layer doesn't have dot-prefixed name special handling,
 * we just create a regular directory called ".trash" - parsable like any
 * other - which is functionally what we need.
 *
 * GUI lists current trash contents with Restore + Permanent-Delete
 * actions.  Surfaced to the desktop as the "Lomtar" icon by desktop.c.
 * ============================================================================ */
#include "apps.h"
#include "window.h"
#include "icons.h"
#include "gfx.h"
#include "theme.h"   /* glass tokens for the app-level Aero sheen */
#include "font.h"
#include "string.h"
#include "debug.h"
#include "nxfs.h"
#include "mouse.h"
#include "dialogs.h"
#include "i18n.h"
#include "ctxmenu.h"

#define TRASH_MAX_ENTRIES 32
#define TRASH_DIR    ".trash"
#define TRASH_INFO   "trashinfo.dat"

typedef struct {
    bool   in_use;
    char   original_name[NXFS_NAME_MAX];
    char   storage_name [NXFS_NAME_MAX];
    uint32_t size;
} trash_entry_t;

static trash_entry_t g_trash[TRASH_MAX_ENTRIES];
static bool          g_loaded = false;

static uint32_t ensure_sys(void) {
    if (!nxfs_is_mounted()) return 0;
    uint32_t ino;
    if (nxfs_resolve(0, "sys", &ino) != NXFS_OK) {
        if (nxfs_create_dir(0, "sys", &ino) != NXFS_OK) return 0;
    }
    return ino;
}
static uint32_t ensure_trash(void) {
    uint32_t sys = ensure_sys();
    if (!sys) return 0;
    uint32_t ino;
    if (nxfs_resolve(sys, TRASH_DIR, &ino) != NXFS_OK) {
        if (nxfs_create_dir(sys, TRASH_DIR, &ino) != NXFS_OK) return 0;
    }
    return ino;
}

static void load_index(void) {
    if (g_loaded) return;
    memset(g_trash, 0, sizeof(g_trash));
    g_loaded = true;
    uint32_t tdir = ensure_trash();
    if (!tdir) return;
    uint32_t info;
    if (nxfs_resolve(tdir, TRASH_INFO, &info) != NXFS_OK) return;
    char buf[2048];
    uint32_t got = 0;
    if (nxfs_read_file(info, buf, sizeof(buf) - 1, &got) != NXFS_OK) return;
    buf[got] = 0;
    char *p = buf;
    int n = 0;
    while (*p && n < TRASH_MAX_ENTRIES) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol; *eol = 0;
        char *bar = strchr(p, '|');
        if (bar) {
            *bar = 0;
            strncpy(g_trash[n].storage_name, p, NXFS_NAME_MAX - 1);
            g_trash[n].storage_name[NXFS_NAME_MAX - 1] = 0;
            strncpy(g_trash[n].original_name, bar + 1, NXFS_NAME_MAX - 1);
            g_trash[n].original_name[NXFS_NAME_MAX - 1] = 0;
            g_trash[n].in_use = true;
            n++;
        }
        *eol = saved;
        if (saved == '\n') eol++;
        p = eol;
    }
    debug_printf("[trash] index loaded, %d entries\n", n);
}

static void save_index(void) {
    uint32_t tdir = ensure_trash();
    if (!tdir) return;
    uint32_t info;
    if (nxfs_resolve(tdir, TRASH_INFO, &info) != NXFS_OK) {
        if (nxfs_create_file(tdir, TRASH_INFO, &info) != NXFS_OK) return;
    }
    char buf[2048];
    uint32_t pos = 0;
    for (int i = 0; i < TRASH_MAX_ENTRIES; i++) {
        if (!g_trash[i].in_use) continue;
        int n = ksnprintf(buf + pos, sizeof(buf) - pos, "%s|%s\n",
                          g_trash[i].storage_name,
                          g_trash[i].original_name);
        if (n <= 0) break;
        pos += (uint32_t)n;
    }
    nxfs_write_file(info, buf, pos);
}

int trash_count(void) {
    load_index();
    int n = 0;
    for (int i = 0; i < TRASH_MAX_ENTRIES; i++) if (g_trash[i].in_use) n++;
    return n;
}

int trash_move_in(const char *src_path) {
    load_index();
    uint32_t tdir = ensure_trash();
    if (!tdir) return -1;
    /* Look up the source file in cwd. */
    uint32_t ino;
    if (nxfs_resolve(nxfs_cwd(), src_path, &ino) != NXFS_OK) return -1;
    /* Find free slot. */
    int slot = -1;
    for (int i = 0; i < TRASH_MAX_ENTRIES; i++) {
        if (!g_trash[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;
    /* Build storage name = "tNN_origname" so the on-disk file is unique. */
    char storage[NXFS_NAME_MAX];
    ksnprintf(storage, sizeof(storage), "t%02d_%s", slot, src_path);
    if (strlen(storage) >= NXFS_NAME_MAX) storage[NXFS_NAME_MAX - 1] = 0;

    /* Stream-copy the payload (v3 files can be arbitrarily large; the
     * old whole-buffer read silently truncated past 16 KiB). */
    static uint8_t buf[NXFS_LEGACY_BUF];
    uint32_t new_ino;
    if (nxfs_create_file(tdir, storage, &new_ino) != NXFS_OK) return -1;
    if (nxfs_write_begin(new_ino) != NXFS_OK) {
        nxfs_delete_file(tdir, storage);
        return -1;
    }
    uint64_t off = 0;
    for (;;) {
        int n = nxfs_read_at(ino, off, buf, sizeof(buf));
        if (n < 0) {
            nxfs_write_end(false);
            nxfs_delete_file(tdir, storage);
            return -1;
        }
        if (n == 0) break;
        if (nxfs_write_append(buf, (uint32_t)n) != NXFS_OK) {
            nxfs_delete_file(tdir, storage);
            return -1;
        }
        off += (uint32_t)n;
        wm_tick();
    }
    if (nxfs_write_end(true) != NXFS_OK) {
        nxfs_delete_file(tdir, storage);
        return -1;
    }
    uint32_t got = (off > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)off;
    /* Remove the original. */
    nxfs_delete_file(nxfs_cwd(), src_path);

    g_trash[slot].in_use = true;
    strncpy(g_trash[slot].storage_name,  storage,  NXFS_NAME_MAX - 1);
    g_trash[slot].storage_name [NXFS_NAME_MAX - 1] = 0;
    strncpy(g_trash[slot].original_name, src_path, NXFS_NAME_MAX - 1);
    g_trash[slot].original_name[NXFS_NAME_MAX - 1] = 0;
    g_trash[slot].size = got;
    save_index();
    debug_printf("[trash] moved '%s' to /sys/.trash/%s (%u bytes)\n",
                 src_path, storage, got);
    return 0;
}

int trash_restore(int idx) {
    if (idx < 0 || idx >= TRASH_MAX_ENTRIES) return -1;
    if (!g_trash[idx].in_use) return -1;
    uint32_t tdir = ensure_trash();
    uint32_t ino;
    if (nxfs_resolve(tdir, g_trash[idx].storage_name, &ino) != NXFS_OK) return -1;
    static uint8_t buf[NXFS_LEGACY_BUF];
    uint32_t new_ino;
    if (nxfs_create_file(nxfs_cwd(), g_trash[idx].original_name,
                         &new_ino) != NXFS_OK) {
        return -1;
    }
    if (nxfs_write_begin(new_ino) != NXFS_OK) return -1;
    uint64_t off = 0;
    for (;;) {
        int n = nxfs_read_at(ino, off, buf, sizeof(buf));
        if (n < 0) { nxfs_write_end(false); return -1; }
        if (n == 0) break;
        if (nxfs_write_append(buf, (uint32_t)n) != NXFS_OK) return -1;
        off += (uint32_t)n;
        wm_tick();
    }
    if (nxfs_write_end(true) != NXFS_OK) return -1;
    nxfs_delete_file(tdir, g_trash[idx].storage_name);
    g_trash[idx].in_use = false;
    save_index();
    return 0;
}

int trash_empty(void) {
    load_index();
    uint32_t tdir = ensure_trash();
    int removed = 0;
    for (int i = 0; i < TRASH_MAX_ENTRIES; i++) {
        if (!g_trash[i].in_use) continue;
        nxfs_delete_file(tdir, g_trash[i].storage_name);
        g_trash[i].in_use = false;
        removed++;
    }
    save_index();
    return removed;
}

/* ---- Recycle Bin GUI ------------------------------------------------- */
#define TR_W   460
#define TR_H   320
#define TR_BG  0xFFF0F0F4
#define TR_ROW 18

static window_t *g_tr_win = NULL;
static int       g_tr_sel = -1;

static void tr_redraw(void);

static bool tr_click(window_t *w, int cx, int cy,
                     uint8_t pressed, uint8_t btn) {
    if (w != g_tr_win) return false;
    if (!(pressed & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT))) return true;

    int list_top = 40;
    int list_bot = (int)w->content.height - 44;
    if (cy >= list_top && cy < list_bot) {
        int row = (cy - list_top) / TR_ROW;
        int real = -1; int seen = 0;
        for (int i = 0; i < TRASH_MAX_ENTRIES; i++) {
            if (!g_trash[i].in_use) continue;
            if (seen == row) { real = i; break; }
            seen++;
        }
        if (real >= 0) {
            g_tr_sel = real;
            tr_redraw();
            if (pressed & MOUSE_BTN_RIGHT) {
                ctxmenu_item_t items[2];
                memset(items, 0, sizeof(items));
                strncpy(items[0].label, i18n("btn.restore"), 23);
                items[0].id = 1;
                strncpy(items[1].label, i18n("btn.delete"), 23);
                items[1].id = 2;
                items[1].dangerous = true;
                int sx = g_tr_win->x + WM_BORDER + cx + 4;
                int sy = g_tr_win->y + WM_BORDER + WM_TITLE_H + 2 + cy + 4;
                (void)sx; (void)sy;
                (void)items;  /* simplified: handle directly with dialog */
            }
        }
        return true;
    }
    /* Buttons */
    int by = (int)w->content.height - 36;
    if (cy >= by && cy < by + 26) {
        if (cx >= 16 && cx < 16 + 110 && g_tr_sel >= 0) {
            trash_restore(g_tr_sel);
            g_tr_sel = -1;
            tr_redraw();
            return true;
        }
        if (cx >= 140 && cx < 140 + 130) {
            int n = trash_empty();
            char info_msg[64];
            ksnprintf(info_msg, sizeof(info_msg),
                      "Permanently removed %d item(s).", n);
            const char *info[] = { info_msg };
            dialog_info("Recycle Bin", info, 1);
            g_tr_sel = -1;
            tr_redraw();
            return true;
        }
    }
    (void)btn;
    return true;
}

static void tr_redraw(void) {
    if (!g_tr_win || !g_tr_win->in_use) { g_tr_win = NULL; return; }
    draw_target_t *t = &g_tr_win->content;
    gfx_clear(t, TR_BG);
    gfx_draw_string_aa(t, 12, 12, i18n("app.trash.title"),
                       0xFF002878, TR_BG);
    int row = 0;
    int n_items = 0;
    for (int i = 0; i < TRASH_MAX_ENTRIES; i++) {
        if (!g_trash[i].in_use) continue;
        n_items++;
        int y = 40 + row * TR_ROW;
        bool sel = (i == g_tr_sel);
        if (sel) {
            gfx_fill_rect(t, 0, y, (int)t->width, TR_ROW, 0xFF002878);
            gfx_blend_rect(t, 0, y, (int)t->width, 1, GLASS_EDGE_LIGHT);
            gfx_blend_rect(t, 0, y, (int)t->width, TR_ROW / 2, 0x18FFFFFFu);
        }
        char line[80];
        ksnprintf(line, sizeof(line), "%-30s %u B",
                  g_trash[i].original_name, g_trash[i].size);
        gfx_draw_string(t, 14, y + (TR_ROW - 8) / 2, line,
                        sel ? 0xFFFFFFFF : 0xFF101010,
                        sel ? 0xFF002878 : TR_BG);
        row++;
    }
    if (n_items == 0) {
        gfx_draw_string(t, 14, 50, i18n("msg.trash_empty"),
                        0xFF606080, TR_BG);
    }
    int by = (int)t->height - 36;
    const char *l1 = i18n("btn.restore");
    const char *l2 = i18n("btn.empty_trash");
    gfx_fill_rect(t, 16, by, 110, 26, 0xFF208030);
    gfx_draw_rect(t, 16, by, 110, 26, 0xFF000000);
    gfx_draw_string(t, 16 + (110 - (int)strlen(l1) * 8) / 2,
                    by + (26 - 8) / 2, l1, 0xFFFFFFFF, 0xFF208030);

    gfx_fill_rect(t, 140, by, 130, 26, 0xFF902020);
    gfx_draw_rect(t, 140, by, 130, 26, 0xFF000000);
    gfx_draw_string(t, 140 + (130 - (int)strlen(l2) * 8) / 2,
                    by + (26 - 8) / 2, l2, 0xFFFFFFFF, 0xFF902020);

    wm_mark_dirty();
}

static void tr_resize_cb(window_t *w) { (void)w; tr_redraw(); }
static void tr_destroy_cb(window_t *w) { if (w == g_tr_win) g_tr_win = NULL; }

bool trash_open(void) {
    load_index();
    if (g_tr_win && g_tr_win->in_use) {
        wm_set_focus(g_tr_win);
        tr_redraw();
        return true;
    }
    g_tr_win = wm_create_window(100, 90, TR_W, TR_H, L(STR_APP_TRASH));
    if (!g_tr_win) return false;
    wm_set_content_click(g_tr_win, tr_click, NULL);
    wm_set_resizable(g_tr_win, true, 360, 220);
    wm_set_resize_cb(g_tr_win, tr_resize_cb);
    wm_set_destroy_cb(g_tr_win, tr_destroy_cb);
    wm_set_icon(g_tr_win, ICON_TRASH);
    tr_redraw();
    return true;
}
