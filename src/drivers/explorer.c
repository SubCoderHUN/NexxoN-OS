/* ============================================================================
 * NexxoN OS - NexxoN Explorer implementation  (v2.0)
 * ----------------------------------------------------------------------------
 * GUI overhaul: Windows 7 / Vista inspired file manager layout (without
 * Aero transparencies).  The window content is split into four regions:
 *
 *   +---------------------------------------------------------------+
 *   |  Back  Forward  Up    Address: /sys/  ...........             |  ← nav bar
 *   +-----------------+---------------------------------------------+
 *   | Sidebar         |                                             |
 *   |  This PC        |   File list (folders first, then files)     |
 *   |  Local Disk     |                                             |
 *   |  Documents      |                                             |
 *   |  Downloads      |                                             |
 *   +-----------------+---------------------------------------------+
 *   |  Status: 5 items (3 folders, 2 files)                          |
 *   +---------------------------------------------------------------+
 *
 * Per-session navigation history powers Back / Forward; Up walks the
 * VFS parent_inode link.  The address bar text is editable but, since
 * NXFS lookups are inode-keyed and not path-keyed inside this app, we
 * keep the field as a "type to go" interactive control (Enter resolves
 * a "/sys/foo" style path against the NXFS root).
 *
 * All shipping strings funnel through lang_get(STR_*) — no raw English
 * literals live in the rendering loop, so a language switch repaints
 * the explorer in the new locale via the WM force-repaint callback.
 * ============================================================================ */
#include "explorer.h"
#include "window.h"
#include "icons.h"
#include "ctxmenu.h"
#include "nxfs.h"
#include "vfs.h"
#include "gfx.h"
#include "theme.h"   /* glass tokens for the app-level Aero sheen */
#include "i18n.h"
#include "font.h"
#include "vga.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "pit.h"
#include "shell.h"
#include "terminal.h"
#include "keyboard.h"
#include "dialogs.h"
#include "apps.h"      /* trash_move_in: route deletes through the Recycle Bin */
#include "editor.h"
#include "notify.h"
#include "pnp.h"

/* ---------- Window dimensions ----------------------------------------- */
#define XP_WIN_X        80
#define XP_WIN_Y        80
#define XP_WIN_W        620
#define XP_WIN_H        420
#define XP_MIN_W        420
#define XP_MIN_H        260

/* ---------- Layout (content-area, post-decoration coordinates) -------- */
#define XP_NAV_H        32
#define XP_STATUS_H     20
#define XP_SIDEBAR_W    140
#define XP_ROW_H        20
#define XP_PAD_L        6
#define XP_ICON_W       14
#define XP_ICON_H       14
#define XP_BTN_W        28
#define XP_ADDR_LBL_W   58

/* ---------- Colour palette (Windows 7 Aero-less light) ---------------- */
#define XP_BG           0xFFFFFFFF
#define XP_NAV_BG_TOP   0xFFE8EEF8
#define XP_NAV_BG_BOT   0xFFC8D4E8
#define XP_NAV_BORDER   0xFF7090B0
#define XP_NAV_FG       0xFF202830
#define XP_STATUS_BG    0xFFEDEDF0
#define XP_STATUS_FG    0xFF404048
#define XP_SIDEBAR_BG   0xFFF4F6FA
#define XP_SIDEBAR_BORDER 0xFFB0BCD0
#define XP_SIDEBAR_FG   0xFF202828
#define XP_SIDEBAR_HOT  0xFFC0DCF8
#define XP_SIDEBAR_HDR  0xFF003898
#define XP_DIR_FG       0xFF103080
#define XP_FILE_FG      0xFF202020
#define XP_SCRIPT_FG    0xFF806000
#define XP_PARENT_FG    0xFF306030
#define XP_HOT_BG       0xFFD0E4F8
#define XP_SEL_BG       0xFF90C0F0
#define XP_DIR_ICON     0xFFFFCC30
#define XP_FILE_ICON    0xFFE0E0E8
#define XP_SCRIPT_ICON  0xFFA060FF
#define XP_ICON_BORDER  0xFF606060
#define XP_BTN_BG       0xFFE0E8F4
#define XP_BTN_BG_H     0xFFC8D8F0
#define XP_BTN_BG_DIS   0xFFE8E8EC
#define XP_BTN_FG       0xFF202020
#define XP_BTN_FG_DIS   0xFFA0A0A8
#define XP_ADDR_BG      0xFFFFFFFF
#define XP_ADDR_FG      0xFF101010
#define XP_ADDR_BORDER  0xFF6080A0
#define XP_USB_ICON     0xFF30A030
#define XP_SIDEBAR_DIV  0xFFC0C8D8

/* ---------- Context menu item ids ------------------------------------- */
#define XP_CTX_OPEN        1
#define XP_CTX_DELETE      2
#define XP_CTX_RENAME      3
#define XP_CTX_PROPERTIES  4
#define XP_CTX_COPY        5
/* Background (empty-area) menu ids. */
#define XP_CTX_NEWDIR      6
#define XP_CTX_PASTE       7
#define XP_CTX_REFRESH     8
#define XP_CTX_NEWFILE     9

/* Synthetic entry slot for the parent ".." pseudo-entry. */
#define XP_PARENT_TAG      0xFFFFFFFFu

/* Maximum entries the explorer renders.  +1 for the synthetic ".." row. */
#define XP_MAX_ENTRIES  (NXFS_MAX_CHILDREN + 1)

/* History ring (per-session): each entry is the inode that the user
 * was browsing.  Capped to a small power-of-two so back/forward feels
 * snappy without bloating BSS. */
#define XP_HIST_MAX        32

typedef struct {
    bool         in_use;
    uint32_t     inode;
    uint8_t      type;
    char         name[NXFS_NAME_MAX];
    uint32_t     size;
} xp_entry_t;

typedef struct {
    const char *label_en;       /* English fallback */
    lang_id_t   label_id;
    const char *target_path;    /* path resolved against root each click */
    char        glyph;
    uint32_t    accent;
} sidebar_entry_t;

static const sidebar_entry_t g_sidebar[] = {
    { "This PC",     STR_XP_SIDEBAR_HOME, "",         'P', 0xFF1850C8 },
    { "Local Disk",  STR_XP_SIDEBAR_ROOT, "",         'C', 0xFF205088 },
    { "System",      STR_XP_SIDEBAR_SYS,  "sys",      'S', 0xFFE08020 },
    { "Documents",   STR_XP_SIDEBAR_USB,  "docs",     'D', 0xFF208030 },
    { "Downloads",   STR_XP_SIDEBAR_USB,  "downloads",'L', 0xFF802080 },
};
#define XP_SIDEBAR_COUNT ((int)(sizeof(g_sidebar) / sizeof(g_sidebar[0])))

/* Hungarian-friendly localised sidebar labels.  Mapping by index since
 * the existing STR_XP_SIDEBAR_* constants don't cover every entry. */
static const char *sidebar_label(int idx) {
    switch (idx) {
        case 0: return lang_get(STR_XP_SIDEBAR_HOME); /* "Saját" / "This PC" */
        case 1:
            return (i18n_get_language() == LANG_HU)
                ? "Helyi lemez (C:)" : "Local Disk (C:)";
        case 2:
            return (i18n_get_language() == LANG_HU)
                ? "Rendszer" : "System";
        case 3:
            return (i18n_get_language() == LANG_HU)
                ? "Dokumentumok" : "Documents";
        case 4:
            return (i18n_get_language() == LANG_HU)
                ? "Letöltések" : "Downloads";
        default: return "";
    }
}

static struct {
    window_t   *win;
    uint32_t    cwd_ino;
    int         entry_count;
    int         selected;
    int         scroll_top;
    uint32_t    last_click_ms;
    int         last_click_idx;
    xp_entry_t  entries[XP_MAX_ENTRIES];

    /* Navigation history.  hist[hist_pos] = current cwd; back decrements,
     * forward increments.  Pushes truncate the forward tail. */
    uint32_t    hist[XP_HIST_MAX];
    int         hist_count;
    int         hist_pos;

    /* Address bar state. */
    char        addr_text[256];
    int         addr_caret;
    bool        addr_focus;

    /* VFS browsing (USB drives). */
    bool        browsing_vfs;
    char        vfs_mount_name[VFS_NAME_MAX];
    char        vfs_subpath[256];
} g_xp = { 0 };

#define DBLCLICK_MS  450u

/* ---------- Helpers --------------------------------------------------- */
static bool stale_window(void) {
    if (!g_xp.win) return true;
    if (!g_xp.win->in_use) {
        g_xp.win = NULL;
        return true;
    }
    return false;
}

static bool name_is_script(const char *name) {
    size_t n = strlen(name);
    if (n < 4) return false;
    return name[n-3] == '.' && name[n-2] == 'n' && name[n-1] == 'x';
}

static void rebuild_entries(void);      /* defined below */
static void redraw(void);               /* defined below */

static bool eq_names_ci(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Case-insensitive "ends with .ext" check for the open dispatch. */
static bool name_has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    if (n < e + 2 || name[n - e - 1] != '.') return false;
    for (size_t i = 0; i < e; i++) {
        char a = name[n - e + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

/* Absolute VFS path of the directory being browsed, optionally with a
 * child name appended ("/usb0/photos" + "cat.jpg" -> "/usb0/photos/cat.jpg"). */
static void vfs_full_path(char *out, size_t cap, const char *child) {
    size_t n = (size_t)ksnprintf(out, cap, "/%s%s",
                                 g_xp.vfs_mount_name, g_xp.vfs_subpath);
    if (child && child[0] && n + strlen(child) + 2 < cap) {
        if (n == 0 || out[n - 1] != '/') { out[n++] = '/'; out[n] = 0; }
        strcat(out, child);
    }
}

/* ---------- Copy/paste (one file, across nxfs <-> USB volumes) ---------- *
 * The clipboard remembers WHERE the source lives; the actual bytes move at
 * paste time through a dedicated static buffer (the kernel has no general
 * heap for multi-MiB blobs).  Files above the buffer size are refused with
 * a clear message instead of writing a silently truncated copy. */
#define XP_COPY_MAX  (2u * 1024u * 1024u)
static uint8_t g_xp_copybuf[XP_COPY_MAX];
static struct {
    bool     valid;
    bool     from_vfs;
    char     vfs_path[352];     /* absolute "/usbN/dir/file" when from_vfs */
    uint32_t nxfs_ino;          /* source inode otherwise                  */
    char     name[NXFS_NAME_MAX];
} g_xp_clip = { 0 };

static void xp_notify_error(lang_id_t why) {
    const char *line = lang_get(why);
    dialog_info(lang_get(STR_XP_OP_FAILED), &line, 1);
}

static void do_copy(int idx, xp_entry_t *e) {
    if (e->inode == XP_PARENT_TAG || e->type == NXFS_TYPE_DIR) return;
    g_xp_clip.valid    = true;
    g_xp_clip.from_vfs = g_xp.browsing_vfs;
    if (g_xp.browsing_vfs) {
        vfs_full_path(g_xp_clip.vfs_path, sizeof(g_xp_clip.vfs_path), e->name);
    } else {
        g_xp_clip.nxfs_ino = e->inode;
    }
    strncpy(g_xp_clip.name, e->name, sizeof(g_xp_clip.name) - 1);
    g_xp_clip.name[sizeof(g_xp_clip.name) - 1] = 0;
    char msg[96];
    ksnprintf(msg, sizeof(msg), lang_get(STR_XP_COPIED), g_xp_clip.name);
    notify_post(NOTIFY_INFO, lang_get(STR_APP_EXPLORER), msg);
    (void)idx;
}

/* Does `name` already exist in the directory being browsed? */
static bool name_exists_here(const char *name) {
    for (int i = 0; i < g_xp.entry_count; i++) {
        if (g_xp.entries[i].inode == XP_PARENT_TAG) continue;
        if (eq_names_ci(g_xp.entries[i].name, name)) return true;
    }
    return false;
}

/* Keeps the compositor + native USB input alive between copy chunks -
 * multi-MiB streamed copies would otherwise freeze the UI exactly like
 * any other modal loop that forgets to pump. */
static void xp_copy_pump(void) {
    pnp_tick();
    wm_tick();
}

static void do_paste(void) {
    static bool pasting = false;        /* wm_tick pumps input: re-entry
                                         * could start a second stream  */
    if (pasting) return;
    if (!g_xp_clip.valid) return;
    if (g_xp.browsing_vfs && !vfs_writable(g_xp.vfs_mount_name)) {
        xp_notify_error(STR_XP_READ_ONLY);
        return;
    }
    if (name_exists_here(g_xp_clip.name)) {
        xp_notify_error(STR_XP_NAME_TAKEN);
        return;
    }

    /* VFS -> VFS (FAT/exFAT both ends): streamed copy, NO size cap.
     * The 2 MiB buffer is reused as the chunk window.  Falls through to
     * the buffered path when a backend lacks the ops (NTFS source). */
    if (g_xp_clip.from_vfs && g_xp.browsing_vfs) {
        char dst[352];
        vfs_full_path(dst, sizeof(dst), g_xp_clip.name);
        if (vfs_can_stream_copy(g_xp_clip.vfs_path, dst)) {
            pasting = true;
            int r2 = vfs_copy_streamed(g_xp_clip.vfs_path, dst,
                                       g_xp_copybuf, 256u * 1024u,
                                       xp_copy_pump);
            pasting = false;
            if (r2 < 0) {
                xp_notify_error(STR_XP_OP_FAILED);
                return;
            }
            char msg[96];
            ksnprintf(msg, sizeof(msg), lang_get(STR_XP_PASTE_DONE),
                      g_xp_clip.name);
            notify_post(NOTIFY_SUCCESS, lang_get(STR_APP_EXPLORER), msg);
            rebuild_entries();
            redraw();
            return;
        }
    }

    int n;
    if (g_xp_clip.from_vfs) {
        n = vfs_read(g_xp_clip.vfs_path, g_xp_copybuf, XP_COPY_MAX);
        if (n > 0 && (uint32_t)n >= XP_COPY_MAX) {
            /* The whole buffer filled: the file may be larger than what we
             * read — refuse rather than write a truncated copy. */
            xp_notify_error(STR_XP_FILE_TOO_BIG);
            return;
        }
    } else {
        uint32_t got = 0;
        if (nxfs_read_file(g_xp_clip.nxfs_ino, g_xp_copybuf,
                           XP_COPY_MAX, &got) != NXFS_OK) {
            xp_notify_error(STR_XP_OP_FAILED);
            return;
        }
        n = (int)got;
    }
    if (n < 0) { xp_notify_error(STR_XP_OP_FAILED); return; }

    int r;
    if (g_xp.browsing_vfs) {
        char dst[352];
        vfs_full_path(dst, sizeof(dst), g_xp_clip.name);
        r = vfs_write_file(dst, g_xp_copybuf, (uint32_t)n);
    } else {
        uint32_t ino = 0;
        r = (nxfs_create_file(g_xp.cwd_ino, g_xp_clip.name, &ino) == NXFS_OK &&
             nxfs_write_file(ino, g_xp_copybuf, (uint32_t)n) == NXFS_OK) ? 0 : -1;
    }
    if (r < 0) {
        xp_notify_error(STR_XP_OP_FAILED);
        return;
    }
    char msg[96];
    ksnprintf(msg, sizeof(msg), lang_get(STR_XP_PASTE_DONE), g_xp_clip.name);
    notify_post(NOTIFY_SUCCESS, lang_get(STR_APP_EXPLORER), msg);
    rebuild_entries();
    redraw();
}

static uint32_t row_color(const xp_entry_t *e) {
    if (e->inode == XP_PARENT_TAG)            return XP_PARENT_FG;
    if (e->type == NXFS_TYPE_DIR)             return XP_DIR_FG;
    if (name_is_script(e->name))              return XP_SCRIPT_FG;
    return XP_FILE_FG;
}

static uint32_t icon_color(const xp_entry_t *e) {
    if (e->inode == XP_PARENT_TAG)            return XP_PARENT_FG;
    if (e->type == NXFS_TYPE_DIR)             return XP_DIR_ICON;
    if (name_is_script(e->name))              return XP_SCRIPT_ICON;
    return XP_FILE_ICON;
}

/* ---------- History --------------------------------------------------- */
static void hist_push(uint32_t ino) {
    /* Truncate forward tail. */
    if (g_xp.hist_pos < g_xp.hist_count - 1) {
        g_xp.hist_count = g_xp.hist_pos + 1;
    }
    /* Drop duplicate of current. */
    if (g_xp.hist_count > 0 && g_xp.hist[g_xp.hist_count - 1] == ino) return;
    if (g_xp.hist_count >= XP_HIST_MAX) {
        for (int i = 1; i < XP_HIST_MAX; i++) {
            g_xp.hist[i - 1] = g_xp.hist[i];
        }
        g_xp.hist_count = XP_HIST_MAX - 1;
        if (g_xp.hist_pos > 0) g_xp.hist_pos--;
    }
    g_xp.hist[g_xp.hist_count++] = ino;
    g_xp.hist_pos = g_xp.hist_count - 1;
}

static bool hist_can_back(void)    { return g_xp.hist_pos > 0; }
static bool hist_can_forward(void) { return g_xp.hist_pos + 1 < g_xp.hist_count; }

/* ---------- Entry table construction ---------------------------------- */
static void rebuild_entries_vfs(void) {
    g_xp.entry_count = 0;
    g_xp.selected    = -1;
    g_xp.scroll_top  = 0;

    /* ".." entry to go back. */
    xp_entry_t *p = &g_xp.entries[g_xp.entry_count++];
    p->in_use = true;
    p->inode  = XP_PARENT_TAG;
    p->type   = NXFS_TYPE_DIR;
    strcpy(p->name, "..");
    p->size   = 0;

    char full[320];
    ksnprintf(full, sizeof(full), "/%s%s", g_xp.vfs_mount_name, g_xp.vfs_subpath);

    vfs_entry_t ve[64];
    int n = vfs_list(full, ve, 64);
    if (n < 0) n = 0;

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            if (pass == 0 && !ve[i].is_dir) continue;
            if (pass == 1 &&  ve[i].is_dir) continue;
            if (strcmp(ve[i].name, ".") == 0 || strcmp(ve[i].name, "..") == 0) continue;
            if (g_xp.entry_count >= XP_MAX_ENTRIES) break;
            xp_entry_t *e = &g_xp.entries[g_xp.entry_count++];
            e->in_use = true;
            e->inode  = 0;
            e->type   = ve[i].is_dir ? NXFS_TYPE_DIR : NXFS_TYPE_FILE;
            strncpy(e->name, ve[i].name, NXFS_NAME_MAX - 1);
            e->name[NXFS_NAME_MAX - 1] = 0;
            e->size = ve[i].size;
        }
    }

    ksnprintf(g_xp.addr_text, sizeof(g_xp.addr_text),
              "/%s%s", g_xp.vfs_mount_name, g_xp.vfs_subpath);
    g_xp.addr_caret = (int)strlen(g_xp.addr_text);
    debug_printf("[explorer] rebuild VFS: /%s%s, %d entries\n",
                 g_xp.vfs_mount_name, g_xp.vfs_subpath, g_xp.entry_count);
}

static void rebuild_entries(void) {
    if (g_xp.browsing_vfs) { rebuild_entries_vfs(); return; }

    g_xp.entry_count = 0;
    g_xp.selected    = -1;
    g_xp.scroll_top  = 0;

    nxfs_inode_t dir;
    if (nxfs_read_inode(g_xp.cwd_ino, &dir) != NXFS_OK) {
        debug_printf("[explorer] failed to read cwd inode %u\n", g_xp.cwd_ino);
        return;
    }

    if (dir.parent_inode != g_xp.cwd_ino) {
        xp_entry_t *p = &g_xp.entries[g_xp.entry_count++];
        p->in_use = true;
        p->inode  = XP_PARENT_TAG;
        p->type   = NXFS_TYPE_DIR;
        strcpy(p->name, "..");
        p->size   = 0;
    }

    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < dir.child_count; i++) {
            nxfs_inode_t ch;
            if (nxfs_read_inode(dir.children[i], &ch) != NXFS_OK) continue;
            bool is_dir = (ch.type == NXFS_TYPE_DIR);
            if (pass == 0 && !is_dir) continue;
            if (pass == 1 &&  is_dir) continue;
            if (g_xp.entry_count >= XP_MAX_ENTRIES) break;
            xp_entry_t *e = &g_xp.entries[g_xp.entry_count++];
            e->in_use = true;
            e->inode  = dir.children[i];
            e->type   = (uint8_t)ch.type;
            strncpy(e->name, ch.name, NXFS_NAME_MAX - 1);
            e->name[NXFS_NAME_MAX - 1] = 0;
            e->size   = ch.size;
        }
    }

    /* Refresh address bar text to mirror the cwd. */
    if (nxfs_pwd_path(g_xp.addr_text, sizeof(g_xp.addr_text)) != NXFS_OK) {
        strcpy(g_xp.addr_text, "/");
    }
    g_xp.addr_caret = (int)strlen(g_xp.addr_text);
    debug_printf("[explorer] rebuild: cwd inode %u, %d entries\n",
                 g_xp.cwd_ino, g_xp.entry_count);
}

/* ---------- Drawing helpers ------------------------------------------- */
static uint32_t lerp_ch_xp(uint32_t a, uint32_t b, int num, int den) {
    if (den <= 0) return a;
    return (uint32_t)((int32_t)a + (int32_t)(b - a) * num / den);
}
static uint32_t lerp_xp(uint32_t a, uint32_t b, int num, int den) {
    uint32_t aa = (a >> 24) & 0xFF, ab = (b >> 24) & 0xFF;
    uint32_t ra = (a >> 16) & 0xFF, rb = (b >> 16) & 0xFF;
    uint32_t ga = (a >>  8) & 0xFF, gb = (b >>  8) & 0xFF;
    uint32_t ba = (a      ) & 0xFF, bb = (b      ) & 0xFF;
    return (lerp_ch_xp(aa, ab, num, den) << 24) |
           (lerp_ch_xp(ra, rb, num, den) << 16) |
           (lerp_ch_xp(ga, gb, num, den) <<  8) |
            lerp_ch_xp(ba, bb, num, den);
}
static void fill_vgrad_xp(draw_target_t *t, int x, int y, int w, int h,
                          uint32_t top, uint32_t bot) {
    if (h <= 0) return;
    for (int r = 0; r < h; r++) {
        uint32_t c = lerp_xp(top, bot, r, h - 1);
        gfx_draw_hline(t, x, y + r, w, c);
    }
}

/* Folder / file icon - simple 14x14 pictograms. */
static void draw_dir_icon(draw_target_t *t, int x, int y) {
    /* Folder tab */
    gfx_fill_rect(t, x + 1, y + 2, 5, 2, XP_DIR_ICON);
    gfx_fill_rect(t, x, y + 4, XP_ICON_W, XP_ICON_H - 4, XP_DIR_ICON);
    /* Highlight */
    gfx_draw_hline(t, x, y + 4, XP_ICON_W, 0xFFFFE890);
    gfx_draw_rect (t, x, y + 4, XP_ICON_W, XP_ICON_H - 4, XP_ICON_BORDER);
    gfx_draw_hline(t, x + 1, y + 2, 5, XP_ICON_BORDER);
}
/* File sheet with a tintable folded corner + content lines (type-aware). */
static void draw_file_icon_t(draw_target_t *t, int x, int y,
                             uint32_t corner, uint32_t lines) {
    gfx_fill_rect(t, x + 1, y, XP_ICON_W - 1, XP_ICON_H, XP_FILE_ICON);
    gfx_fill_rect(t, x + XP_ICON_W - 5, y, 5, 4, corner);
    gfx_draw_rect(t, x + 1, y, XP_ICON_W - 1, XP_ICON_H, XP_ICON_BORDER);
    for (int i = 0; i < 3; i++)
        gfx_fill_rect(t, x + 3, y + 5 + i * 3, XP_ICON_W - 7, 1, lines);
}
static void draw_file_icon(draw_target_t *t, int x, int y) {
    draw_file_icon_t(t, x, y, 0xFFC0C0C8u, 0xFF606070u);
}
static void draw_script_icon(draw_target_t *t, int x, int y) {
    draw_file_icon(t, x, y);
    gfx_fill_rect(t, x + 3, y + 9, 7, 3, XP_SCRIPT_ICON);
}

static bool xp_ends(const char *s, const char *suf) {
    int ls = (int)strlen(s), lf = (int)strlen(suf);
    if (lf > ls) return false;
    for (int i = 0; i < lf; i++) {
        char a = s[ls - lf + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static void draw_entry_icon(draw_target_t *t, int x, int y, const xp_entry_t *e) {
    if (e->inode == XP_PARENT_TAG || e->type == NXFS_TYPE_DIR) {
        draw_dir_icon(t, x, y);
    } else if (name_is_script(e->name)) {
        draw_script_icon(t, x, y);
    } else if (xp_ends(e->name, ".txt")) {
        draw_file_icon_t(t, x, y, 0xFF2A6FD0u, 0xFF3A66A0u);          /* blue doc  */
    } else if (xp_ends(e->name, ".cfg")) {
        draw_file_icon_t(t, x, y, 0xFFE08020u, 0xFF8A6A40u);          /* orange    */
        gfx_fill_rect(t, x + XP_ICON_W / 2 - 1, y + XP_ICON_H / 2, 4, 4, 0xFFE08020u);
    } else {
        draw_file_icon(t, x, y);
    }
}

static void draw_button(draw_target_t *t, int x, int y, int w, int h,
                        const char *label, bool enabled, bool hot) {
    /* Shared Aero button (light neutral body -> auto dark label); dimmed when
     * disabled so the whole system's buttons match. */
    uint32_t base = enabled ? 0xFFD6DEEAu : 0xFFC2C8D2u;
    gfx_draw_button_aero(t, x, y, w, h, label, base, hot && enabled);
}

static void draw_nav_bar(draw_target_t *t) {
    fill_vgrad_xp(t, 0, 0, (int)t->width, XP_NAV_H, XP_NAV_BG_TOP, XP_NAV_BG_BOT);
    gfx_blend_rect(t, 0, 0, (int)t->width, 1, GLASS_GLOSS);          /* top gloss   */
    gfx_blend_rect(t, 0, XP_NAV_H - 1, (int)t->width, 1, GLASS_EDGE_DARK); /* depth */

    int bx = 6, by = 4;
    int bh = XP_NAV_H - 8;
    /* Back / Forward / Up icons - we draw simple arrows directly. */
    draw_button(t, bx,          by, XP_BTN_W, bh, "<", hist_can_back(), false);
    draw_button(t, bx + XP_BTN_W + 4, by, XP_BTN_W, bh, ">", hist_can_forward(), false);
    draw_button(t, bx + 2 * (XP_BTN_W + 4), by, XP_BTN_W, bh, "^", true, false);

    int ax = bx + 3 * (XP_BTN_W + 4) + 6;
    gfx_draw_string(t, ax, by + (bh - FONT_GLYPH_H) / 2,
                    lang_get(STR_XP_ADDR_LABEL),
                    XP_NAV_FG, lerp_xp(XP_NAV_BG_TOP, XP_NAV_BG_BOT, 1, 2));
    int addr_x = ax + XP_ADDR_LBL_W;
    int addr_w = (int)t->width - addr_x - 8;
    int addr_y = by + 2;
    int addr_h = bh - 4;
    gfx_fill_rect(t, addr_x, addr_y, addr_w, addr_h, XP_ADDR_BG);
    gfx_draw_rect(t, addr_x, addr_y, addr_w, addr_h,
                  g_xp.addr_focus ? 0xFF1850C8 : XP_ADDR_BORDER);

    int max_chars = (addr_w - 8) / FONT_GLYPH_W;
    int slen = gfx_string_glyph_count(g_xp.addr_text);
    int start = 0;
    if (slen > max_chars) start = slen - max_chars;
    /* For UTF-8 strings we can't byte-index — but VFS paths are all
     * ASCII so this slice is safe.  We still use byte indexing here
     * because address bar paths are typed by the user and stored as
     * ASCII. */
    const char *display = g_xp.addr_text;
    if (start > 0 && (int)strlen(g_xp.addr_text) > start) {
        display = g_xp.addr_text + start;
    }
    gfx_draw_string(t, addr_x + 4,
                    addr_y + (addr_h - FONT_GLYPH_H) / 2,
                    display, XP_ADDR_FG, XP_ADDR_BG);
    if (g_xp.addr_focus) {
        int caret_x = addr_x + 4 + (g_xp.addr_caret - start) * FONT_GLYPH_W;
        if (caret_x >= addr_x + 4 && caret_x < addr_x + addr_w - 4) {
            gfx_fill_rect(t, caret_x, addr_y + 3, 1, addr_h - 6, XP_ADDR_FG);
        }
    }
}

static int sidebar_usb_count(void) { return vfs_mount_count(); }

static int sidebar_total_count(void) {
    return XP_SIDEBAR_COUNT + sidebar_usb_count();
}

static void draw_sidebar(draw_target_t *t) {
    int top = XP_NAV_H;
    int bot = (int)t->height - XP_STATUS_H;
    int h = bot - top;
    gfx_fill_rect(t, 0, top, XP_SIDEBAR_W, h, XP_SIDEBAR_BG);
    gfx_draw_vline(t, XP_SIDEBAR_W - 1, top, h, XP_SIDEBAR_BORDER);

    gfx_draw_string_aa(t, 10, top + 8,
                       i18n_get_language() == LANG_HU ? "Helyek" : "Locations",
                       XP_SIDEBAR_HDR, XP_SIDEBAR_BG);
    int y = top + 28;

    int mx = mouse_x(), my = mouse_y();
    int abs_w_x = g_xp.win->x + WM_BORDER;
    int abs_w_y = g_xp.win->y + WM_BORDER + WM_TITLE_H + 2;

    for (int i = 0; i < XP_SIDEBAR_COUNT; i++) {
        int row_top = y + i * 26;
        int row_h   = 24;
        bool hot = (mx >= abs_w_x + 2 && mx < abs_w_x + XP_SIDEBAR_W - 2 &&
                    my >= abs_w_y + row_top && my < abs_w_y + row_top + row_h);
        if (hot) {
            gfx_fill_round_rect(t, 2, row_top, XP_SIDEBAR_W - 4, row_h, RADIUS_HOVER, XP_SIDEBAR_HOT);
            gfx_blend_rect(t, 4, row_top + 1, XP_SIDEBAR_W - 8, row_h / 2, GLASS_GLOSS);
        }
        gfx_fill_rect(t, 8, row_top + 4, 16, 16, g_sidebar[i].accent);
        gfx_draw_rect(t, 8, row_top + 4, 16, 16, 0xFF202028);
        char ch[2] = { g_sidebar[i].glyph, 0 };
        gfx_draw_string(t, 12, row_top + 8, ch, 0xFFFFFFFF, g_sidebar[i].accent);
        gfx_draw_string_aa_clipped(t, 30, row_top + (row_h - FONT_GLYPH_H) / 2,
                                XP_SIDEBAR_W - 36,
                                sidebar_label(i), XP_SIDEBAR_FG,
                                hot ? XP_SIDEBAR_HOT : XP_SIDEBAR_BG);
    }

    int usb_n = sidebar_usb_count();
    if (usb_n > 0) {
        int div_y = y + XP_SIDEBAR_COUNT * 26 + 2;
        gfx_draw_hline(t, 8, div_y, XP_SIDEBAR_W - 16, XP_SIDEBAR_DIV);
        gfx_draw_string_aa(t, 10, div_y + 6,
                           L(STR_XP_SIDEBAR_DRIVES),
                           XP_SIDEBAR_HDR, XP_SIDEBAR_BG);
        int uy = div_y + 22;
        for (int u = 0; u < usb_n; u++) {
            vfs_mount_t *m = vfs_get_mount(u);
            if (!m) continue;
            int row_top = uy + u * 26;
            int row_h   = 24;
            bool hot = (mx >= abs_w_x + 2 && mx < abs_w_x + XP_SIDEBAR_W - 2 &&
                        my >= abs_w_y + row_top && my < abs_w_y + row_top + row_h);
            if (hot) {
                gfx_fill_round_rect(t, 2, row_top, XP_SIDEBAR_W - 4, row_h, RADIUS_HOVER, XP_SIDEBAR_HOT);
            gfx_blend_rect(t, 4, row_top + 1, XP_SIDEBAR_W - 8, row_h / 2, GLASS_GLOSS);
            }
            gfx_fill_rect(t, 8, row_top + 4, 16, 16, XP_USB_ICON);
            gfx_draw_rect(t, 8, row_top + 4, 16, 16, 0xFF202028);
            gfx_draw_string(t, 12, row_top + 8, "U", 0xFFFFFFFF, XP_USB_ICON);

            char lbl[48];
            if (m->label[0])
                ksnprintf(lbl, sizeof(lbl), "%s (/%s)", m->label, m->mountpoint);
            else
                ksnprintf(lbl, sizeof(lbl), "/%s", m->mountpoint);
            gfx_draw_string_aa_clipped(t, 30, row_top + (row_h - FONT_GLYPH_H) / 2,
                                    XP_SIDEBAR_W - 36, lbl, XP_SIDEBAR_FG,
                                    hot ? XP_SIDEBAR_HOT : XP_SIDEBAR_BG);
        }
    }
}

static int sidebar_hit(int cx, int cy) {
    int top = XP_NAV_H;
    int y0 = top + 28;
    if (cx < 0 || cx >= XP_SIDEBAR_W) return -1;
    if (cy < y0) return -1;
    int idx = (cy - y0) / 26;
    if (idx >= 0 && idx < XP_SIDEBAR_COUNT) return idx;

    int usb_n = sidebar_usb_count();
    if (usb_n <= 0) return -1;
    int div_y = y0 + XP_SIDEBAR_COUNT * 26 + 2;
    int uy0 = div_y + 22;
    if (cy < uy0) return -1;
    int uidx = (cy - uy0) / 26;
    if (uidx < 0 || uidx >= usb_n) return -1;
    return XP_SIDEBAR_COUNT + uidx;
}

static void draw_status_bar(draw_target_t *t) {
    int sy = (int)t->height - XP_STATUS_H;
    gfx_fill_rect(t, 0, sy, (int)t->width, XP_STATUS_H, XP_STATUS_BG);
    gfx_draw_hline(t, 0, sy, (int)t->width, XP_SIDEBAR_BORDER);

    int n_dirs = 0, n_files = 0;
    for (int i = 0; i < g_xp.entry_count; i++) {
        if (g_xp.entries[i].inode == XP_PARENT_TAG) continue;
        if (g_xp.entries[i].type == NXFS_TYPE_DIR) n_dirs++;
        else                                       n_files++;
    }

    char buf[128];
    if (g_xp.selected >= 0 && g_xp.selected < g_xp.entry_count) {
        const xp_entry_t *e = &g_xp.entries[g_xp.selected];
        const char *kind = (e->inode == XP_PARENT_TAG) ? lang_get(STR_XP_PARENT)
                         : (e->type == NXFS_TYPE_DIR)  ? lang_get(STR_XP_FOLDER)
                         :                               lang_get(STR_XP_FILE);
        if (e->type == NXFS_TYPE_DIR) {
            ksnprintf(buf, sizeof(buf), "%s %s (%s)",
                      lang_get(STR_XP_SELECTED), e->name, kind);
        } else {
            ksnprintf(buf, sizeof(buf), "%s %s (%s, %u B)",
                      lang_get(STR_XP_SELECTED), e->name, kind, e->size);
        }
    } else {
        ksnprintf(buf, sizeof(buf), "%d %s (%d / %d)",
                  n_dirs + n_files, lang_get(STR_XP_NITEMS), n_dirs, n_files);
    }
    int max_chars = ((int)t->width - 8) / FONT_GLYPH_W;
    if (max_chars > 0 && (int)strlen(buf) > max_chars) {
        buf[max_chars - 1] = '.';
        buf[max_chars - 2] = '.';
        buf[max_chars] = 0;
    }
    gfx_draw_string_aa(t, 8, sy + (XP_STATUS_H - FONT_GLYPH_H) / 2,
                    buf, XP_STATUS_FG, XP_STATUS_BG);
}

static int visible_rows(const draw_target_t *t) {
    int top    = XP_NAV_H;
    int bottom = (int)t->height - XP_STATUS_H;
    int avail  = bottom - top;
    return avail / XP_ROW_H;
}

static void draw_list(draw_target_t *t) {
    int top    = XP_NAV_H;
    int bottom = (int)t->height - XP_STATUS_H;
    int avail  = bottom - top;
    int visible = avail / XP_ROW_H;
    int list_x = XP_SIDEBAR_W + 1;
    int list_w = (int)t->width - list_x;

    gfx_fill_rect(t, list_x, top, list_w, avail, XP_BG);

    int max_scroll = g_xp.entry_count - visible;
    if (max_scroll < 0) max_scroll = 0;
    if (g_xp.scroll_top > max_scroll) g_xp.scroll_top = max_scroll;
    if (g_xp.scroll_top < 0)          g_xp.scroll_top = 0;

    int end = g_xp.scroll_top + visible;
    if (end > g_xp.entry_count) end = g_xp.entry_count;

    int mx = mouse_x(), my = mouse_y();
    int abs_w_x = g_xp.win->x + WM_BORDER;
    int abs_w_y = g_xp.win->y + WM_BORDER + WM_TITLE_H + 2;

    for (int i = g_xp.scroll_top; i < end; i++) {
        const xp_entry_t *e = &g_xp.entries[i];
        int row_y = top + (i - g_xp.scroll_top) * XP_ROW_H;
        bool selected = (i == g_xp.selected);
        bool hot = (mx >= abs_w_x + list_x && mx < abs_w_x + (int)t->width &&
                    my >= abs_w_y + row_y && my < abs_w_y + row_y + XP_ROW_H);
        uint32_t bg = selected ? XP_SEL_BG : (hot ? XP_HOT_BG : XP_BG);
        uint32_t fg = row_color(e);

        if (selected || hot) {
            gfx_fill_rect(t, list_x, row_y, list_w, XP_ROW_H, bg);
            /* Glass sheen: top highlight + a soft upper gloss on the row. */
            gfx_blend_rect(t, list_x, row_y, list_w, 1, GLASS_EDGE_LIGHT);
            gfx_blend_rect(t, list_x, row_y, list_w, XP_ROW_H / 2,
                           selected ? 0x22FFFFFFu : 0x12FFFFFFu);
        }

        int icon_x = list_x + XP_PAD_L;
        int icon_y = row_y + (XP_ROW_H - XP_ICON_H) / 2;
        draw_entry_icon(t, icon_x, icon_y, e);
        (void)icon_color;

        int text_x = icon_x + XP_ICON_W + 8;
        int text_y = row_y + (XP_ROW_H - FONT_GLYPH_H) / 2;
        gfx_draw_string_aa_clipped(t, text_x, text_y,
                                list_w - (text_x - list_x) - 80,
                                e->name, fg, bg);

        if (e->inode != XP_PARENT_TAG && e->type == NXFS_TYPE_FILE) {
            char sbuf[16];
            ksnprintf(sbuf, sizeof(sbuf), "%u B", e->size);
            int sw = gfx_string_pixel_width(sbuf);
            int sx = list_x + list_w - 8 - sw;
            gfx_draw_string(t, sx, text_y, sbuf, XP_STATUS_FG, bg);
        }
    }
}

static void redraw(void) {
    if (stale_window()) return;
    draw_target_t *t = &g_xp.win->content;
    gfx_clear(t, XP_BG);
    draw_nav_bar(t);
    draw_sidebar(t);
    draw_list(t);
    draw_status_bar(t);
    wm_mark_dirty();
}

static void explorer_resize_cb(window_t *w) { (void)w; redraw(); }
static void explorer_destroy_cb(window_t *w) {
    if (w == g_xp.win) g_xp.win = NULL;
}

/* TASK i18n: re-paint when the locale changes. */
static void explorer_on_lang_change(lang_t new_lang) {
    (void)new_lang;
    redraw();
}

/* ---------- Navigation ----------------------------------------------- */
static void navigate_to_vfs(const char *mount, const char *subpath) {
    g_xp.browsing_vfs = true;
    strncpy(g_xp.vfs_mount_name, mount, VFS_NAME_MAX - 1);
    g_xp.vfs_mount_name[VFS_NAME_MAX - 1] = 0;
    strncpy(g_xp.vfs_subpath, subpath ? subpath : "/", sizeof(g_xp.vfs_subpath) - 1);
    g_xp.vfs_subpath[sizeof(g_xp.vfs_subpath) - 1] = 0;
    rebuild_entries();
    redraw();
}

static void navigate_to(uint32_t ino, bool track_history) {
    g_xp.browsing_vfs = false;
    if (nxfs_set_cwd(ino) != NXFS_OK) {
        debug_printf("[explorer] nxfs_set_cwd(%u) failed\n", ino);
        return;
    }
    g_xp.cwd_ino = ino;
    rebuild_entries();
    if (track_history) hist_push(ino);
    redraw();
}

static void go_up(void) {
    if (g_xp.browsing_vfs) {
        char *last = NULL;
        for (char *p = g_xp.vfs_subpath; *p; p++)
            if (*p == '/') last = p;
        if (last && last != g_xp.vfs_subpath) {
            *last = 0;
            rebuild_entries();
            redraw();
        } else {
            navigate_to(0, true);
        }
        return;
    }
    nxfs_inode_t cur;
    if (nxfs_read_inode(g_xp.cwd_ino, &cur) != NXFS_OK) return;
    if (cur.parent_inode == g_xp.cwd_ino) return;
    navigate_to(cur.parent_inode, true);
}

static void go_back(void) {
    if (!hist_can_back()) return;
    g_xp.hist_pos--;
    navigate_to(g_xp.hist[g_xp.hist_pos], false);
}

static void go_forward(void) {
    if (!hist_can_forward()) return;
    g_xp.hist_pos++;
    navigate_to(g_xp.hist[g_xp.hist_pos], false);
}

/* Walk a "/foo/bar/baz" path relative to the NXFS root.  Returns the
 * resolved inode, or 0 on failure. */
static uint32_t resolve_address(const char *path) {
    if (!path) return 0;
    while (*path == '/') path++;
    uint32_t cur = 0;
    while (*path) {
        char name[NXFS_NAME_MAX];
        int i = 0;
        while (*path && *path != '/' && i < NXFS_NAME_MAX - 1) {
            name[i++] = *path++;
        }
        name[i] = 0;
        while (*path == '/') path++;
        if (!name[0]) continue;
        uint32_t next;
        if (nxfs_resolve(cur, name, &next) != NXFS_OK) return 0;
        nxfs_inode_t in;
        if (nxfs_read_inode(next, &in) != NXFS_OK) return 0;
        if (in.type != NXFS_TYPE_DIR) return 0;
        cur = next;
    }
    return cur + 1;  /* +1 so 0 (root) returns 1; caller subtracts. */
}

static void address_go(void) {
    if (!g_xp.addr_text[0] || strcmp(g_xp.addr_text, "/") == 0) {
        navigate_to(0, true);
        g_xp.addr_focus = false;
        return;
    }

    /* Check if the path targets a VFS mount (e.g., "/usb0/..."). */
    const char *p = g_xp.addr_text;
    while (*p == '/') p++;
    char first[VFS_NAME_MAX];
    int fi = 0;
    while (*p && *p != '/' && fi < VFS_NAME_MAX - 1)
        first[fi++] = *p++;
    first[fi] = 0;
    if (vfs_find_mount(first)) {
        navigate_to_vfs(first, *p ? p : "/");
        g_xp.addr_focus = false;
        return;
    }

    uint32_t ino_plus_one = resolve_address(g_xp.addr_text);
    if (ino_plus_one == 0) {
        const char *info[] = {
            (i18n_get_language() == LANG_HU)
                ? "A megadott útvonal nem található."
                : "Path not found.",
            g_xp.addr_text
        };
        dialog_info(lang_get(STR_DLG_ERROR_TITLE), info, 2);
        if (nxfs_pwd_path(g_xp.addr_text, sizeof(g_xp.addr_text)) != NXFS_OK) {
            strcpy(g_xp.addr_text, "/");
        }
        g_xp.addr_caret = (int)strlen(g_xp.addr_text);
        redraw();
        return;
    }
    navigate_to(ino_plus_one - 1, true);
    g_xp.addr_focus = false;
}

static void open_selected(int idx) {
    if (idx < 0 || idx >= g_xp.entry_count) return;
    xp_entry_t *e = &g_xp.entries[idx];

    if (e->inode == XP_PARENT_TAG) { go_up(); return; }

    if (g_xp.browsing_vfs) {
        if (e->type == NXFS_TYPE_DIR) {
            size_t slen = strlen(g_xp.vfs_subpath);
            if (slen + strlen(e->name) + 2 < sizeof(g_xp.vfs_subpath)) {
                if (slen == 0 || g_xp.vfs_subpath[slen-1] != '/')
                    strcat(g_xp.vfs_subpath, "/");
                strcat(g_xp.vfs_subpath, e->name);
            }
            rebuild_entries();
            redraw();
            return;
        }
        /* FILE on a mounted volume: dispatch by extension to the app that
         * can render it.  Each opener reads through the VFS, so nothing is
         * staged into NXFS first. */
        char full[352];
        vfs_full_path(full, sizeof(full), e->name);
        if (name_has_ext(e->name, "png") || name_has_ext(e->name, "jpg") ||
            name_has_ext(e->name, "jpeg") || name_has_ext(e->name, "bmp")) {
            imgview_open_file(full);
        } else if (name_has_ext(e->name, "mp3") ||
                   name_has_ext(e->name, "wav")) {
            audioplayer_open_file(full);
        } else {
            editor_run_vfs(full, e->name);
        }
        return;
    }

    if (e->type == NXFS_TYPE_DIR) { navigate_to(e->inode, true); return; }

    char cmdline[128];
    if (name_is_script(e->name)) {
        ksnprintf(cmdline, sizeof(cmdline), "run %s", e->name);
    } else {
        ksnprintf(cmdline, sizeof(cmdline), "efile %s", e->name);
    }
    debug_printf("[explorer] dispatch -> %s\n", cmdline);
    wm_refocus_shell();
    shell_exec(cmdline);
    g_xp.cwd_ino = nxfs_cwd();
    rebuild_entries();
    redraw();
}

/* ---------- Sidebar shortcut click ------------------------------------ */
static void open_sidebar(int idx) {
    if (idx >= XP_SIDEBAR_COUNT) {
        int usb_idx = idx - XP_SIDEBAR_COUNT;
        vfs_mount_t *m = vfs_get_mount(usb_idx);
        if (m) navigate_to_vfs(m->mountpoint, "/");
        return;
    }
    if (idx < 0) return;
    const sidebar_entry_t *s = &g_sidebar[idx];
    if (!s->target_path || !s->target_path[0]) {
        navigate_to(0, true);
        return;
    }
    uint32_t ino;
    if (nxfs_resolve(0, s->target_path, &ino) == NXFS_OK) {
        nxfs_inode_t in;
        if (nxfs_read_inode(ino, &in) == NXFS_OK && in.type == NXFS_TYPE_DIR) {
            navigate_to(ino, true);
            return;
        }
    }
    if (strcmp(s->target_path, "docs") == 0 ||
        strcmp(s->target_path, "downloads") == 0) {
        if (nxfs_create_dir(0, s->target_path, &ino) == NXFS_OK) {
            navigate_to(ino, true);
            return;
        }
    }
    navigate_to(0, true);
}

/* ---------- Action implementations ----------------------------------- */

static void do_delete(int idx, xp_entry_t *e) {
    if (e->inode == XP_PARENT_TAG) return;

    char line1[96], line2[96];
    bool is_dir = (e->type == NXFS_TYPE_DIR);
    ksnprintf(line1, sizeof(line1), lang_get(STR_XP_DELETE_Q), e->name);
    if (is_dir) {
        strncpy(line2, lang_get(STR_XP_DELETE_RECURSIVE), sizeof(line2) - 1);
        line2[sizeof(line2) - 1] = 0;
    } else {
        line2[0] = 0;
    }

    if (!dialog_yes_no(lang_get(STR_XP_DELETE_TITLE),
                       line1,
                       line2[0] ? line2 : NULL)) {
        return;
    }

    int r;
    if (g_xp.browsing_vfs) {
        /* USB / mounted volumes have no local Recycle Bin -- unlink directly. */
        char full[320];
        ksnprintf(full, sizeof(full), "/%s%s/%s",
                  g_xp.vfs_mount_name, g_xp.vfs_subpath, e->name);
        r = vfs_delete(full);
    } else if (is_dir) {
        r = nxfs_delete_dir(g_xp.cwd_ino, e->name);
    } else {
        /* #2: deleted files must land in the Recycle Bin (restorable), not
         * vanish.  trash_move_in() copies the file into /sys/.trash and unlinks
         * the original; it resolves the source relative to nxfs_cwd(), so make
         * sure the global cwd matches the pane we're deleting from.  Fall back
         * to a plain unlink only if the move fails. */
        nxfs_set_cwd(g_xp.cwd_ino);
        r = trash_move_in(e->name);
        if (r != 0) r = nxfs_delete_file(g_xp.cwd_ino, e->name);
    }
    if (r != 0) {
        debug_printf("[explorer] delete '%s' failed: %d\n", e->name, r);
        const char *line_a = "Delete failed.";
        char line_b[80];
        ksnprintf(line_b, sizeof(line_b),
                  "Error code %d.  See serial log.", r);
        const char *info[2] = { line_a, line_b };
        dialog_info("Delete error", info, 2);
    }
    (void)idx;
    rebuild_entries();
    redraw();
}

static void do_rename(int idx, xp_entry_t *e) {
    if (e->inode == XP_PARENT_TAG) return;

    char title[80];
    ksnprintf(title, sizeof(title), "%s: %s",
              lang_get(STR_XP_CTX_RENAME), e->name);

    char new_name[NXFS_NAME_MAX];
    new_name[0] = 0;
    if (!dialog_input(title, lang_get(STR_XP_RENAME_PROMPT), e->name,
                      new_name, sizeof(new_name))) {
        return;
    }
    if (!new_name[0] || strcmp(new_name, e->name) == 0) {
        return;
    }

    if (g_xp.browsing_vfs) {
        char full[352];
        vfs_full_path(full, sizeof(full), e->name);
        int r = vfs_rename(full, new_name);
        if (r == VFS_RENAME_EBADNAME)     xp_notify_error(STR_XP_BAD_SHORTNAME);
        else if (r == VFS_RENAME_EEXISTS) xp_notify_error(STR_XP_NAME_TAKEN);
        else if (r != 0)                  xp_notify_error(STR_XP_OP_FAILED);
        rebuild_entries();
        redraw();
        return;
    }

    int r = nxfs_rename(g_xp.cwd_ino, e->name, new_name);
    if (r != NXFS_OK) {
        if (r == NXFS_ERR_EXISTS)    xp_notify_error(STR_XP_NAME_TAKEN);
        else                          xp_notify_error(STR_XP_OP_FAILED);
    }
    (void)idx;
    rebuild_entries();
    redraw();
}

static void do_properties(int idx, xp_entry_t *e) {
    if (e->inode == XP_PARENT_TAG) return;

    nxfs_inode_t n;
    if (nxfs_read_inode(e->inode, &n) != NXFS_OK) {
        const char *info[1] = { "Failed to read inode from NXFS." };
        dialog_info("Properties error", info, 1);
        return;
    }

    char l1[80], l2[80], l3[80], l4[80], l5[80], l6[96], l7[96];
    ksnprintf(l1, sizeof(l1), "Name        : %s", n.name);
    ksnprintf(l2, sizeof(l2), "Type        : %s",
              n.type == NXFS_TYPE_DIR ? "directory"
              : n.type == NXFS_TYPE_FILE ? "file"
              :                            "<unknown>");
    ksnprintf(l3, sizeof(l3), "Inode       : #%u  (parent #%u)",
              e->inode, n.parent_inode);
    ksnprintf(l4, sizeof(l4), "Magic / flag: 0x%X", n.magic);

    if (n.type == NXFS_TYPE_FILE) {
        if (n.size_hi) {
            ksnprintf(l5, sizeof(l5),
                      "Size        : %u MiB  (%u 4K block%s)",
                      (uint32_t)((((uint64_t)n.size_hi << 32) | n.size) >> 20),
                      n.block_count, n.block_count == 1 ? "" : "s");
        } else {
            ksnprintf(l5, sizeof(l5),
                      "Size        : %u bytes  (%u 4K block%s)",
                      n.size, n.block_count,
                      n.block_count == 1 ? "" : "s");
        }
        uint32_t blk = n.direct[0];
        ksnprintf(l6, sizeof(l6),
                  "First block : index %u  (v3 direct[0])", blk);
        ksnprintf(l7, sizeof(l7),
                  "First LBA   : %u  (data_start + index*8)",
                  nxfs_data_start() + blk * NXFS_BLOCK_SECTORS);
    } else {
        ksnprintf(l5, sizeof(l5),
                  "Children    : %u / %u entries",
                  n.child_count, (uint32_t)NXFS_MAX_CHILDREN);
        ksnprintf(l6, sizeof(l6),
                  "Block usage : %u block%s (dir inline)",
                  n.block_count,
                  n.block_count == 1 ? "" : "s");
        l7[0] = 0;
    }

    const char *lines[7] = { l1, l2, l3, l4, l5, l6, l7[0] ? l7 : NULL };
    int n_lines = l7[0] ? 7 : 6;

    char title[80];
    ksnprintf(title, sizeof(title), "Properties: %s", e->name);
    dialog_info(title, lines, n_lines);
    (void)idx;
}

/* ---------- Context menu --------------------------------------------- */
static int g_ctx_target_idx = -1;

static void ctx_callback(int id, void *user) {
    (void)user;
    int idx = g_ctx_target_idx;
    g_ctx_target_idx = -1;
    if (idx < 0 || idx >= g_xp.entry_count) {
        debug_printf("[explorer] ctx menu fired with stale target\n");
        return;
    }
    xp_entry_t *e = &g_xp.entries[idx];

    switch (id) {
        case XP_CTX_OPEN:        open_selected(idx);          break;
        case XP_CTX_DELETE:      do_delete   (idx, e);        break;
        case XP_CTX_RENAME:      do_rename   (idx, e);        break;
        case XP_CTX_PROPERTIES:  do_properties(idx, e);       break;
        case XP_CTX_COPY:        do_copy     (idx, e);        break;
        default:
            debug_printf("[explorer] unknown ctx menu id %d\n", id);
    }
}

/* Background (empty-area) menu: directory-level operations. */
static void do_new_folder(void) {
    if (g_xp.browsing_vfs && !vfs_writable(g_xp.vfs_mount_name)) {
        xp_notify_error(STR_XP_READ_ONLY);
        return;
    }
    char name[NXFS_NAME_MAX];
    name[0] = 0;
    if (!dialog_input(lang_get(STR_XP_CTX_NEW_FOLDER),
                      lang_get(STR_XP_NEWDIR_PROMPT), "",
                      name, sizeof(name)))
        return;
    if (!name[0]) return;
    if (name_exists_here(name)) {
        xp_notify_error(STR_XP_NAME_TAKEN);
        return;
    }
    int r;
    if (g_xp.browsing_vfs) {
        char full[352];
        vfs_full_path(full, sizeof(full), name);
        r = vfs_mkdir_path(full);
    } else {
        uint32_t ino = 0;
        r = (nxfs_create_dir(g_xp.cwd_ino, name, &ino) == NXFS_OK) ? 0 : -1;
    }
    if (r < 0) xp_notify_error(STR_XP_OP_FAILED);
    rebuild_entries();
    redraw();
}

/* Append `ext` to `name` when it has no extension yet. */
static void xp_ensure_ext(char *name, size_t cap, const char *ext) {
    for (const char *p = name; *p; p++) if (*p == '.') return;
    size_t l = strlen(name), e = strlen(ext);
    if (l + e < cap) strcpy(name + l, ext);
}

/* "New File": a Text/Config/Custom chooser, then a name prompt; creates the
 * file in the current directory (NXFS or a writable mount) and refreshes. */
static void do_new_file_xp(void) {
    if (g_xp.browsing_vfs && !vfs_writable(g_xp.vfs_mount_name)) {
        xp_notify_error(STR_XP_READ_ONLY);
        return;
    }
    bool hu = (i18n_get_language() == LANG_HU);
    const char *labels[3] = {
        hu ? "Szoveges fajl (.txt)" : "Text file (.txt)",
        hu ? "Konfig fajl (.cfg)"   : "Config file (.cfg)",
        hu ? "Egyedi..."            : "Custom...",
    };
    const char *icons[3] = { "x.txt", "x.cfg", "" };
    int sel = dialog_choice(hu ? "Uj fajl" : "New File",
                            hu ? "Milyen fajlt hozzunk letre?" : "What kind of file?",
                            labels, icons, 3);
    if (sel < 0) return;
    const char *def = (sel == 0) ? "untitled.txt"
                    : (sel == 1) ? "untitled.cfg" : "untitled";
    char name[NXFS_NAME_MAX];
    name[0] = 0;
    if (!dialog_input(hu ? "Uj fajl" : "New File",
                      hu ? "Fajlnev:" : "File name:", def, name, sizeof(name))
        || !name[0]) return;
    if (sel == 0)      xp_ensure_ext(name, sizeof(name), ".txt");
    else if (sel == 1) xp_ensure_ext(name, sizeof(name), ".cfg");
    if (name_exists_here(name)) { xp_notify_error(STR_XP_NAME_TAKEN); return; }

    int r;
    if (g_xp.browsing_vfs) {
        char full[352];
        vfs_full_path(full, sizeof(full), name);
        r = vfs_write_file(full, "", 0);
    } else {
        uint32_t ino = 0;
        r = (nxfs_create_file(g_xp.cwd_ino, name, &ino) == NXFS_OK) ? 0 : -1;
    }
    if (r < 0) xp_notify_error(STR_XP_OP_FAILED);
    rebuild_entries();
    redraw();
}

static void bg_ctx_callback(int id, void *user) {
    (void)user;
    switch (id) {
        case XP_CTX_NEWFILE: do_new_file_xp();                  break;
        case XP_CTX_NEWDIR:  do_new_folder();                   break;
        case XP_CTX_PASTE:   do_paste();                        break;
        case XP_CTX_REFRESH: rebuild_entries(); redraw();       break;
        default: break;
    }
}

static void spawn_bg_menu(int screen_x, int screen_y) {
    bool writable = !g_xp.browsing_vfs ||
                    vfs_writable(g_xp.vfs_mount_name);
    bool hu = (i18n_get_language() == LANG_HU);
    ctxmenu_item_t items[5];
    int n = 0;

    items[n].id = XP_CTX_NEWFILE; items[n].separator = false;
    items[n].dangerous = false;   items[n].disabled  = !writable;
    strncpy(items[n].label, hu ? "Uj fajl..." : "New File...",
            CTXMENU_LABEL_MAX - 1);
    items[n].label[CTXMENU_LABEL_MAX - 1] = 0;
    n++;

    items[n].id = XP_CTX_NEWDIR; items[n].separator = false;
    items[n].dangerous = false;  items[n].disabled  = !writable;
    strncpy(items[n].label, lang_get(STR_XP_CTX_NEW_FOLDER),
            CTXMENU_LABEL_MAX - 1);
    items[n].label[CTXMENU_LABEL_MAX - 1] = 0;
    n++;

    items[n].id = XP_CTX_PASTE;  items[n].separator = false;
    items[n].dangerous = false;
    items[n].disabled  = !g_xp_clip.valid || !writable;
    strncpy(items[n].label, lang_get(STR_XP_CTX_PASTE),
            CTXMENU_LABEL_MAX - 1);
    items[n].label[CTXMENU_LABEL_MAX - 1] = 0;
    n++;

    items[n].id = 0; items[n].separator = true;
    items[n].dangerous = false; items[n].disabled = false;
    items[n].label[0] = 0;
    n++;

    items[n].id = XP_CTX_REFRESH; items[n].separator = false;
    items[n].dangerous = false;   items[n].disabled  = false;
    strncpy(items[n].label, lang_get(STR_XP_CTX_REFRESH),
            CTXMENU_LABEL_MAX - 1);
    items[n].label[CTXMENU_LABEL_MAX - 1] = 0;
    n++;

    ctxmenu_open(screen_x, screen_y, items, n, bg_ctx_callback, NULL);
}

static void spawn_context_menu(int entry_idx, int screen_x, int screen_y) {
    g_ctx_target_idx = entry_idx;
    xp_entry_t *e = &g_xp.entries[entry_idx];
    bool is_parent = (e->inode == XP_PARENT_TAG);
    bool vfs_rw = g_xp.browsing_vfs && vfs_writable(g_xp.vfs_mount_name);
    bool readonly = g_xp.browsing_vfs && !vfs_rw;
    /* Rename needs backend support (FAT yes, exFAT/NTFS not yet). */
    bool renamable = !g_xp.browsing_vfs || vfs_renamable(g_xp.vfs_mount_name);

    ctxmenu_item_t items[7];
    int n = 0;

    items[n].id        = XP_CTX_OPEN;
    items[n].separator = false;
    items[n].dangerous = false;
    items[n].disabled  = is_parent;
    strncpy(items[n].label,
            lang_get(STR_XP_CTX_OPEN), CTXMENU_LABEL_MAX - 1);
    items[n].label[CTXMENU_LABEL_MAX - 1] = 0;
    n++;

    items[n].id        = XP_CTX_COPY;
    items[n].separator = false;
    items[n].dangerous = false;
    items[n].disabled  = is_parent || (e->type == NXFS_TYPE_DIR);
    strncpy(items[n].label, lang_get(STR_XP_CTX_COPY), CTXMENU_LABEL_MAX - 1);
    items[n].label[CTXMENU_LABEL_MAX - 1] = 0;
    n++;

    items[n].id = 0; items[n].separator = true;
    items[n].dangerous = false; items[n].disabled = false;
    items[n].label[0] = 0;
    n++;

    items[n].id        = XP_CTX_RENAME;
    items[n].separator = false;
    items[n].dangerous = false;
    items[n].disabled  = is_parent || readonly || !renamable;
    strncpy(items[n].label, lang_get(STR_XP_CTX_RENAME), CTXMENU_LABEL_MAX - 1);
    items[n].label[CTXMENU_LABEL_MAX - 1] = 0;
    n++;

    items[n].id        = XP_CTX_DELETE;
    items[n].separator = false;
    items[n].dangerous = true;
    items[n].disabled  = is_parent || readonly;
    strncpy(items[n].label, lang_get(STR_XP_CTX_DELETE), CTXMENU_LABEL_MAX - 1);
    items[n].label[CTXMENU_LABEL_MAX - 1] = 0;
    n++;

    items[n].id        = XP_CTX_PROPERTIES;
    items[n].separator = false;
    items[n].dangerous = false;
    items[n].disabled  = is_parent;
    strncpy(items[n].label, lang_get(STR_XP_CTX_PROPERTIES), CTXMENU_LABEL_MAX - 1);
    items[n].label[CTXMENU_LABEL_MAX - 1] = 0;
    n++;

    ctxmenu_open(screen_x, screen_y, items, n, ctx_callback, NULL);
}

/* ---------- Click dispatch (called by the WM mouse handler) ----------- */
static int hit_button(int cx, int cy) {
    int by = 4;
    int bh = XP_NAV_H - 8;
    if (cy < by || cy >= by + bh) return -1;
    int bx = 6;
    for (int i = 0; i < 3; i++) {
        if (cx >= bx && cx < bx + XP_BTN_W) return i;
        bx += XP_BTN_W + 4;
    }
    return -1;
}

static int hit_addr_bar(int cx, int cy, int win_w) {
    int by = 4;
    int bh = XP_NAV_H - 8;
    int ax = 6 + 3 * (XP_BTN_W + 4) + 6 + XP_ADDR_LBL_W;
    int aw = win_w - ax - 8;
    if (cx < ax || cx >= ax + aw) return -1;
    if (cy < by + 2 || cy >= by + bh - 2) return -1;
    return ax;
}

static int hit_entry(draw_target_t *t, int cx, int cy) {
    int top    = XP_NAV_H;
    int bottom = (int)t->height - XP_STATUS_H;
    if (cy < top || cy >= bottom) return -1;
    if (cx < XP_SIDEBAR_W) return -1;
    int row = (cy - top) / XP_ROW_H;
    int idx = g_xp.scroll_top + row;
    if (idx < 0 || idx >= g_xp.entry_count) return -1;
    int vis = visible_rows(t);
    if (row >= vis) return -1;
    return idx;
}

static bool xp_click(window_t *w, int cx, int cy, uint8_t pressed, uint8_t btn) {
    (void)btn;
    if (stale_window() || w != g_xp.win) return false;

    if (pressed & MOUSE_BTN_LEFT) {
        /* Nav bar buttons. */
        if (cy < XP_NAV_H) {
            int b = hit_button(cx, cy);
            if (b == 0) { go_back();    return true; }
            if (b == 1) { go_forward(); return true; }
            if (b == 2) { go_up();      return true; }
            int ax = hit_addr_bar(cx, cy, (int)w->content.width);
            if (ax >= 0) {
                g_xp.addr_focus = true;
                wm_set_focus(g_xp.win);
                redraw();
                return true;
            }
            return true;
        }

        g_xp.addr_focus = false;

        /* Sidebar. */
        if (cx < XP_SIDEBAR_W) {
            int s = sidebar_hit(cx, cy);
            if (s >= 0) {
                open_sidebar(s);
            }
            return true;
        }

        /* File list. */
        int idx = hit_entry(&w->content, cx, cy);
        if (idx < 0) {
            if (g_xp.selected != -1) { g_xp.selected = -1; redraw(); }
            return true;
        }

        uint32_t now = pit_ms();
        bool is_dbl  = (g_xp.last_click_idx == idx) &&
                       (now - g_xp.last_click_ms <= DBLCLICK_MS);
        g_xp.last_click_idx = idx;
        g_xp.last_click_ms  = now;

        if (is_dbl) {
            g_xp.last_click_idx = -1;
            open_selected(idx);
            return true;
        }

        g_xp.selected = idx;
        redraw();
        return true;
    }

    if (pressed & MOUSE_BTN_RIGHT) {
        if (cy < XP_NAV_H || cx < XP_SIDEBAR_W) return true;
        int idx = hit_entry(&w->content, cx, cy);
        int sx = g_xp.win->x + WM_BORDER + cx + 4;
        int sy = g_xp.win->y + WM_BORDER + WM_TITLE_H + 2 + cy + 4;
        if (idx < 0) {
            /* Empty area: directory-level menu (new folder / paste / ...). */
            spawn_bg_menu(sx, sy);
            return true;
        }

        g_xp.selected = idx;
        redraw();
        spawn_context_menu(idx, sx, sy);
        return true;
    }

    return false;
}

/* ---------- Key dispatch for the address bar -------------------------- */
static bool xp_key(window_t *w, int c) {
    if (w != g_xp.win || !g_xp.addr_focus) return false;
    if (c == '\n' || c == '\r') {
        address_go();
        return true;
    }
    if (c == '\b') {
        int n = (int)strlen(g_xp.addr_text);
        if (g_xp.addr_caret > 0 && n > 0) {
            memmove(g_xp.addr_text + g_xp.addr_caret - 1,
                    g_xp.addr_text + g_xp.addr_caret,
                    (size_t)(n - g_xp.addr_caret + 1));
            g_xp.addr_caret--;
        }
        redraw();
        return true;
    }
    if (c == KEY_LEFT)  { if (g_xp.addr_caret > 0) g_xp.addr_caret--; redraw(); return true; }
    if (c == KEY_RIGHT) {
        int n = (int)strlen(g_xp.addr_text);
        if (g_xp.addr_caret < n) g_xp.addr_caret++;
        redraw();
        return true;
    }
    if (c == KEY_HOME) { g_xp.addr_caret = 0; redraw(); return true; }
    if (c == KEY_END)  { g_xp.addr_caret = (int)strlen(g_xp.addr_text); redraw(); return true; }
    if (c >= 0x20 && c < 0x7F) {
        int n = (int)strlen(g_xp.addr_text);
        if (n + 1 < (int)sizeof(g_xp.addr_text)) {
            memmove(g_xp.addr_text + g_xp.addr_caret + 1,
                    g_xp.addr_text + g_xp.addr_caret,
                    (size_t)(n - g_xp.addr_caret + 1));
            g_xp.addr_text[g_xp.addr_caret] = (char)c;
            g_xp.addr_caret++;
        }
        redraw();
        return true;
    }
    return true;
}

/* ---------- Public API ------------------------------------------------ */
bool explorer_active(void) { return !stale_window(); }

bool explorer_open(void) {
    if (!stale_window()) {
        wm_set_focus(g_xp.win);
        g_xp.cwd_ino = nxfs_cwd();
        rebuild_entries();
        redraw();
        return true;
    }

    g_xp.win = wm_create_window(XP_WIN_X, XP_WIN_Y,
                                 XP_WIN_W, XP_WIN_H,
                                 L(STR_APP_EXPLORER));
    if (!g_xp.win) {
        debug_printf("[explorer] no window slot or framebuffer space\n");
        return false;
    }
    g_xp.cwd_ino       = nxfs_cwd();
    g_xp.selected      = -1;
    g_xp.scroll_top    = 0;
    g_xp.last_click_ms = 0;
    g_xp.last_click_idx = -1;
    g_xp.hist_count    = 0;
    g_xp.hist_pos      = -1;
    g_xp.addr_focus    = false;
    g_xp.addr_text[0]  = 0;
    g_xp.addr_caret    = 0;
    hist_push(g_xp.cwd_ino);

    wm_set_content_click(g_xp.win, xp_click, NULL);
    wm_set_key_handler  (g_xp.win, xp_key);
    wm_set_resizable    (g_xp.win, true, XP_MIN_W, XP_MIN_H);
    wm_set_resize_cb    (g_xp.win, explorer_resize_cb);
    wm_set_destroy_cb   (g_xp.win, explorer_destroy_cb);
    wm_set_icon         (g_xp.win, ICON_EXPLORER);
    /* Re-paint on a system-wide locale change. */
    static bool lang_cb_registered = false;
    if (!lang_cb_registered) {
        lang_register_cb(explorer_on_lang_change);
        lang_cb_registered = true;
    }
    rebuild_entries();
    redraw();
    wm_set_focus(g_xp.win);
    return true;
}

void explorer_refresh(void) {
    if (stale_window()) return;
    g_xp.cwd_ino = nxfs_cwd();
    rebuild_entries();
    redraw();
}

/* Hot-plug hook from the PnP daemon: a volume just (un)mounted.  Refresh
 * the Drives sidebar; if the directory on screen lived on the volume that
 * vanished, drop back to Home instead of showing a dead listing. */
void explorer_on_mounts_changed(void) {
    if (stale_window()) return;
    if (g_xp.browsing_vfs && !vfs_find_mount(g_xp.vfs_mount_name)) {
        navigate_to(0, true);              /* rebuilds + redraws */
        return;
    }
    rebuild_entries();
    redraw();
}
