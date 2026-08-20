/* ============================================================================
 * NexxoN OS - Desktop Environment implementation  (v2.0)
 * ----------------------------------------------------------------------------
 * v2.0 additions over v1:
 *
 *   - BUG 1: right-click is now first-class.  desktop_handle_right_click()
 *     opens a context menu over the icon under the cursor (Open / Rename /
 *     Edit command / Delete / Properties) or over empty wallpaper
 *     (New Icon... / Refresh / About).
 *
 *   - BUG 3: icons survive reboot via /sys/desktop.cfg.  Auto-saved by
 *     desktop_add_icon / desktop_remove_icon / desktop_update_icon, and
 *     re-loaded by desktop_load_icons() after NXFS comes up at boot.
 *
 *   - BUG 7: nicer panel gradient, sharper start-button bevels, refined
 *     start-menu padding, and richer icon glyphs with drop shadows.
 *
 * Persistence-on-write is deliberate: a kernel exception that abandons
 * the user's session must NOT lose the icons they just configured.  Save
 * cost is one NXFS write per mutation - acceptable for the dozens of
 * icons we ever store.
 * ============================================================================ */
#include "desktop.h"
#include "gfx.h"
#include "font.h"
#include "vga.h"
#include "rtc.h"
#include "pit.h"
#include "string.h"
#include "debug.h"
#include "acpi.h"
#include "nxfs.h"
#include "ctxmenu.h"
#include "dialogs.h"
#include "mouse.h"
#include "bmp.h"
#include "window.h"
#include "apps.h"
#include "i18n.h"
#include "notify.h"
#include "explorer.h"
#include "nexstore.h"
#include "audioplayer.h"
#include "installer.h"
#include "multimon.h"
#include "wifi.h"
#include "bluetooth.h"
#include "speaker.h"
#include "theme.h"
#include "tray.h"
#include "audio.h"
#include "auth.h"
#include "icons.h"
#include "doomapp.h"
#include "programs.h"

/* ---------- Colour palette (Windows 7 Aero Glass) ---------------------- */
#define BG_BASE             0xFF1A2840u
#define BG_DOT              0xFF2A3858u
/* Taskbar: dark glass gradient #1A2B3C → #111A24 (per spec) */
#define PANEL_BG_TOP        0xFF1A2B3Cu
#define PANEL_BG_BOT        0xFF111A24u
/* 1 px top-edge highlight: white alpha 80 (0x50) */
#define PANEL_TOP_EDGE      0x50FFFFFFu
#define PANEL_FG            0xFFE8E8F0u
#define PANEL_LO            0xFF000000u
/* Aero orb: glossy blue sphere, sized to fit inside the 40 px taskbar.
 * Radius 16 gives a 32 px diameter — leaves a tidy 4 px margin top/bot. */
#define ORB_RADIUS          16
#define ORB_TOP             0xFF5A9AEEu   /* bright sky-blue */
#define ORB_BOT             0xFF0E30A0u   /* deep navy */
#define ORB_TOP_HOT         0xFF78BCFFu
#define ORB_BOT_HOT         0xFF1848D0u
/* Show Desktop strip (10 px wide at far right, 1 px white-alpha-50 border) */
#define SHOW_DESK_W         10
#define SHOW_DESK_BORDER    0x32FFFFFFu   /* white alpha 50 */
/* Taskbar window tiles */
#define TILE_BG_NORMAL      0x80182840u
#define TILE_BG_ACTIVE      0x99305890u
#define TILE_BORDER         0x60FFFFFFu
/* Start menu dimensions — two-pane Aero layout */
#define MENU_W              476           /* wide enough for 16 px labels */
#define MENU_LEFT_W         330
#define MENU_RIGHT_W        146
#define MENU_BORDER_W       8
#define MENU_GLASS          0x78000000u   /* outer frame: black alpha 120 */
/* Aero panes — both alpha-blended over the glass frame so the
 * underlying wallpaper/desktop bleeds through and the menu reads as
 * frosted glass instead of two solid blocks. */
#define MENU_LEFT_BG        0xE6FFFFFFu   /* left pane: white alpha 230  */
#define MENU_RIGHT_BG       0xC8182533u   /* right pane: #182533 alpha 200 */
/* Item hover overlay must remain visible against the translucent pane */
#define MENU_LEFT_BG_OPAQUE 0xFFFFFFFFu
#define MENU_ITEM_H         26
#define MENU_PAD            8
#define MENU_SEARCH_H       28
#define USER_PIC_SZ         48
/* Menu item colours */
#define MENU_HI             0xFFFFFFFFu
#define MENU_LO             0xFF404040u
#define MENU_ITEM_FG        0xFF101010u
#define MENU_ITEM_HOT_BG    0xFFCCE4F8u   /* Vista hover: light blue */
#define MENU_ITEM_HOT_FG    0xFF000000u   /* black text on hover */
#define MENU_RIGHT_FG       0xFFFFFFFFu   /* right pane text: white */
#define MENU_RIGHT_HOT      0x40FFFFFFu   /* right pane hover tint */
/* Icons */
#define ICON_BOX_BG         0xFFFFE040u
#define ICON_BOX_HI         0xFFFFFCB0u
#define ICON_BOX_LO         0xFF8A6800u
#define ICON_BOX_SHADOW     0x80000000u
#define ICON_LABEL_FG       0xFFFFFFFFu
#define ICON_LABEL_BG       0xFF1A2840u
#define ICON_LABEL_BG_SEL   0xFF2050B0u

/* ---------- Panel geometry ---------------------------------------------- */
/* Orb centre sits 2 px above the taskbar top edge (ORB_RADIUS = 18) */
#define ORB_CENTER_X        (4 + ORB_RADIUS)      /* 22 px from left edge */
/* The "start button" hit area covers the orb's bounding box */
#define START_BTN_X         4
#define START_BTN_W         (ORB_RADIUS * 2 + 4)
#define START_BTN_H         (ORB_RADIUS * 2 + 4)
/* Power band kept for internal click routing only; visual power items
 * live in the right pane of the new two-pane start menu. */
#define MENU_POWER_BAND_H   34

/* Double-click detection window (ms). */
#define DBLCLICK_WINDOW_MS  450u

/* Context-menu IDs (must be distinct from explorer's). */
#define DT_CTX_OPEN         101
#define DT_CTX_RENAME       102
#define DT_CTX_EDIT_CMD     103
#define DT_CTX_DELETE       104
#define DT_CTX_PROPERTIES   105
#define DT_CTX_NEW_ICON     201
#define DT_CTX_REFRESH      202
#define DT_CTX_NEW_FILE     204
#define DT_CTX_ABOUT        203

/* Taskbar tile right-click menu (TASK 9). */
#define DT_CTX_TILE_RESTORE 301
#define DT_CTX_TILE_MIN     302
#define DT_CTX_TILE_CLOSE   303

/* ---------- Icon table state ------------------------------------------- */
typedef struct {
    bool        in_use;
    char        label[DESKTOP_ICON_LABEL_MAX];
    char        cmd  [DESKTOP_CMD_MAX];
    int         x, y;          /* recomputed every frame */
} icon_t;

static icon_t g_icons[DESKTOP_MAX_ICONS];

/* ---------- Wallpaper state ------------------------------------------- */
static bmp_image_t g_wallpaper = { 0 };
static bool        g_wallpaper_ok = false;

/* ---------- Misc state ------------------------------------------------- */
static bool             g_menu_open      = false;
static uint32_t         g_last_click_ms  = 0;
static int              g_last_click_icon = -1;

static char             g_pending_cmd[DESKTOP_CMD_MAX] = {0};
static bool             g_have_pending   = false;

/* Target of the most recent right-click; -1 = empty desktop. */
static int              g_ctx_target_idx = -1;

/* Window id (not pointer — pointer could be stale after destroy) that
 * the taskbar context menu refers to.  0 = no pending menu. */
static int              g_ctx_tile_win_id = 0;

/* Selected icon index (visual highlight via different label colour). */
static int              g_selected_icon  = -1;

/* TASK 33: calendar popup state.  Mutually exclusive with the start
 * menu so only one panel-anchored overlay is up at a time.  The geometry
 * (CAL_W / CAL_H) only depends on the font metrics so it doesn't need
 * a runtime measurement pass. */
#define CAL_W              200
#define CAL_H              176
#define CAL_HDR_H          22
#define CAL_BG             0xFFF8F8FC
#define CAL_HDR_TOP        0xFF002878
#define CAL_HDR_BOT        0xFF1850C8
#define CAL_CELL_W         (CAL_W / 7)
static bool             g_calendar_open  = false;

/* ---------- Helpers ---------------------------------------------------- */
static int screen_width(void)  { return (int)vga_width();  }
static int screen_height(void) { return (int)vga_height(); }

int desktop_panel_top(void)     { return screen_height() - DESKTOP_PANEL_H; }
int desktop_usable_height(void) { return screen_height() - DESKTOP_PANEL_H; }

static void compute_icon_pos(int idx, int *x, int *y) {
    int per_row = (screen_width() - DESKTOP_ICON_GAP_X) /
                  (DESKTOP_ICON_W  + DESKTOP_ICON_GAP_X);
    if (per_row < 1) per_row = 1;
    int col = idx % per_row;
    int row = idx / per_row;
    *x = DESKTOP_ICON_GAP_X + col * (DESKTOP_ICON_W + DESKTOP_ICON_GAP_X);
    *y = DESKTOP_ICON_GAP_Y + row * (DESKTOP_ICON_H + DESKTOP_ICON_GAP_Y);
}

static bool point_in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* Centre the orb in the 40 px taskbar so it never protrudes above the
 * top edge.  With ORB_RADIUS = 16 the orb is 32 px tall, leaving a 4 px
 * margin top and bottom inside the panel. */
static int orb_center_y(void) {
    return desktop_panel_top() + DESKTOP_PANEL_H / 2;
}
static int start_btn_y(void) {
    return orb_center_y() - ORB_RADIUS;
}

/* Start menu item table.  One entry per launchable shortcut.  The order
 * here is the order users see; rearranging is one source-line.  Each
 * item carries a label key (i18n), an accent colour for the icon plaque,
 * and a single-letter icon glyph (the bitmap font is enough for now).
 *
 * Open callbacks return so the menu can close right after launch; their
 * side-effect is whatever the underlying app does. */
typedef void (*menu_open_fn)(void);

typedef struct {
    lang_id_t    str_id;          /* TASK i18n v2: STR_* enum lookup       */
    const char  *en_fallback;     /* (kept for diagnostic / debug printf)  */
    uint32_t     accent;
    char         glyph;
    menu_open_fn fn;
    const char  *cmd;             /* shortcut command key (desktop icons)  */
} menu_item_t;

/* Helper wrappers — every app launch goes through apps_launch() so the
 * shell window-launcher path enforces CPL=3 at least once per program
 * (see apps.h "Ring-3 launch policy").  Apps that don't yet wrap
 * themselves still benefit from the privilege transition here. */
static void sm_open_settings  (void) { (void)apps_launch(gephaz_open,  "settings"); }
static void sm_open_taskmgr   (void) { (void)apps_launch(taskmgr_open, "taskmgr"); }
static void sm_open_explorer  (void) { (void)apps_launch(explorer_open,"explorer"); }
static void sm_open_browser   (void) { (void)apps_launch(browser_open, "browser"); }
static void sm_open_usermgr   (void) { (void)apps_launch(usermgr_open, "usermgr"); }
static void sm_open_trash     (void) { (void)apps_launch(trash_open,   "trash"); }
static void sm_open_devmgr    (void) { (void)apps_launch(devmgr_open,  "devmgr"); }
static void sm_open_nexsheet  (void) { (void)apps_launch(nexsheet_open,"nexsheet"); }
static void sm_open_nexstore     (void) { (void)apps_launch(nexstore_open,    "nexstore");     }
static void sm_open_audioplayer  (void) { (void)apps_launch(audioplayer_open,  "audioplayer");  }
static void sm_open_installer    (void) { (void)apps_launch(installer_open,    "installer");    }
static void sm_open_multimon     (void) { (void)apps_launch(multimon_open,     "displays");     }
static void sm_open_wifi         (void) { (void)apps_launch(wifi_manager_open,  "wifi");         }
static void sm_open_bluetooth    (void) { (void)apps_launch(bt_manager_open,    "bluetooth");    }
static void sm_open_doom         (void) { (void)apps_launch(doomapp_open,       "doom");         }
static void sm_open_programs     (void) { (void)apps_launch(programs_open,      "programs");     }

/* Editor + scratchpad: create / open a /scratch.txt file.  This gives
 * the Start Menu a one-click "open editor" entry without requiring a
 * filename argument up-front. */
extern void editor_run(uint32_t inode, const char *display_name);
static void sm_open_editor(void) {
    if (!nxfs_is_mounted()) return;
    uint32_t ino;
    if (nxfs_resolve(0, "scratch.txt", &ino) != NXFS_OK) {
        if (nxfs_create_file(0, "scratch.txt", &ino) != NXFS_OK) return;
    }
    editor_run(ino, "scratch.txt");
}

static void sm_action_restart (void) { acpi_reboot(); }
static void sm_action_shutdown(void) { acpi_shutdown(); }

static const menu_item_t g_start_apps[] = {
    { STR_STARTMENU_TASKMGR,  "Task Manager",   0xFF1850C8, 'T', sm_open_taskmgr,  "taskmgr"  },
    { STR_STARTMENU_SETTINGS, "Settings",       0xFFE08020, 'S', sm_open_settings, "settings" },
    { STR_STARTMENU_EXPLORER, "Explorer",       0xFF208030, 'E', sm_open_explorer, "explorer" },
    { STR_STARTMENU_BROWSER,  "Browser",        0xFF20A0C0, 'B', sm_open_browser,  "browser"  },
    { STR_STARTMENU_EDITOR,   "Text Editor",    0xFF802080, 'N', sm_open_editor,   "editor"   },
    { STR_STARTMENU_NEXSHEET, "NexSheet",       0xFF208070, 'X', sm_open_nexsheet, "nexsheet" },
    { STR_STARTMENU_USERS,    "User Manager",   0xFFA02020, 'U', sm_open_usermgr,  "usermgr"  },
    { STR_STARTMENU_DEVMGR,   "Device Manager", 0xFF606080, 'D', sm_open_devmgr,   "devmgr"   },
    { STR_STARTMENU_NEXSTORE,     "NexxStore",    0xFF30A030, 'K', sm_open_nexstore,    "nexstore"    },
    { STR_STARTMENU_AUDIOPLAYER, "Music Player",     0xFF8030A0, 'M', sm_open_audioplayer, "audioplayer" },
    { STR_STARTMENU_INSTALLER,  "Install NexxoN",   0xFF3060A0, 'I', sm_open_installer,   "installer"   },
    { STR_STARTMENU_DISPLAYS,   "Display Manager",  0xFF506080, 'L', sm_open_multimon,    "displays"    },
    { STR_STARTMENU_WIFI,       "Wi-Fi Networks",   0xFF2080A0, 'W', sm_open_wifi,        "wifi"        },
    { STR_STARTMENU_BLUETOOTH,  "Bluetooth",        0xFF2050C0, 'B', sm_open_bluetooth,   "bluetooth"   },
    { STR_STARTMENU_DOOM,       "DOOM",             0xFF7A1010, 'D', sm_open_doom,        "doom"        },
    { STR_STARTMENU_PROGRAMS,   "Programs",         0xFF3E78C8, 'P', sm_open_programs,    "programs"    },
    { STR_STARTMENU_TRASH,      "Recycle Bin",      0xFF505050, 'R', sm_open_trash,       "trash"       },
};
#define MENU_APP_COUNT ((int)(sizeof(g_start_apps) / sizeof(g_start_apps[0])))

static const menu_item_t g_start_power[] = {
    { STR_STARTMENU_RESTART,  "Restart",        0xFFA02020, '>', sm_action_restart  },
    { STR_STARTMENU_SHUTDOWN, "Shut down",      0xFF801010, 'x', sm_action_shutdown },
};
#define MENU_POWER_COUNT ((int)(sizeof(g_start_power) / sizeof(g_start_power[0])))

/* #6: the "Install NexxoN" entry must disappear once the OS is running from
 * an installed disk -- there's nothing left to install.  These helpers give
 * the rest of the menu code a compacted view of the launchable apps that
 * skips any hidden entry (no gap left behind). */
static bool menu_item_hidden(const menu_item_t *it) {
    return it->fn == sm_open_installer && nxfs_is_installed();
}
static int menu_app_count(void) {
    int n = 0;
    for (int i = 0; i < MENU_APP_COUNT; i++)
        if (!menu_item_hidden(&g_start_apps[i])) n++;
    return n;
}
static const menu_item_t *menu_app_item(int row) {
    int n = 0;
    for (int i = 0; i < MENU_APP_COUNT; i++) {
        if (menu_item_hidden(&g_start_apps[i])) continue;
        if (n == row) return &g_start_apps[i];
        n++;
    }
    return NULL;
}

/* ---------- #10: Start-menu search ------------------------------------- *
 * The bottom search box is now live: clicking it gives it keyboard focus
 * (routed in by the shell idle loop via desktop_handle_key), and typing
 * filters the left pane to matching programs AND files.  Selecting a hit
 * launches the app or opens the file's folder in Explorer. */
#define SM_SEARCH_MAX   40
#define SM_RESULT_MAX   13
typedef struct {
    char         name[NXFS_NAME_MAX];
    uint32_t     accent;
    char         glyph;
    menu_open_fn app_fn;     /* app result (NULL for a file result)       */
    uint32_t     nav_inode;  /* file result: directory to open in Explorer */
} sm_result_t;
static char        g_search[SM_SEARCH_MAX];
static int         g_search_len    = 0;
static bool        g_search_active = false;
static sm_result_t g_results[SM_RESULT_MAX];
static int         g_result_count  = 0;

static char sm_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static bool sm_match(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && sm_lc(*a) == sm_lc(*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}
static void sm_add(const char *name, uint32_t accent, char glyph,
                   menu_open_fn fn, uint32_t nav_inode) {
    if (g_result_count >= SM_RESULT_MAX) return;
    sm_result_t *r = &g_results[g_result_count++];
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = 0;
    r->accent = accent; r->glyph = glyph;
    r->app_fn = fn; r->nav_inode = nav_inode;
}
/* Recursively match filesystem entries against the query (bounded depth). */
static void sm_scan_files(uint32_t dir_ino, int depth) {
    if (depth > 3 || g_result_count >= SM_RESULT_MAX) return;
    nxfs_inode_t dir;
    if (nxfs_read_inode(dir_ino, &dir) != NXFS_OK) return;
    for (uint32_t i = 0; i < dir.child_count && g_result_count < SM_RESULT_MAX; i++) {
        nxfs_inode_t ch;
        if (nxfs_read_inode(dir.children[i], &ch) != NXFS_OK) continue;
        bool is_dir = (ch.type == NXFS_TYPE_DIR);
        if (sm_match(ch.name, g_search))
            sm_add(ch.name, 0xFF607080, is_dir ? 'D' : 'F', NULL,
                   is_dir ? dir.children[i] : dir_ino);
        if (is_dir) sm_scan_files(dir.children[i], depth + 1);
    }
}
static void sm_rebuild(void) {
    g_result_count = 0;
    if (g_search_len == 0) return;
    int an = menu_app_count();
    for (int i = 0; i < an && g_result_count < SM_RESULT_MAX; i++) {
        const menu_item_t *it = menu_app_item(i);
        if (!it) continue;
        const char *label = lang_get(it->str_id);
        if (sm_match(label, g_search) || sm_match(it->en_fallback, g_search))
            sm_add(label, it->accent, it->glyph, it->fn, 0);
    }
    if (nxfs_is_mounted()) sm_scan_files(0, 0);
}
static bool sm_searching(void) { return g_search_active && g_search_len > 0; }

static int start_menu_h(void) {
    /* Two-pane Aero layout.  Height driven by the taller of:
     *   - left pane:  border + userphoto area + app list + search + border
     *   - right pane: same outer border + power items at bottom          */
    return MENU_BORDER_W                /* top glass frame   */
         + USER_PIC_SZ + MENU_PAD      /* user picture zone */
         + menu_app_count() * MENU_ITEM_H
         + MENU_PAD + MENU_SEARCH_H    /* search box        */
         + MENU_PAD
         + MENU_BORDER_W;              /* bottom glass frame */
}

static int menu_x(void) { return START_BTN_X; }

/* ---- Start-menu open/close slide animation (ISSUE 13) ------------------ *
 * The menu slides up out of the taskbar on open and back down on close.
 * Because every menu draw + hit-test routes through menu_y(), adding the
 * animation offset here moves the whole menu as one piece.  The compositor
 * draws the menu BEFORE the panel so the part still below the taskbar top
 * is covered by it — giving a clean "emerges from the taskbar" effect. */
#define MENU_ANIM_MS  ANIM_MENU_MS    /* unified liquid timing (theme.h) */
static uint32_t g_menu_anim_ms = 0;     /* transition start time   */
static bool     g_menu_closing  = false;/* true = animating closed */

static int menu_base_y(void) { return desktop_panel_top() - start_menu_h() - 2; }

/* Vertical offset (px, >=0 slides the menu down behind the taskbar).  Driven
 * by ease-out cubic so the menu decelerates smoothly as it arrives. */
static int menu_anim_offset(void) {
    uint32_t dt    = pit_ms() - g_menu_anim_ms;
    int      slide = start_menu_h() + 4;
    if (dt >= MENU_ANIM_MS) return g_menu_closing ? slide : 0;
    int p = (int)anim_ease_out_cubic(dt, MENU_ANIM_MS);   /* 0..256, eased */
    return g_menu_closing ? (slide * p / 256)         /* 0 -> hidden */
                          : (slide * (256 - p) / 256);/* hidden -> 0 */
}

static int menu_y(void) { return menu_base_y() + menu_anim_offset(); }

/* Find the icon slot under (mx, my), or -1 if outside any. */
static int hit_icon(int mx, int my) {
    if (my >= desktop_panel_top()) return -1;
    for (int i = 0; i < DESKTOP_MAX_ICONS; i++) {
        if (!g_icons[i].in_use) continue;
        if (point_in_rect(mx, my, g_icons[i].x, g_icons[i].y,
                          DESKTOP_ICON_W, DESKTOP_ICON_H)) {
            return i;
        }
    }
    return -1;
}

/* ---------- Lifecycle -------------------------------------------------- */
/* TASK i18n v2: language-change listener.  Closes any open start menu
 * (so the next paint rebuilds with the new strings) and marks the WM
 * dirty so the taskbar + icon labels refresh in the next compose. */
static void desktop_on_language_change(lang_t new_lang) {
    (void)new_lang;
    g_menu_open = false;
    wm_mark_dirty();
}

void desktop_init(void) {
    debug_step("desktop: initialising panel + icon table (v2.0)");
    memset(g_icons, 0, sizeof(g_icons));
    g_menu_open       = false;
    g_last_click_ms   = 0;
    g_last_click_icon = -1;
    g_have_pending    = false;
    g_pending_cmd[0]  = 0;
    g_ctx_target_idx  = -1;
    g_selected_icon   = -1;
    lang_register_cb(desktop_on_language_change);
    debug_ok("desktop: ready (icon persistence available once NXFS mounts)");
}

/* ---------- Persistence ------------------------------------------------ */
/* /sys directory + /sys/desktop.cfg lookup, creating the directory on
 * first save.  Returns the file's inode (creating an empty file on first
 * call) on success, or 0 on any failure. */
static uint32_t ensure_cfg_inode(void) {
    if (!nxfs_is_mounted()) return 0;

    uint32_t root = 0;             /* always inode 0 */
    uint32_t sys_ino = 0;
    if (nxfs_resolve(root, DESKTOP_CFG_DIR, &sys_ino) != NXFS_OK) {
        if (nxfs_create_dir(root, DESKTOP_CFG_DIR, &sys_ino) != NXFS_OK) {
            debug_printf("[desktop] could not create /%s\n", DESKTOP_CFG_DIR);
            return 0;
        }
        debug_printf("[desktop] created /%s for persistence\n",
                     DESKTOP_CFG_DIR);
    }

    uint32_t cfg_ino = 0;
    if (nxfs_resolve(sys_ino, DESKTOP_CFG_FILE, &cfg_ino) != NXFS_OK) {
        if (nxfs_create_file(sys_ino, DESKTOP_CFG_FILE, &cfg_ino) != NXFS_OK) {
            debug_printf("[desktop] could not create /%s/%s\n",
                         DESKTOP_CFG_DIR, DESKTOP_CFG_FILE);
            return 0;
        }
        debug_printf("[desktop] created /%s/%s\n",
                     DESKTOP_CFG_DIR, DESKTOP_CFG_FILE);
    }
    return cfg_ino;
}

int desktop_save_icons(void) {
    uint32_t ino = ensure_cfg_inode();
    if (!ino) return -1;

    /* Format: one line per icon, "label|cmd\n".  We deliberately use '|'
     * as the delimiter because NXFS valid_name rejects it - it can never
     * appear inside a legitimate path. */
    static char buf[DESKTOP_MAX_ICONS *
                    (DESKTOP_ICON_LABEL_MAX + DESKTOP_CMD_MAX + 8)];
    uint32_t pos = 0;
    int n_written = 0;
    for (int i = 0; i < DESKTOP_MAX_ICONS; i++) {
        if (!g_icons[i].in_use) continue;
        int wrote = ksnprintf(buf + pos, sizeof(buf) - pos,
                              "%s|%s\n", g_icons[i].label, g_icons[i].cmd);
        if (wrote <= 0 || (uint32_t)wrote >= sizeof(buf) - pos) break;
        pos += (uint32_t)wrote;
        n_written++;
    }
    int r = nxfs_write_file(ino, buf, pos);
    if (r != NXFS_OK) {
        debug_printf("[desktop] save FAILED: nxfs code %d\n", r);
        return -1;
    }
    debug_printf("[desktop] saved %d icon%s (%u bytes) to /%s/%s\n",
                 n_written, n_written == 1 ? "" : "s",
                 pos, DESKTOP_CFG_DIR, DESKTOP_CFG_FILE);
    return n_written;
}

int desktop_load_icons(void) {
    if (!nxfs_is_mounted()) return 0;

    /* Look up /sys/desktop.cfg.  ABSENT is fine - just means no icons saved. */
    uint32_t sys_ino = 0;
    if (nxfs_resolve(0, DESKTOP_CFG_DIR, &sys_ino) != NXFS_OK) return 0;
    uint32_t cfg_ino = 0;
    if (nxfs_resolve(sys_ino, DESKTOP_CFG_FILE, &cfg_ino) != NXFS_OK) return 0;

    static char buf[DESKTOP_MAX_ICONS *
                    (DESKTOP_ICON_LABEL_MAX + DESKTOP_CMD_MAX + 8)];
    uint32_t got = 0;
    int r = nxfs_read_file(cfg_ino, buf, sizeof(buf) - 1, &got);
    if (r != NXFS_OK) {
        debug_printf("[desktop] load: read FAILED nxfs code %d\n", r);
        return 0;
    }
    buf[got] = 0;

    /* Wipe the in-memory table so a partial reload doesn't double-up. */
    memset(g_icons, 0, sizeof(g_icons));

    int restored = 0;
    char *p = buf;
    while (*p && restored < DESKTOP_MAX_ICONS) {
        char *bar = NULL;
        char *nl  = p;
        while (*nl && *nl != '\n') {
            if (*nl == '|' && !bar) bar = nl;
            nl++;
        }
        if (!bar || bar == p) {
            /* Malformed line - skip to next. */
            if (*nl == '\n') nl++;
            p = nl;
            continue;
        }
        *bar = 0;
        if (*nl) { *nl = 0; }

        const char *label = p;
        const char *cmd   = bar + 1;
        if (*label && *cmd) {
            icon_t *ic = &g_icons[restored];
            ic->in_use = true;
            strncpy(ic->label, label, DESKTOP_ICON_LABEL_MAX - 1);
            ic->label[DESKTOP_ICON_LABEL_MAX - 1] = 0;
            strncpy(ic->cmd, cmd, DESKTOP_CMD_MAX - 1);
            ic->cmd[DESKTOP_CMD_MAX - 1] = 0;
            compute_icon_pos(restored, &ic->x, &ic->y);
            restored++;
        }
        p = nl + (*nl ? 0 : 0);
        if (*p == '\n') p++;
    }

    /* Compact: shift to the front so slot indices are contiguous. */
    int dst = 0;
    for (int i = 0; i < DESKTOP_MAX_ICONS; i++) {
        if (g_icons[i].in_use) {
            if (dst != i) g_icons[dst] = g_icons[i];
            dst++;
        }
    }
    for (int i = dst; i < DESKTOP_MAX_ICONS; i++) {
        g_icons[i].in_use = false;
    }
    for (int i = 0; i < DESKTOP_MAX_ICONS; i++) {
        if (g_icons[i].in_use) compute_icon_pos(i, &g_icons[i].x, &g_icons[i].y);
    }

    debug_printf("[desktop] loaded %d icon%s from /%s/%s\n",
                 dst, dst == 1 ? "" : "s",
                 DESKTOP_CFG_DIR, DESKTOP_CFG_FILE);
    return dst;
}

/* ---------- Wallpaper loader ----------------------------------------- */
/* Static buffer to read /wallpaper.bmp into.  NXFS caps a single file at
 * NXFS_MAX_BLOCKS * NXFS_SECTOR_SIZE = 32 * 512 = 16 KiB which is too
 * small for a real wallpaper, but the same parser will accept anything a
 * future block-list extension can deliver.  For now we keep an 8 MiB
 * staging buffer so a wallpaper.bmp written via a future NXFS-large-file
 * path doesn't need a second code change here. */
#define WALLPAPER_STAGE_BYTES (8u * 1024u * 1024u)
ALIGNED(16) static uint8_t g_wallpaper_stage[WALLPAPER_STAGE_BYTES];

bool desktop_has_wallpaper(void) { return g_wallpaper_ok; }

bool desktop_load_wallpaper(void) {
    g_wallpaper_ok = false;
    bmp_free(&g_wallpaper);
    if (!nxfs_is_mounted()) return false;
    uint32_t ino;
    if (nxfs_resolve(0, "wallpaper.bmp", &ino) != NXFS_OK) {
        debug_printf("[desktop] no /wallpaper.bmp on the FS\n");
        return false;
    }
    uint32_t got = 0;
    int r = nxfs_read_file(ino, g_wallpaper_stage,
                           sizeof(g_wallpaper_stage), &got);
    if (r != NXFS_OK || got == 0) {
        debug_printf("[desktop] wallpaper.bmp read failed: r=%d got=%u\n",
                     r, got);
        return false;
    }
    if (!bmp_decode(g_wallpaper_stage, got, &g_wallpaper)) {
        debug_printf("[desktop] wallpaper.bmp decode failed\n");
        return false;
    }
    g_wallpaper_ok = true;
    debug_printf("[desktop] wallpaper loaded: %ux%u (%u bytes)\n",
                 g_wallpaper.width, g_wallpaper.height,
                 g_wallpaper.pixel_bytes);
    return true;
}

/* ---------- Background + icons ---------------------------------------- */
static void blit_wallpaper(draw_target_t *t) {
    /* Centred blit: clip on both axes if the image is larger than the
     * destination, otherwise pad the surrounding area with BG_BASE so
     * the empty margin doesn't show garbage. */
    int dw = (int)t->width;
    int dh = (int)t->height;
    int iw = (int)g_wallpaper.width;
    int ih = (int)g_wallpaper.height;
    int ox = (dw - iw) / 2;
    int oy = (dh - ih) / 2;
    /* Fill margin (only matters when wallpaper is smaller than screen). */
    if (ox > 0 || oy > 0) gfx_clear(t, BG_BASE);
    int x0 = ox < 0 ? 0  : ox;
    int y0 = oy < 0 ? 0  : oy;
    int x1 = ox + iw; if (x1 > dw) x1 = dw;
    int y1 = oy + ih; if (y1 > dh) y1 = dh;
    int src_x0 = ox < 0 ? -ox : 0;
    int src_y0 = oy < 0 ? -oy : 0;
    for (int y = y0; y < y1; y++) {
        uint32_t *dstrow = (uint32_t *)(t->fb + (uint32_t)y * t->pitch);
        uint32_t *srcrow = (uint32_t *)(g_wallpaper.pixels +
                                        (uint32_t)(src_y0 + (y - y0)) * (uint32_t)iw * 4u);
        int span = x1 - x0;
        memcpy(dstrow + x0, srcrow + src_x0, (uint32_t)span * 4u);
    }
}

static void draw_wallpaper(draw_target_t *t) {
    if (g_wallpaper_ok && g_wallpaper.pixels) {
        blit_wallpaper(t);
        return;
    }
    /* Built-in Aero wallpaper: deep blue vertical sweep with a wide soft
     * light bloom in the upper third — reads as "glass over sky" without
     * costing an image asset.  Row-banded so it composes fast. */
    int h = (int)t->height, w = (int)t->width;
    for (int y = 0; y < h; y++) {
        /* Brighter "Liquid Glass" Aero sky: #123E76 (top) -> #061730 (bottom)
         * so the frosted panels above pick up real light through the blur. */
        int r = 0x12 + ((0x06 - 0x12) * y) / (h ? h : 1);
        int g = 0x3E + ((0x17 - 0x3E) * y) / (h ? h : 1);
        int b = 0x76 + ((0x30 - 0x76) * y) / (h ? h : 1);
        /* Wide soft light bloom centred at 38% height: up to +44, blue-biased */
        int d = y - (h * 38) / 100;
        if (d < 0) d = -d;
        int bloom = 44 - (d * 44) / (h / 2 ? h / 2 : 1);
        if (bloom < 0) bloom = 0;
        r += bloom / 3; g += (bloom * 2) / 3; b += bloom;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        uint32_t c = 0xFF000000u | ((uint32_t)r << 16)
                   | ((uint32_t)g << 8) | (uint32_t)b;
        gfx_fill_rect(t, 0, y, w, 1, c);
    }
}

/* Subtle vertical gradient helper.  Compositor-only - we don't bother
 * exporting it through gfx.h because the only callers are the panel /
 * title bars. */
static uint32_t lerp_ch(uint32_t a, uint32_t b, int num, int den) {
    if (den <= 0) return a;
    return (uint32_t)((int32_t)a + (int32_t)(b - a) * num / den);
}
static uint32_t lerp_argb(uint32_t a, uint32_t b, int num, int den) {
    uint32_t aa = (a >> 24) & 0xFF, ab = (b >> 24) & 0xFF;
    uint32_t ra = (a >> 16) & 0xFF, rb = (b >> 16) & 0xFF;
    uint32_t ga = (a >>  8) & 0xFF, gb = (b >>  8) & 0xFF;
    uint32_t ba = (a      ) & 0xFF, bb = (b      ) & 0xFF;
    return (lerp_ch(aa, ab, num, den) << 24) |
           (lerp_ch(ra, rb, num, den) << 16) |
           (lerp_ch(ga, gb, num, den) <<  8) |
            lerp_ch(ba, bb, num, den);
}
static void fill_vgrad(draw_target_t *t, int x, int y, int w, int h,
                       uint32_t top, uint32_t bot) {
    if (h <= 0) return;
    for (int r = 0; r < h; r++) {
        uint32_t c = lerp_argb(top, bot, r, h - 1);
        gfx_draw_hline(t, x, y + r, w, c);
    }
}

/* Integer square-root (Newton) — local copy for the orb renderer. */
static int isqrt_d(int v) {
    if (v <= 0) return 0;
    int x = v, y2 = (x + 1) / 2;
    while (y2 < x) { x = y2; y2 = (x + v / x) / 2; }
    return x;
}

/* Glossy blue sphere for the Start Orb.
 * Renders a per-pixel vertical gradient circle then overlays a
 * semi-transparent white highlight ellipse in the upper portion. */
/* Anti-aliased thick line segment (a rounded capsule) — coverage derived
 * from the per-pixel distance to the segment.  Pure integer (Q8) so it runs
 * anywhere the kernel does.  This is what gives the start orb a CLEAN vector
 * "N" instead of a blocky bitmap glyph. `half_q8` = half stroke width in Q8. */
static void orb_stroke(draw_target_t *t, int x0, int y0, int x1, int y1,
                       int half_q8, uint32_t color) {
    int minx = (x0 < x1 ? x0 : x1) - 3, maxx = (x0 > x1 ? x0 : x1) + 3;
    int miny = (y0 < y1 ? y0 : y1) - 3, maxy = (y0 > y1 ? y0 : y1) + 3;
    int abx = x1 - x0, aby = y1 - y0;
    int len_sq = abx * abx + aby * aby;
    int ablen  = isqrt_d(len_sq); if (ablen < 1) ablen = 1;
    uint8_t ca = (uint8_t)((color >> 24) & 0xFFu);
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            int apx = px - x0, apy = py - y0;
            int dot = apx * abx + apy * aby;
            int dist_q8;
            if (dot <= 0) {
                dist_q8 = isqrt_d((apx * apx + apy * apy) * 65536);
            } else if (dot >= len_sq) {
                int bpx = px - x1, bpy = py - y1;
                dist_q8 = isqrt_d((bpx * bpx + bpy * bpy) * 65536);
            } else {
                int cross = abx * apy - aby * apx;
                if (cross < 0) cross = -cross;
                dist_q8 = (cross * 256) / ablen;
            }
            int cov = half_q8 + 128 - dist_q8;     /* +0.5 px AA feather */
            if (cov <= 0) continue;
            if (cov > 256) cov = 256;
            uint8_t a = (uint8_t)((cov >= 256 ? 255 : cov) * ca / 255);
            if (!a) continue;
            gfx_blend_pixel(t, px, py, (color & 0x00FFFFFFu) | ((uint32_t)a << 24));
        }
    }
}

static void draw_start_orb(draw_target_t *t, int cx, int cy, int r, bool hot) {
    uint32_t c_top = hot ? ORB_TOP_HOT : ORB_TOP;
    uint32_t c_bot = hot ? ORB_BOT_HOT : ORB_BOT;
    int r2 = r * r;

    /* ---- Base glass sphere: vertical gradient with an AA edge. -------- */
    for (int dy = -r; dy <= r; dy++) {
        int rem = r2 - dy * dy;
        if (rem <= 0) continue;
        int dx_max = isqrt_d(rem);
        uint32_t row_c = lerp_argb(c_top, c_bot, dy + r, r * 2) | 0xFF000000u;
        for (int dx = -dx_max; dx <= dx_max; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 < (r - 1) * (r - 1)) {
                gfx_putpixel(t, cx + dx, cy + dy, row_c);
            } else {
                int edge = isqrt_d(d2) - r + 1;
                int a = 255 - edge * 180;
                if (a > 0) {
                    if (a > 255) a = 255;
                    uint32_t mc = (row_c & 0x00FFFFFFu) | ((uint32_t)(uint8_t)a << 24);
                    gfx_blend_pixel(t, cx + dx, cy + dy, mc);
                }
            }
        }
    }

    /* ---- Bottom refraction rim-light: light bending through the glass
     * pools as a bright cool arc hugging the lower inner edge. ---------- */
    int inr = r - 1;
    for (int dy = 1; dy <= inr; dy++) {
        int rem = inr * inr - dy * dy;
        if (rem < 0) continue;
        int dx_max = isqrt_d(rem);
        for (int dx = -dx_max; dx <= dx_max; dx++) {
            int dist = isqrt_d(dx * dx + dy * dy);
            int band = inr - dist;            /* 0 at the inner edge */
            if (band < 0 || band > 2) continue;
            int a = 130 - band * 42 - (inr - dy) * 7;
            if (a <= 0) continue;
            if (a > 170) a = 170;
            gfx_blend_pixel(t, cx + dx, cy + dy,
                            ((uint32_t)(uint8_t)a << 24) | 0x00CFE6FFu);
        }
    }

    /* ---- Top gloss highlight: a glossy elliptical cap. ---------------- */
    int hl_rx = r * 60 / 100;
    int hl_ry = r * 34 / 100;
    int hl_cy = cy - r * 30 / 100;
    for (int dy = -hl_ry; dy <= hl_ry; dy++) {
        int rem2 = hl_rx * hl_rx - (hl_rx * hl_rx / (hl_ry * hl_ry + 1)) * dy * dy;
        if (rem2 <= 0) continue;
        int dx_max = isqrt_d(rem2);
        for (int dx = -dx_max; dx <= dx_max; dx++) {
            int dist_n = (dx * dx * 64) / (hl_rx * hl_rx + 1)
                       + (dy * dy * 64) / (hl_ry * hl_ry + 1);
            int a = (120 * (64 - dist_n)) / 64;
            if (a <= 0 || a > 120) continue;
            gfx_blend_pixel(t, cx + dx, hl_cy + dy,
                            ((uint32_t)(uint8_t)a << 24) | 0x00FFFFFFu);
        }
    }

    /* ---- Custom AA vector "N" with an engraved drop shadow. ----------- */
    int lx = cx - 5, rx = cx + 5, ty = cy - 8, by = cy + 8;
    int half = 300;                         /* ~2.3 px stroke */
    /* depth shadow, offset +1 px */
    orb_stroke(t, lx, ty + 1, lx, by + 1, half, 0x66061626u);
    orb_stroke(t, rx, ty + 1, rx, by + 1, half, 0x66061626u);
    orb_stroke(t, lx, ty + 1, rx, by + 1, half, 0x66061626u);
    /* glossy white strokes */
    orb_stroke(t, lx, ty, lx, by, half, 0xFFF4F8FEu);
    orb_stroke(t, rx, ty, rx, by, half, 0xFFF4F8FEu);
    orb_stroke(t, lx, ty, rx, by, half, 0xFFF4F8FEu);
}

/* ---------- Type-aware icon graphics --------------------------------- *
 * App shortcuts mirror their Start-menu plaque; .txt / .cfg files get a
 * text / config sheet; other files a generic sheet; a custom command a
 * neutral tile with its initial.  Shared by the desktop and (via
 * desktop_draw_shortcut_icon) the Explorer + dialog pickers. */
static bool app_icon_for_cmd(const char *cmd, uint32_t *accent, char *glyph) {
    if (!cmd) return false;
    for (int i = 0; i < MENU_APP_COUNT; i++) {
        const char *k = g_start_apps[i].cmd, *c = cmd;
        if (!k) continue;
        while (*k && *c && *c != ' ' && *k == *c) { k++; c++; }
        if (*k == 0 && (*c == 0 || *c == ' ')) {
            if (accent) *accent = g_start_apps[i].accent;
            if (glyph)  *glyph  = g_start_apps[i].glyph;
            return true;
        }
    }
    return false;
}
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static bool str_has_suffix(const char *s, const char *suf) {
    if (!s || !suf) return false;
    int ls = (int)strlen(s), lf = (int)strlen(suf);
    if (lf > ls) return false;
    for (int i = 0; i < lf; i++) if (lc(s[ls - lf + i]) != lc(suf[i])) return false;
    return true;
}
static bool str_has_dot(const char *s) {
    for (; s && *s; s++) if (*s == '.') return true;
    return false;
}

static void draw_app_plaque(draw_target_t *t, int x, int y, int sz,
                            uint32_t accent, char glyph) {
    int r = sz / 5;
    gfx_fill_round_rect(t, x, y, sz, sz, r, accent);
    gfx_blend_round_rect(t, x + 2, y + 2, sz - 4, sz / 2 - 1, r, 0x4CFFFFFFu);
    gfx_draw_round_rect(t, x, y, sz, sz, r, 0x80000000u);
    char g[2] = { glyph, 0 };
    gfx_draw_string_aa2x(t, x + (sz - 2 * FONT_GLYPH_W) / 2,
                         y + (sz - 2 * FONT_GLYPH_H) / 2, g, 0xFFFFFFFFu, 0x00000000u);
}
static void draw_paper(draw_target_t *t, int x, int y, int sz) {
    int fold = sz / 4;
    gfx_fill_round_rect(t, x + 3, y, sz - 3, sz, 3, 0xFFF6F8FCu);
    gfx_fill_rect(t, x + sz - fold, y, fold, fold, 0xFFD2D8E2u);   /* folded corner */
    gfx_draw_hline(t, x + sz - fold, y + fold, fold, 0xFF9098A4u);
    gfx_draw_round_rect(t, x + 3, y, sz - 3, sz, 3, 0xFF8890A0u);
}
static void draw_txt_icon(draw_target_t *t, int x, int y, int sz) {
    draw_paper(t, x, y, sz);
    for (int i = 0; i < 4; i++)
        gfx_fill_rect(t, x + sz / 5, y + sz / 3 + i * (sz / 8),
                      (i == 3 ? sz / 3 : sz / 2), 2, 0xFF6B7686u);
    gfx_fill_round_rect(t, x + 1, y + sz - 13, 15, 12, 2, 0xFF2A6FD0u);
    gfx_draw_string_aa(t, x + 5, y + sz - 12, "T", 0xFFFFFFFFu, 0x00000000u);
}
static void draw_cfg_icon(draw_target_t *t, int x, int y, int sz) {
    draw_paper(t, x, y, sz);
    int cx = x + sz / 2 + 1, cy = y + sz / 2, gr = sz / 5;
    gfx_fill_rect(t, cx - 2, cy - gr - 3, 4, 4, 0xFFE08020u);   /* teeth N/S/E/W */
    gfx_fill_rect(t, cx - 2, cy + gr - 1, 4, 4, 0xFFE08020u);
    gfx_fill_rect(t, cx - gr - 3, cy - 2, 4, 4, 0xFFE08020u);
    gfx_fill_rect(t, cx + gr - 1, cy - 2, 4, 4, 0xFFE08020u);
    gfx_fill_circle(t, cx, cy, gr, 0xFFE08020u);
    gfx_fill_circle(t, cx, cy, gr - 3 > 1 ? gr - 3 : 1, 0xFFF6F8FCu);
    gfx_fill_circle(t, cx, cy, 2, 0xFFE08020u);
    gfx_fill_round_rect(t, x + 1, y + sz - 13, 15, 12, 2, 0xFFD07818u);
    gfx_draw_string_aa(t, x + 5, y + sz - 12, "C", 0xFFFFFFFFu, 0x00000000u);
}
static void draw_generic_file_icon(draw_target_t *t, int x, int y, int sz) {
    draw_paper(t, x, y, sz);
    for (int i = 0; i < 3; i++)
        gfx_fill_rect(t, x + sz / 5, y + sz / 3 + i * (sz / 8), sz / 2, 2, 0xFFB0B8C4u);
}

/* Draw the icon for a (label, cmd) shortcut at (x,y) with box size sz.
 * Exposed so Explorer + the dialog pickers render identical icons. */
void desktop_draw_shortcut_icon(draw_target_t *t, int x, int y, int sz,
                                const char *label, const char *cmd) {
    uint32_t accent; char glyph;
    icon_id_t ico = icon_for_cmd(cmd);
    if (ico != ICON_NONE)                            icon_draw(t, x, y, sz, ico);
    else if (app_icon_for_cmd(cmd, &accent, &glyph)) draw_app_plaque(t, x, y, sz, accent, glyph);
    else if (str_has_suffix(label, ".txt"))          draw_txt_icon(t, x, y, sz);
    else if (str_has_suffix(label, ".cfg"))          draw_cfg_icon(t, x, y, sz);
    else if (str_has_dot(label))                     draw_generic_file_icon(t, x, y, sz);
    else {
        char first = (label && label[0]) ? label[0] : '?';
        if (first >= 'a' && first <= 'z') first = (char)(first - 'a' + 'A');
        draw_app_plaque(t, x, y, sz, 0xFF4A6FA0u, first);
    }
}

static void draw_icon(draw_target_t *t, icon_t *ic, bool selected) {
    int bx = ic->x + (DESKTOP_ICON_W - DESKTOP_ICON_BOX) / 2;
    int by = ic->y;
    int sz = DESKTOP_ICON_BOX;

    /* Soft rounded drop shadow, then the type-aware icon graphic. */
    gfx_blend_round_rect(t, bx + 2, by + 3, sz, sz, 6, 0x44000000u);
    desktop_draw_shortcut_icon(t, bx, by, sz, ic->label, ic->cmd);

    /* Label centred under the body.  Truncate visually if too long. */
    int lbl_y = by + DESKTOP_ICON_BOX + 4;
    int label_w = (int)strlen(ic->label) * FONT_GLYPH_W;
    int lbl_x = ic->x + (DESKTOP_ICON_W - label_w) / 2;
    if (lbl_x < ic->x) lbl_x = ic->x;
    uint32_t lbl_bg = selected ? ICON_LABEL_BG_SEL : ICON_LABEL_BG;
    gfx_fill_round_rect(t, lbl_x - 4, lbl_y - 2,
                        label_w + 8, FONT_GLYPH_H + 4, 3, lbl_bg);
    if (selected) {
        gfx_draw_round_rect(t, lbl_x - 4, lbl_y - 2,
                            label_w + 8, FONT_GLYPH_H + 4, 3, MENU_HI);
    }
    /* Soft dark halo + AA glyphs keep labels readable on any wallpaper. */
    gfx_draw_string_aa(t, lbl_x + 1, lbl_y + 1, ic->label,
                       0xFF101820u, 0x00000000u);
    gfx_draw_string_aa(t, lbl_x, lbl_y, ic->label,
                       ICON_LABEL_FG, 0x00000000u);
}

void desktop_draw_background(draw_target_t *t) {
    if (!t || !t->fb) return;
    draw_wallpaper(t);
    for (int i = 0; i < DESKTOP_MAX_ICONS; i++) {
        if (!g_icons[i].in_use) continue;
        compute_icon_pos(i, &g_icons[i].x, &g_icons[i].y);
        if (g_icons[i].y + DESKTOP_ICON_H > desktop_panel_top() - 4) continue;
        draw_icon(t, &g_icons[i], i == g_selected_icon);
    }
}

/* ---------- Panel ------------------------------------------------------ */

/* Remove old bevel-button helper — replaced by Aero orb */
void desktop_draw_panel(draw_target_t *t) {
    if (!t || !t->fb) return;
    int panel_top = desktop_panel_top();
    int sw        = screen_width();

    /* ---- Taskbar background: true frosted glass (spec §4) ------------- *
     * Blur the wallpaper/windows behind the 40 px bar, then blend a cool
     * translucent blue glass over it so the desktop light glows through.  */
    gfx_box_blur(t, 0, panel_top, sw, DESKTOP_PANEL_H, GLASS_BLUR_R);
    gfx_blend_rect(t, 0, panel_top, sw, DESKTOP_PANEL_H, GLASS_TASKBAR);

    /* 1 px top-edge gloss highlight + a soft second line — the "glass edge" */
    gfx_blend_rect(t, 0, panel_top,     sw, 1, GLASS_EDGE_LIGHT);
    gfx_blend_rect(t, 0, panel_top + 1, sw, 1, GLASS_GLOSS);

    /* ---- Start Orb (spec §1): 36×36 glossy blue circle, protruding    *
     * ~2 px above the taskbar top edge.  Hit area matches bounding box. */
    draw_start_orb(t, ORB_CENTER_X, orb_center_y(), ORB_RADIUS, g_menu_open);

    /* ---- Window taskbar tiles ----------------------------------------- */
    {
        window_t *wins[WM_MAX_WINDOWS];
        int n = wm_get_windows(wins, WM_MAX_WINDOWS);
        /* Tiles start after the orb's bounding box */
        int tb_x = START_BTN_X + START_BTN_W + 8;
        int tb_h = DESKTOP_PANEL_H - 8;
        int tb_y = panel_top + (DESKTOP_PANEL_H - tb_h) / 2;
        int tb_w = 190;            /* roomy enough for the 16 px face */
        int tray_cap = tray_volume_x(sw) - 16;
        for (int i = 0; i < n; i++) {
            window_t *w = wins[i];
            if (!w->in_use || w->protected) continue;
            if (tb_x + tb_w > tray_cap) break;
            uint32_t bg = w->minimized ? TILE_BG_NORMAL : TILE_BG_ACTIVE;
            /* Glass tile: rounded body + gloss wash + soft border */
            gfx_blend_rect(t, tb_x + 1, tb_y + 1, tb_w - 2, tb_h - 2, bg);
            gfx_blend_rect(t, tb_x + 1, tb_y + 1, tb_w - 2,
                           (tb_h - 2) / 2, 0x28FFFFFFu);
            gfx_blend_rect(t, tb_x,             tb_y, tb_w, 1,    TILE_BORDER);
            gfx_blend_rect(t, tb_x,             tb_y, 1, tb_h,    TILE_BORDER);
            gfx_blend_rect(t, tb_x + tb_w - 1,  tb_y, 1, tb_h,   TILE_BORDER);
            gfx_blend_rect(t, tb_x, tb_y + tb_h - 1, tb_w, 1,    TILE_BORDER);
            /* App pictogram (when the app claimed one) + title. */
            int text_x = tb_x + 8;
            if (w->icon != ICON_NONE) {
                int isz = tb_h - 10;
                icon_draw(t, tb_x + 6, tb_y + 5, isz, (icon_id_t)w->icon);
                text_x = tb_x + 6 + isz + 6;
            }
            gfx_draw_string_aa2x_clipped(t, text_x,
                            tb_y + (tb_h - 2 * FONT_GLYPH_H) / 2,
                            tb_x + tb_w - 6 - text_x, w->title,
                            0xFFFFFFFFu, 0x00000000u);
            tb_x += tb_w + 5;
        }
    }

    /* ---- Clock area --------------------------------------------------- */
    rtc_time_t rt;
    rtc_now(&rt);
    char tbuf[12], dbuf[16];
    rtc_format_time(&rt, tbuf, sizeof(tbuf));
    rtc_format_date(&rt, dbuf, sizeof(dbuf));

    /* Two-line tray clock: 16 px time over the 8 px date — the modern
     * taskbar arrangement, and far easier to read at a glance. */
    int time_w  = (int)strlen(tbuf) * FONT_GLYPH_W * 2;
    int date_w  = (int)strlen(dbuf) * FONT_GLYPH_W;
    int clock_w = time_w > date_w ? time_w : date_w;
    int tray_volx = tray_volume_x(sw);
    int clock_x = tray_volx - clock_w - 14;
    if (clock_x < START_BTN_X + START_BTN_W + 200)
        clock_x = START_BTN_X + START_BTN_W + 200;
    int clock_y = panel_top + 4;

    /* Liquid Glass clock tile: a raised frosted plaque — rounded body,
     * top gloss sweep, light bevel + inner seam — with a crisp white time
     * over a soft slate date.  Replaces the old sunken dark box. */
    int plaque_w = clock_w + 16;
    int plaque_h = DESKTOP_PANEL_H - 8;
    int plaque_x = clock_x - 8;
    int plaque_y = panel_top + 4;
    gfx_blend_round_rect(t, plaque_x, plaque_y, plaque_w, plaque_h, 5,
                         0x2CFFFFFFu);                          /* frost body */
    gfx_blend_round_rect(t, plaque_x + 1, plaque_y + 1, plaque_w - 2,
                         plaque_h / 2 - 1, 5, 0x2AFFFFFFu);      /* gloss     */
    gfx_draw_round_rect(t, plaque_x, plaque_y, plaque_w, plaque_h, 5,
                        0x5CFFFFFFu);                            /* bevel     */
    gfx_blend_rect(t, plaque_x + 2, plaque_y + plaque_h - 1,
                   plaque_w - 4, 1, 0x30000000u);                /* seat      */

    int tx0 = clock_x + (clock_w - time_w) / 2;
    gfx_draw_string_aa2x(t, tx0 + 1, clock_y + 1, tbuf,
                         0x66102030u, 0x00000000u);              /* shadow    */
    gfx_draw_string_aa2x(t, tx0, clock_y, tbuf,
                         0xFFF4F8FDu, 0x00000000u);              /* time      */
    gfx_draw_string(t, clock_x + (clock_w - date_w) / 2,
                    clock_y + 2 * FONT_GLYPH_H + 2,
                    dbuf, 0xFFC4D3E2u, 0x00000000u);             /* date      */

    /* ---- Show Desktop button (spec §1): 10 px strip, far-right edge --- *
     * 1 px left border: #FFFFFF alpha 50.                                */
    int sd_x = sw - SHOW_DESK_W;
    gfx_blend_rect(t, sd_x,     panel_top, SHOW_DESK_W, DESKTOP_PANEL_H, 0x18FFFFFFu);
    gfx_blend_rect(t, sd_x,     panel_top, 1,           DESKTOP_PANEL_H, SHOW_DESK_BORDER);
}

/* ---------- Start menu (Windows 7 two-pane Aero layout) --------------- */

/* Left-pane app row geometry */
static void menu_app_rect(int i, int *x, int *y, int *w, int *h) {
    int base_y = menu_y() + MENU_BORDER_W + USER_PIC_SZ + MENU_PAD;
    *x = menu_x() + MENU_BORDER_W + MENU_PAD;
    *y = base_y + i * MENU_ITEM_H;
    *w = MENU_LEFT_W - MENU_BORDER_W - 2 * MENU_PAD;
    *h = MENU_ITEM_H;
}

/* Right-pane power button row geometry */
static void menu_power_rect(int i, int *x, int *y, int *w, int *h) {
    int mh       = start_menu_h();
    int band_top = menu_y() + mh - MENU_BORDER_W - MENU_POWER_BAND_H + 4;
    int slot_w   = (MENU_RIGHT_W - 2 * MENU_PAD) / MENU_POWER_COUNT;
    *x = menu_x() + MENU_LEFT_W + MENU_PAD + i * slot_w;
    *y = band_top;
    *w = slot_w - 4;
    *h = MENU_POWER_BAND_H - 8;
}

/* Draw a single left-pane app item */
/* Render one left-pane row from an explicit label/accent/glyph.  Shared by
 * the app list and the #10 search results.  When `icon` is a real pictogram
 * it replaces the coloured letter plaque (Liquid Glass icon set). */
static void draw_menu_row(draw_target_t *t, int ix, int iy, int iw, int ih,
                          const char *label, uint32_t accent, char glyph,
                          icon_id_t icon, bool hot) {
    if (hot) {
        /* Spec §3 hover: subtle #FFFFFF 15 % fill, 4 px rounded, soft ring. */
        gfx_blend_round_rect(t, ix, iy, iw, ih, RADIUS_HOVER, GLASS_HOVER);
        gfx_blend_round_rect(t, ix, iy, iw, ih / 2, RADIUS_HOVER, 0x14FFFFFFu);
        gfx_draw_round_rect(t, ix, iy, iw, ih, RADIUS_HOVER, 0x40FFFFFFu);
    }

    int plaque = ih - 6;
    int gx = ix + 4, gy = iy + 3;
    if (icon != ICON_NONE) {
        /* Vector pictogram straight onto the glass (no plaque). */
        icon_draw(t, gx, gy, plaque, icon);
    } else {
        /* Coloured icon plaque: rounded, glossy, AA glyph (files/custom). */
        gfx_fill_round_rect(t, gx, gy, plaque, plaque, 3, accent);
        gfx_blend_rect(t, gx + 2, gy + 1, plaque - 4, plaque / 2, 0x46FFFFFFu);
        gfx_draw_round_rect(t, gx, gy, plaque, plaque, 3, 0x70000000u);
        char g[2] = { glyph, 0 };
        gfx_draw_string_aa(t, gx + (plaque - FONT_GLYPH_W) / 2,
                           gy + (plaque - FONT_GLYPH_H) / 2,
                           g, 0xFFFFFFFFu, 0x00000000u);
    }

    /* 16 px chrome label — the start menu is a launcher, legibility first. */
    int text_x = ix + 4 + plaque + 8;
    int text_y = iy + (ih - 2 * FONT_GLYPH_H) / 2;
    int avail_w = (ix + iw) - text_x - 4;
    uint32_t fg = hot ? MENU_ITEM_HOT_FG : MENU_ITEM_FG;
    gfx_draw_string_aa2x_clipped(t, text_x, text_y, avail_w, label,
                                 fg, 0x00000000u);
}

static void draw_menu_item_left(draw_target_t *t, int ix, int iy, int iw, int ih,
                                const menu_item_t *item, bool hot) {
    draw_menu_row(t, ix, iy, iw, ih, lang_get(item->str_id),
                  item->accent, item->glyph, icon_for_cmd(item->cmd), hot);
}

/* Power symbol: ring with gap at 12 o'clock + vertical line through centre */
static void draw_power_icon(draw_target_t *t, int cx, int cy, uint32_t col) {
    for (int dy = -7; dy <= 7; dy++) {
        for (int dx = -7; dx <= 7; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 < 25 || d2 > 49) continue;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (dy < 0 && adx * 4 < ady * 3) continue;  /* top gap ~±37° */
            gfx_blend_pixel(t, cx + dx, cy + dy, col);
        }
    }
    for (int k = -7; k <= 0; k++) {
        gfx_blend_pixel(t, cx - 1, cy + k, col);
        gfx_blend_pixel(t, cx,     cy + k, col);
    }
}

/* Circular-arrow restart icon: 300° arc (gap at bottom-right) + arrowhead */
static void draw_restart_icon(draw_target_t *t, int cx, int cy, uint32_t col) {
    for (int dy = -7; dy <= 7; dy++) {
        for (int dx = -7; dx <= 7; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 < 25 || d2 > 49) continue;
            if (dx > 2 && dy > 2) continue;   /* skip bottom-right gap */
            gfx_blend_pixel(t, cx + dx, cy + dy, col);
        }
    }
    /* Arrowhead at gap tail (right side, pointing downward) */
    gfx_blend_pixel(t, cx + 4, cy + 4, col);
    gfx_blend_pixel(t, cx + 5, cy + 3, col);
    gfx_blend_pixel(t, cx + 6, cy + 4, col);
    gfx_blend_pixel(t, cx + 5, cy + 5, col);
    gfx_blend_pixel(t, cx + 6, cy + 5, col);
}

/* Draw a power button in the right pane — icon only, no text */
static void draw_menu_item_right(draw_target_t *t, int ix, int iy, int iw, int ih,
                                 const menu_item_t *item, int idx, bool hot) {
    /* Glossy rounded glass button (accent tint + top gloss + 3-D bevel). */
    uint32_t base = item->accent & 0x00FFFFFFu;
    gfx_blend_round_rect(t, ix, iy, iw, ih, RADIUS_HOVER,
                         base | (hot ? 0xE6000000u : 0xB4000000u));
    gfx_blend_round_rect(t, ix + 1, iy + 1, iw - 2, ih / 2, RADIUS_HOVER, 0x33FFFFFFu);
    gfx_glass_bevel(t, ix, iy, iw, ih, RADIUS_HOVER, 0x66FFFFFFu, 0x40000000u);
    /* Icon centred in button area */
    int cx = ix + iw / 2;
    int cy = iy + ih / 2;
    uint32_t icon_col = 0xE0FFFFFFu;
    if (idx == 1) draw_power_icon  (t, cx, cy, icon_col);  /* Shutdown */
    else          draw_restart_icon(t, cx, cy, icon_col);  /* Restart  */
}

/* Two-pane frosted glass: blur the menu backdrop once, then blend the light
 * left pane and the bluish right pane INSIDE the rounded silhouette (so all
 * four outer corners round but the central divider stays straight).  Per-row
 * spans with circular corner insets give clean rounded corners without a
 * clip API. */
static void draw_menu_glass(draw_target_t *t, int mx, int my, int W, int H,
                            int divider_x, int radius,
                            uint32_t left, uint32_t right) {
    gfx_box_blur(t, mx, my, W, H, GLASS_BLUR_R);
    for (int row = 0; row < H; row++) {
        int inset = 0;
        if (row < radius) {
            int yy = radius - 1 - row;
            inset = radius - isqrt_d(radius * radius - yy * yy);
        } else if (row >= H - radius) {
            int yy = row - (H - radius);
            inset = radius - isqrt_d(radius * radius - yy * yy);
        }
        int y  = my + row;
        int x0 = mx + inset;
        int x1 = mx + W - inset;
        int dl = divider_x < x1 ? divider_x : x1;   /* clamp divider to span */
        int dr = divider_x > x0 ? divider_x : x0;
        if (dl > x0) gfx_blend_rect(t, x0, y, dl - x0, 1, left);
        if (x1 > dr) gfx_blend_rect(t, dr, y, x1 - dr, 1, right);
    }
}

void desktop_draw_menu(draw_target_t *t) {
    if (!t || !t->fb || !g_menu_open) return;

    /* Drive the open/close slide.  While animating, keep the compositor
     * dirty so the next frame advances the slide; finish the close once
     * the animation completes. */
    uint32_t anim_dt = pit_ms() - g_menu_anim_ms;
    if (anim_dt < MENU_ANIM_MS) {
        /* Advance the slide by damaging only the menu's travel band (left
         * MENU_W column from its top down to the screen bottom), not the whole
         * screen — so the menu no longer "builds up" frame by frame on the
         * slow LFB. */
        int mb = menu_base_y();
        wm_damage_rect(menu_x() - 4, mb - 4, MENU_W + 8, screen_height() - mb + 4);
    } else if (g_menu_closing) {
        g_menu_open    = false;
        g_menu_closing = false;
        return;
    }

    int mx  = menu_x();
    int my  = menu_y();
    int mh  = start_menu_h();

    /* ---- Two-pane frosted glass (spec §4) ----------------------------- *
     * Blur the backdrop once, blend the light left pane (~32 %) + the
     * bluish darker right pane (~60 %) inside an 8 px rounded silhouette,
     * then the 3-D glass bevel (light top/left, dark bottom/right).      */
    int divider_x = mx + MENU_LEFT_W;
    draw_menu_glass(t, mx, my, MENU_W, mh, divider_x, RADIUS_PANEL,
                    GLASS_MENU_LEFT, GLASS_MENU_RIGHT);
    gfx_glass_bevel(t, mx, my, MENU_W, mh, RADIUS_PANEL,
                    GLASS_EDGE_LIGHT, GLASS_EDGE_DARK);
    /* 1 px etched divider between the panes (dark line + light highlight) */
    gfx_blend_rect(t, divider_x,     my + RADIUS_PANEL, 1, mh - 2 * RADIUS_PANEL, 0x30000000u);
    gfx_blend_rect(t, divider_x + 1, my + RADIUS_PANEL, 1, mh - 2 * RADIUS_PANEL, 0x24FFFFFFu);

    /* Pane content rectangles (kept for the item / search / avatar layout). */
    int lp_x = mx + MENU_BORDER_W;
    int lp_w = MENU_LEFT_W - MENU_BORDER_W;
    int rp_x = mx + MENU_LEFT_W;
    int rp_w = MENU_RIGHT_W - MENU_BORDER_W;

    /* ---- Glossy glass user avatar (the "liquid glass" bust, spec) ----- */
    int pic_x = rp_x + (rp_w - USER_PIC_SZ) / 2;
    int pic_y = my + MENU_BORDER_W + 10;
    int acx   = pic_x + USER_PIC_SZ / 2;
    /* Frosted rounded backing tile with a 3-D bevel. */
    gfx_blend_round_rect(t, pic_x - 6, pic_y - 6, USER_PIC_SZ + 12, USER_PIC_SZ + 14,
                         8, 0x3AFFFFFFu);
    gfx_glass_bevel(t, pic_x - 6, pic_y - 6, USER_PIC_SZ + 12, USER_PIC_SZ + 14,
                    8, 0x66FFFFFFu, 0x33000000u);
    /* Glass shoulders + head: translucent light-blue spheres. */
    gfx_fill_circle(t, acx, pic_y + USER_PIC_SZ + 4, USER_PIC_SZ / 2 + 2, 0xFF7FA8D8u);
    int hr = USER_PIC_SZ * 30 / 100;
    gfx_fill_circle(t, acx, pic_y + hr + 5, hr + 1, 0xFF93B9E4u);
    gfx_fill_circle(t, acx, pic_y + hr + 5, hr,     0xFFB2D0EFu);
    /* Specular highlight (upper-left) — the glassy catch-light. */
    gfx_fill_circle(t, acx - hr / 3, pic_y + hr, hr / 3, 0xE8FFFFFFu);
    gfx_blend_pixel(t, acx - hr / 3, pic_y + hr, 0xFFFFFFFFu);

    /* Username centred below the picture, 10 px gap */
    const char *uname = auth_current_user();
    if (!uname || !uname[0]) uname = "NexxoN User";
    int un_w = gfx_string_pixel_width(uname) * 2;
    int un_x = rp_x + (rp_w - un_w) / 2;
    int un_y = pic_y + USER_PIC_SZ + 8;
    gfx_draw_string_aa2x(t, un_x, un_y, uname, MENU_RIGHT_FG, 0x00000000u);

    /* ---- Left pane: app list, or #10 search results while searching --- */
    int cur_mx = mouse_x(), cur_my = mouse_y();
    int app_n = menu_app_count();
    if (sm_searching()) {
        if (g_result_count == 0) {
            int ix, iy, iw, ih;
            menu_app_rect(0, &ix, &iy, &iw, &ih);
            gfx_draw_string(t, ix + 6, iy + (ih - FONT_GLYPH_H) / 2,
                            (i18n_get_language() == LANG_HU)
                              ? "Nincs talalat" : "No results",
                            0xFF808088u, 0x00000000u);
        }
        for (int i = 0; i < g_result_count && i < app_n; i++) {
            int ix, iy, iw, ih;
            menu_app_rect(i, &ix, &iy, &iw, &ih);
            bool hot = point_in_rect(cur_mx, cur_my, ix, iy, iw, ih);
            draw_menu_row(t, ix, iy, iw, ih, g_results[i].name,
                          g_results[i].accent, g_results[i].glyph,
                          ICON_NONE, hot);
        }
    } else {
        for (int i = 0; i < app_n; i++) {
            int ix, iy, iw, ih;
            menu_app_rect(i, &ix, &iy, &iw, &ih);
            bool hot = point_in_rect(cur_mx, cur_my, ix, iy, iw, ih);
            draw_menu_item_left(t, ix, iy, iw, ih, menu_app_item(i), hot);
        }
    }

    /* ---- Search box at the bottom of the left pane (spec §4, #10) ----- */
    {
        int sb_x = lp_x + MENU_PAD;
        int sb_y = my + mh - MENU_BORDER_W - MENU_SEARCH_H - MENU_PAD;
        int sb_w = lp_w - 2 * MENU_PAD;
        /* Frosted rounded input: translucent white over the blurred pane. */
        gfx_blend_round_rect(t, sb_x, sb_y, sb_w, MENU_SEARCH_H, RADIUS_HOVER, 0xCCFFFFFFu);
        /* Focused box gets a blue ring so it's clearly editable. */
        gfx_draw_round_rect(t, sb_x, sb_y, sb_w, MENU_SEARCH_H, RADIUS_HOVER,
                            g_search_active ? GLASS_ACCENT : 0x66FFFFFFu);
        gfx_blend_rect(t, sb_x + 2, sb_y + 1, sb_w - 4, 1, GLASS_GLOSS);
        int txt_y = sb_y + (MENU_SEARCH_H - FONT_GLYPH_H) / 2;
        if (g_search_len > 0) {
            gfx_draw_string_clipped(t, sb_x + 8, txt_y, sb_w - 16, g_search,
                                    GLASS_TEXT_DARK, 0x00000000u);
            if (g_search_active) {
                int cx = sb_x + 8 + g_search_len * FONT_GLYPH_W;
                gfx_fill_rect(t, cx, sb_y + 4, 1, MENU_SEARCH_H - 8, GLASS_TEXT_DARK);
            }
        } else {
            /* #10/#12: localised placeholder (was a hard-coded English string). */
            gfx_draw_string_clipped(t, sb_x + 8, txt_y, sb_w - 16,
                (i18n_get_language() == LANG_HU)
                  ? "Programok es fajlok keresese"
                  : "Search programs and files",
                GLASS_TEXT_MUTED, 0x00000000u);
        }
    }

    /* ---- Right pane: divider + power buttons -------------------------- */
    {
        int div_y = my + mh - MENU_BORDER_W - MENU_POWER_BAND_H - 2;
        gfx_blend_rect(t, rp_x + 4, div_y, rp_w - 8, 1, 0x60FFFFFFu);
    }
    for (int i = 0; i < MENU_POWER_COUNT; i++) {
        int ix, iy, iw, ih;
        menu_power_rect(i, &ix, &iy, &iw, &ih);
        bool hot = point_in_rect(cur_mx, cur_my, ix, iy, iw, ih);
        draw_menu_item_right(t, ix, iy, iw, ih, &g_start_power[i], i, hot);
    }
}

/* ---------- Click routing --------------------------------------------- */
static bool inside_start_btn(int mx, int my) {
    /* Circular hit-test for the orb (tolerant: use bounding box + 2 px) */
    int dx = mx - ORB_CENTER_X;
    int dy = my - orb_center_y();
    return (dx * dx + dy * dy) <= (ORB_RADIUS + 2) * (ORB_RADIUS + 2);
}

static bool inside_menu(int mx, int my) {
    if (!g_menu_open) return false;
    return point_in_rect(mx, my, menu_x(), menu_y(), MENU_W, start_menu_h());
}

bool desktop_handle_left_click_panel(int mx, int my) {
    /* Notification toast clicks (action buttons + dismiss) come first
     * because toasts sit above the taskbar and may overlap icon tiles. */
    if (notify_handle_click(mx, my)) return true;

    if (inside_start_btn(mx, my)) {
        desktop_toggle_menu();
        debug_printf("[desktop] start menu %s\n",
                     (g_menu_open && !g_menu_closing) ? "OPEN" : "closed");
        return true;
    }

    /* Taskbar window-tile click: restore the minimised window. */
    if (my >= desktop_panel_top()) {
        /* The system tray applets get FIRST shot at clicks (volume
         * slider, network icon, notify bell) before we even consider
         * the taskbar tiles.  This way the slider drag feels native
         * and the tile region is naturally bounded by the cluster's
         * left edge. */
        if (tray_handle_click(mx, my, false)) {
            return true;
        }
        int tb_x = START_BTN_X + START_BTN_W + 8;
        int tb_w = 140;
        int tray_cap = tray_volume_x(screen_width()) - 16;
        window_t *wins[WM_MAX_WINDOWS];
        int n = wm_get_windows(wins, WM_MAX_WINDOWS);
        for (int i = 0; i < n; i++) {
            if (!wins[i]->in_use || wins[i]->protected) continue;
            if (tb_x + tb_w > tray_cap) break;
            if (mx >= tb_x && mx < tb_x + tb_w) {
                if (wins[i]->minimized) wm_restore(wins[i]);
                else                    wm_set_focus(wins[i]);
                return true;
            }
            tb_x += tb_w + 4;
        }
        /* Clock plaque hit-test toggles the calendar popup. */
        {
            rtc_time_t rt;
            rtc_now(&rt);
            char tbuf[12], dbuf[16];
            rtc_format_time(&rt, tbuf, sizeof(tbuf));
            rtc_format_date(&rt, dbuf, sizeof(dbuf));
            int total_chars = (int)strlen(tbuf) + 2 + (int)strlen(dbuf);
            int clock_w = total_chars * FONT_GLYPH_W;
            int tray_volx = tray_volume_x(screen_width());
            int clock_x = tray_volx - clock_w - 14;
            int panel_top = desktop_panel_top();
            int clock_y = panel_top + (DESKTOP_PANEL_H - FONT_GLYPH_H) / 2;
            int plaque_x = clock_x - 7;
            int plaque_y = clock_y - 4;
            int plaque_w = clock_w + 14;
            int plaque_h = FONT_GLYPH_H + 8;
            if (mx >= plaque_x && mx < plaque_x + plaque_w &&
                my >= plaque_y && my < plaque_y + plaque_h) {
                g_calendar_open = !g_calendar_open;
                debug_printf("[desktop] calendar %s\n",
                             g_calendar_open ? "OPEN" : "closed");
                return true;
            }
        }
        if (g_menu_open) g_menu_open = false;
        if (g_calendar_open) g_calendar_open = false;
        return true;
    }
    /* Click anywhere else on the desktop closes the calendar. */
    if (g_calendar_open) g_calendar_open = false;

    if (g_menu_open && inside_menu(mx, my)) {
        /* #10: clicking the search box focuses it for keyboard input. */
        {
            int mh   = start_menu_h();
            int sb_x = menu_x() + MENU_BORDER_W + MENU_PAD;
            int sb_y = menu_y() + mh - MENU_BORDER_W - MENU_SEARCH_H - MENU_PAD;
            int sb_w = (MENU_LEFT_W - MENU_BORDER_W) - 2 * MENU_PAD;
            if (point_in_rect(mx, my, sb_x, sb_y, sb_w, MENU_SEARCH_H)) {
                g_search_active = true;
                return true;
            }
        }
        /* Left pane: search results while searching, else app quick-launch. */
        int app_n = menu_app_count();
        int rows  = sm_searching()
                  ? (g_result_count < app_n ? g_result_count : app_n) : app_n;
        for (int i = 0; i < rows; i++) {
            int ix, iy, iw, ih;
            menu_app_rect(i, &ix, &iy, &iw, &ih);
            if (point_in_rect(mx, my, ix, iy, iw, ih)) {
                if (sm_searching()) {
                    sm_result_t r = g_results[i];   /* copy before resetting */
                    g_menu_open = false;
                    g_search_active = false; g_search[0] = 0;
                    g_search_len = 0; g_result_count = 0;
                    if (r.app_fn) {
                        r.app_fn();
                    } else {
                        nxfs_set_cwd(r.nav_inode);
                        (void)apps_launch(explorer_open, "explorer");
                    }
                    return true;
                }
                const menu_item_t *it = menu_app_item(i);
                g_menu_open = false;
                if (it) {
                    debug_printf("[desktop] start menu launching '%s'\n",
                                 it->en_fallback);
                    if (it->fn) it->fn();
                }
                return true;
            }
        }
        /* Right pane: Restart / Shutdown. */
        for (int i = 0; i < MENU_POWER_COUNT; i++) {
            int ix, iy, iw, ih;
            menu_power_rect(i, &ix, &iy, &iw, &ih);
            if (point_in_rect(mx, my, ix, iy, iw, ih)) {
                g_menu_open = false;
                debug_printf("[desktop] start menu power[%d] %s\n",
                             i, g_start_power[i].en_fallback);
                if (g_start_power[i].fn) g_start_power[i].fn();
                return true;
            }
        }
        return true;
    }

    if (g_menu_open) {
        g_menu_open = false;
        return true;
    }

    return false;
}

/* ---------- Type-aware "open" routing (double-click) ------------------- *
 * A desktop icon's stored command is a HINT, not gospel.  App shortcuts
 * (cmd matches a built-in) launch verbatim, but a FILE / FOLDER icon is
 * opened BY TYPE so a double-click always lands in the right app:
 *     .txt / .cfg ........ text editor
 *     directory .......... Explorer
 *     anything else ...... text editor  (safe fallback, never a dead click)
 * This also rescues the legacy "edit <name>" form — there is no `edit`
 * shell command — by rewriting it to the working `efile <name>`. */

/* Pull the filesystem target out of an icon command, or NULL when the
 * command is a pure app / shell verb that should run as-is.  Recognises the
 * "edit/efile/open/view/explorer <name>" verb forms and a bare file name
 * (anything that is not itself a known built-in app key). */
static const char *icon_open_target(const char *cmd) {
    if (!cmd) return NULL;
    while (*cmd == ' ') cmd++;
    static const char *verbs[] = {
        "edit ", "efile ", "open ", "view ", "explorer "
    };
    for (int i = 0; i < (int)(sizeof(verbs) / sizeof(verbs[0])); i++) {
        size_t vl = strlen(verbs[i]);
        if (strncmp(cmd, verbs[i], vl) == 0) {
            const char *a = cmd + vl;
            while (*a == ' ') a++;
            return *a ? a : NULL;   /* bare "explorer" -> the app, not a file */
        }
    }
    /* A bare single token that is NOT a known app key is a file / folder. */
    if (*cmd && !strchr(cmd, ' ')) {
        uint32_t accent; char glyph;
        if (app_icon_for_cmd(cmd, &accent, &glyph)) return NULL;
        return cmd;
    }
    return NULL;   /* multi-token custom command: run exactly as written */
}

/* Build the command that actually opens what `stored` refers to, routed by
 * the target's filesystem type. */
static void desktop_open_command_for(const char *stored, char *out, size_t outsz) {
    const char *target = icon_open_target(stored);
    if (!target) {
        strncpy(out, stored ? stored : "", outsz - 1);
        out[outsz - 1] = 0;
        return;
    }
    bool is_dir = false;
    if (nxfs_is_mounted()) {
        uint32_t ino;
        int r = nxfs_resolve_path(target, &ino);
        if (r != NXFS_OK) r = nxfs_resolve(0, target, &ino);
        if (r == NXFS_OK) {
            nxfs_inode_t nd;
            if (nxfs_read_inode(ino, &nd) == NXFS_OK && nd.type == NXFS_TYPE_DIR)
                is_dir = true;
        }
    }
    if (is_dir)
        ksnprintf(out, outsz, "explorer %s", target);   /* folder -> Explorer  */
    else
        ksnprintf(out, outsz, "efile %s", target);       /* .txt/.cfg/unknown   */
}

bool desktop_handle_left_click_icon(int mx, int my, uint32_t now_ms) {
    if (my >= desktop_panel_top()) return false;

    int hit_idx = hit_icon(mx, my);
    if (hit_idx < 0) {
        g_last_click_icon = -1;
        if (g_selected_icon != -1) { g_selected_icon = -1; }
        return false;
    }

    g_selected_icon = hit_idx;
    bool is_dbl = false;
    if (g_last_click_icon == hit_idx) {
        if ((now_ms - g_last_click_ms) <= DBLCLICK_WINDOW_MS) {
            is_dbl = true;
        }
    }
    g_last_click_icon = hit_idx;
    g_last_click_ms   = now_ms;

    if (!is_dbl) return true;

    desktop_open_command_for(g_icons[hit_idx].cmd, g_pending_cmd,
                             sizeof(g_pending_cmd));
    debug_printf("[desktop] icon %d \"%s\" double-clicked -> stored '%s' -> open '%s'\n",
                 hit_idx, g_icons[hit_idx].label, g_icons[hit_idx].cmd, g_pending_cmd);
    g_have_pending = true;
    g_last_click_icon = -1;
    return true;
}

/* ---------- Right-click context menu (BUG 1) -------------------------- */
static void do_icon_open(int idx) {
    if (idx < 0 || idx >= DESKTOP_MAX_ICONS || !g_icons[idx].in_use) return;
    desktop_open_command_for(g_icons[idx].cmd, g_pending_cmd,
                             sizeof(g_pending_cmd));
    g_have_pending = true;
}

static void do_icon_rename(int idx) {
    if (idx < 0 || idx >= DESKTOP_MAX_ICONS || !g_icons[idx].in_use) return;
    char title[64];
    ksnprintf(title, sizeof(title), "Rename '%s'", g_icons[idx].label);
    char new_label[DESKTOP_ICON_LABEL_MAX];
    new_label[0] = 0;
    if (!dialog_input(title, "New icon label:",
                      g_icons[idx].label,
                      new_label, sizeof(new_label))) return;
    if (!new_label[0]) return;
    strncpy(g_icons[idx].label, new_label, DESKTOP_ICON_LABEL_MAX - 1);
    g_icons[idx].label[DESKTOP_ICON_LABEL_MAX - 1] = 0;
    desktop_save_icons();
}

static void do_icon_edit_cmd(int idx) {
    if (idx < 0 || idx >= DESKTOP_MAX_ICONS || !g_icons[idx].in_use) return;
    char title[64];
    ksnprintf(title, sizeof(title), "Command of '%s'", g_icons[idx].label);
    char new_cmd[DESKTOP_CMD_MAX];
    new_cmd[0] = 0;
    if (!dialog_input(title, "Shell command to run on double-click:",
                      g_icons[idx].cmd,
                      new_cmd, sizeof(new_cmd))) return;
    if (!new_cmd[0]) return;
    strncpy(g_icons[idx].cmd, new_cmd, DESKTOP_CMD_MAX - 1);
    g_icons[idx].cmd[DESKTOP_CMD_MAX - 1] = 0;
    desktop_save_icons();
}

static void do_icon_delete(int idx) {
    if (idx < 0 || idx >= DESKTOP_MAX_ICONS || !g_icons[idx].in_use) return;
    char line1[80];
    ksnprintf(line1, sizeof(line1),
              "Remove desktop icon '%s' ?", g_icons[idx].label);
    if (!dialog_yes_no("Confirm Remove", line1,
                       "The icon's command remains intact, only the shortcut goes.")) {
        return;
    }
    desktop_remove_icon(idx);
}

static void do_icon_properties(int idx) {
    if (idx < 0 || idx >= DESKTOP_MAX_ICONS || !g_icons[idx].in_use) return;
    char l1[80], l2[160], l3[64];
    ksnprintf(l1, sizeof(l1), "Label   : %s", g_icons[idx].label);
    ksnprintf(l2, sizeof(l2), "Command : %s", g_icons[idx].cmd);
    ksnprintf(l3, sizeof(l3), "Slot    : %d / %d",
              idx, DESKTOP_MAX_ICONS);
    const char *lines[3] = { l1, l2, l3 };
    dialog_info("Icon properties", lines, 3);
}

/* "Add Shortcut": a styled app picker (every built-in app shows its icon) +
 * a Custom command option. */
static void do_new_icon(void) {
    bool hu = (i18n_get_language() == LANG_HU);
    const char *labels[MENU_APP_COUNT + 1];   /* picker display (localised)   */
    const char *cmds  [MENU_APP_COUNT + 1];
    const char *names [MENU_APP_COUNT + 1];   /* short label for the icon     */
    int n = 0;
    for (int i = 0; i < MENU_APP_COUNT; i++) {
        if (menu_item_hidden(&g_start_apps[i])) continue;
        labels[n] = lang_get(g_start_apps[i].str_id);
        names[n]  = g_start_apps[i].en_fallback;   /* always fits, clean */
        cmds[n]   = g_start_apps[i].cmd;
        n++;
    }
    int custom = n;
    labels[n] = hu ? "Egyedi parancs..." : "Custom command...";
    cmds[n]   = "";
    n++;

    int sel = dialog_choice(hu ? "Parancsikon hozzaadasa" : "Add Shortcut",
                            hu ? "Valassz alkalmazast:"   : "Choose an app:",
                            labels, cmds, n);
    if (sel < 0) return;
    if (sel == custom) {
        char label[DESKTOP_ICON_LABEL_MAX] = {0};
        if (!dialog_input(hu ? "Egyedi parancsikon" : "Custom shortcut",
                          hu ? "Cimke:" : "Icon label:", "", label, sizeof(label))
            || !label[0]) return;
        char cmd[DESKTOP_CMD_MAX] = {0};
        if (!dialog_input(hu ? "Egyedi parancsikon" : "Custom shortcut",
                          hu ? "Parancs:" : "Shell command:", "", cmd, sizeof(cmd))
            || !cmd[0]) return;
        desktop_add_icon(label, cmd);
    } else {
        /* Use the short canonical app name for the icon label so it always
         * fits the icon slot (the picker still shows the full localised name);
         * the icon's plaque comes from the command, not the label. */
        desktop_add_icon(names[sel], cmds[sel]);
    }
}

/* Create an empty file in the NXFS root (best-effort) and drop a desktop icon
 * that opens it in the editor. */
static void create_file_shortcut(const char *name) {
    if (nxfs_is_mounted()) {
        uint32_t ino;
        if (nxfs_resolve(0, name, &ino) != NXFS_OK)
            (void)nxfs_create_file(0, name, &ino);
    }
    char cmd[DESKTOP_CMD_MAX];
    ksnprintf(cmd, sizeof(cmd), "edit %s", name);
    desktop_add_icon(name, cmd);
}

/* "New File": a type chooser (Text / Config / Custom) then a name prompt. */
static void do_new_file(void) {
    bool hu = (i18n_get_language() == LANG_HU);
    const char *labels[3] = {
        hu ? "Szoveges fajl (.txt)" : "Text file (.txt)",
        hu ? "Konfig fajl (.cfg)"   : "Config file (.cfg)",
        hu ? "Egyedi..."            : "Custom...",
    };
    const char *cmds[3] = { "x.txt", "x.cfg", "" };   /* icon hints */
    int sel = dialog_choice(hu ? "Uj fajl" : "New File",
                            hu ? "Milyen fajlt hozzunk letre?" : "What kind of file?",
                            labels, cmds, 3);
    if (sel < 0) return;
    const char *def = (sel == 0) ? "untitled.txt"
                    : (sel == 1) ? "untitled.cfg" : "untitled";
    char name[DESKTOP_ICON_LABEL_MAX] = {0};
    if (!dialog_input(hu ? "Uj fajl" : "New File",
                      hu ? "Fajlnev:" : "File name:", def, name, sizeof(name))
        || !name[0]) return;
    /* Force the chosen extension when the user didn't type one. */
    if (sel == 0 && !str_has_suffix(name, ".txt") && !str_has_dot(name)) {
        size_t l = strlen(name);
        if (l + 4 < sizeof(name)) { strcpy(name + l, ".txt"); }
    } else if (sel == 1 && !str_has_suffix(name, ".cfg") && !str_has_dot(name)) {
        size_t l = strlen(name);
        if (l + 4 < sizeof(name)) { strcpy(name + l, ".cfg"); }
    }
    create_file_shortcut(name);
}

#ifndef NEXXON_OS_VERSION
#define NEXXON_OS_VERSION "1.0"
#endif

static void do_about(void) {
    extern int      smp_cpu_count(void);
    extern uint32_t boot_info_mem_total_kib(void);

    static char l_ver[48], l_build[48], l_cpu[48], l_mem[48], l_disp[48];
    ksnprintf(l_ver,   sizeof(l_ver),   "NexxoN OS  v%s", NEXXON_OS_VERSION);
    ksnprintf(l_build, sizeof(l_build), "%s: %s", L(STR_ABOUT_BUILD), __DATE__);
    ksnprintf(l_cpu,   sizeof(l_cpu),   "%s: %d", L(STR_ABOUT_CPUS), smp_cpu_count());
    ksnprintf(l_mem,   sizeof(l_mem),   "%s: %u MiB", L(STR_ABOUT_MEMORY),
              boot_info_mem_total_kib() / 1024u);
    ksnprintf(l_disp,  sizeof(l_disp),  "%s: %ux%u", L(STR_ABOUT_DISPLAY),
              vga_width(), vga_height());

    const char *lines[10] = {
        l_ver,
        L(STR_ABOUT_TAGLINE),
        "",
        l_build,
        l_cpu,
        l_mem,
        l_disp,
        "",
        L(STR_ABOUT_FEATURES),
        L(STR_ABOUT_HINT),
    };
    dialog_info(L(STR_ABOUT_TITLE), lines, 10);
}

static void desktop_ctx_callback(int id, void *user) {
    (void)user;
    int idx = g_ctx_target_idx;
    g_ctx_target_idx = -1;
    switch (id) {
        case DT_CTX_OPEN:        do_icon_open(idx);        break;
        case DT_CTX_RENAME:      do_icon_rename(idx);      break;
        case DT_CTX_EDIT_CMD:    do_icon_edit_cmd(idx);    break;
        case DT_CTX_DELETE:      do_icon_delete(idx);      break;
        case DT_CTX_PROPERTIES:  do_icon_properties(idx);  break;
        case DT_CTX_NEW_ICON:    do_new_icon();            break;
        case DT_CTX_NEW_FILE:    do_new_file();            break;
        case DT_CTX_REFRESH:     /* nothing to do, recompose covers it */ break;
        case DT_CTX_ABOUT:       do_about();               break;
        default:
            debug_printf("[desktop] unknown ctx menu id %d\n", id);
    }
}

static void push_item(ctxmenu_item_t *items, int *n, int id, const char *label,
                      bool separator, bool dangerous, bool disabled) {
    ctxmenu_item_t *it = &items[*n];
    it->id        = id;
    it->separator = separator;
    it->dangerous = dangerous;
    it->disabled  = disabled;
    if (label) {
        strncpy(it->label, label, CTXMENU_LABEL_MAX - 1);
        it->label[CTXMENU_LABEL_MAX - 1] = 0;
    } else {
        it->label[0] = 0;
    }
    (*n)++;
}

bool desktop_handle_right_click(int mx, int my) {
    if (my >= desktop_panel_top()) return false;
    int icon_idx = hit_icon(mx, my);
    g_ctx_target_idx = icon_idx;
    g_selected_icon  = icon_idx;

    ctxmenu_item_t items[8];
    int n = 0;
    if (icon_idx >= 0) {
        push_item(items, &n, DT_CTX_OPEN,       "Open",            false, false, false);
        push_item(items, &n, 0,                 "",                true,  false, false);
        push_item(items, &n, DT_CTX_RENAME,     "Rename...",       false, false, false);
        push_item(items, &n, DT_CTX_EDIT_CMD,   "Edit command...", false, false, false);
        push_item(items, &n, DT_CTX_DELETE,     "Delete",          false, true,  false);
        push_item(items, &n, 0,                 "",                true,  false, false);
        push_item(items, &n, DT_CTX_PROPERTIES, "Properties",      false, false, false);
    } else {
        bool hu = (i18n_get_language() == LANG_HU);
        push_item(items, &n, DT_CTX_NEW_ICON, hu ? "Parancsikon hozzaadasa..." : "Add Shortcut...", false, false, false);
        push_item(items, &n, DT_CTX_NEW_FILE, hu ? "Uj fajl..."       : "New File...",      false, false, false);
        push_item(items, &n, DT_CTX_REFRESH,  hu ? "Asztal frissitese" : "Refresh desktop",  false, false, false);
        push_item(items, &n, 0,               "",                       true,  false, false);
        push_item(items, &n, DT_CTX_ABOUT,    hu ? "A NexxoN OS-rol"   : "About NexxoN OS",  false, false, false);
    }
    ctxmenu_open(mx + 4, my + 4, items, n, desktop_ctx_callback, NULL);
    return true;
}

/* Open the Start menu with a fresh search box and an upward slide. */
static void menu_begin_open(void) {
    g_menu_open     = true;
    g_menu_closing  = false;
    g_menu_anim_ms  = pit_ms();
    g_calendar_open = false;
    tray_netinfo_close();
    g_search[0] = 0; g_search_len = 0; g_result_count = 0;
    g_search_active = false;
}

/* Begin the close slide.  The menu keeps drawing (and stays "open" for
 * hit-testing purposes) until the animation completes in desktop_draw_menu. */
static void menu_begin_close(void) {
    if (!g_menu_open || g_menu_closing) return;
    g_menu_closing = true;
    g_menu_anim_ms = pit_ms();
}

/* Public toggle used by the Start button AND the Windows key (ISSUE: Win
 * key shows/hides the Start menu). */
void desktop_toggle_menu(void) {
    if (g_menu_open && !g_menu_closing) menu_begin_close();
    else                                menu_begin_open();
}

void desktop_close_menu(void) { menu_begin_close(); }
bool desktop_menu_open(void)  { return g_menu_open; }
bool desktop_calendar_open(void) { return g_calendar_open; }

/* ---------- #10: search keyboard routing ------------------------------- *
 * The start menu is not a WM window, so the shell idle loop forwards
 * keystrokes here while the search box owns input. */
bool desktop_search_active(void) { return g_menu_open && g_search_active; }

void desktop_handle_key(int c) {
    if (!g_menu_open || !g_search_active) return;
    if (c == 27) {                         /* Esc: clear / unfocus          */
        if (g_search_len > 0) { g_search[0] = 0; g_search_len = 0; g_result_count = 0; }
        else                  { g_search_active = false; }
    } else if (c == '\b') {                /* Backspace                     */
        if (g_search_len > 0) { g_search[--g_search_len] = 0; sm_rebuild(); }
    } else if (c == '\n' || c == '\r') {   /* Enter: launch first result    */
        if (g_result_count > 0) {
            sm_result_t r = g_results[0];
            g_menu_open = false; g_search_active = false;
            g_search[0] = 0; g_search_len = 0; g_result_count = 0;
            if (r.app_fn) r.app_fn();
            else { nxfs_set_cwd(r.nav_inode); (void)apps_launch(explorer_open, "explorer"); }
            return;
        }
    } else if (c >= 0x20 && c < 0x7F) {    /* printable                     */
        if (g_search_len < SM_SEARCH_MAX - 1) {
            g_search[g_search_len++] = (char)c;
            g_search[g_search_len] = 0;
            sm_rebuild();
        }
    }
    wm_mark_dirty();
}

/* ---------- Calendar popup (TASK 33) ---------------------------------- *
 * Pops out above the clock.  Always renders the current month; no
 * navigation arrows yet (next iteration will add prev/next month). */
static int days_in_month(int y, int m) {
    static const int t[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2) {
        bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
        return leap ? 29 : 28;
    }
    return t[m - 1];
}

/* Zeller's congruence — Sunday = 0 for the 1st of (y, m). */
static int first_weekday(int y, int m) {
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100;
    int J = y / 100;
    int h = (1 + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    /* Zeller: 0=Saturday … 6=Friday.  Convert to 0=Sunday. */
    return (h + 6) % 7;
}

void desktop_draw_calendar(draw_target_t *t) {
    if (!t || !t->fb || !g_calendar_open) return;
    rtc_time_t rt;
    rtc_now(&rt);

    /* Anchor the popup to the bottom-right above the clock plaque. */
    int cw       = screen_width();
    int cal_x    = cw - CAL_W - 8;
    int cal_y    = desktop_panel_top() - CAL_H - 4;
    if (cal_y < 4) cal_y = 4;

    /* Frosted-glass chrome: soft shadow + blur backdrop + frost + bevel. */
    gfx_drop_shadow_round(t, cal_x, cal_y, CAL_W, CAL_H, RADIUS_PANEL, 8);
    gfx_glass_panel(t, cal_x, cal_y, CAL_W, CAL_H, RADIUS_PANEL, GLASS_POPUP);
    /* Header strip: blue title band with a rounded top to match the body. */
    fill_vgrad(t, cal_x + 1, cal_y + 1, CAL_W - 2, CAL_HDR_H,
               CAL_HDR_TOP, CAL_HDR_BOT);
    gfx_blend_rect(t, cal_x + 2, cal_y + 1, CAL_W - 4, 1, GLASS_GLOSS);
    gfx_glass_bevel(t, cal_x, cal_y, CAL_W, CAL_H, RADIUS_PANEL,
                    GLASS_EDGE_LIGHT, GLASS_EDGE_DARK);

    /* Header: month + year.  HU and EN month lists are kept separate so
     * the calendar mirrors the active locale.  HU uses the proper
     * accented forms (Március, Május, Június, Július). */
    static const char *months_en[12] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    static const char *months_hu[12] = {
        "Január","Február","Március","Április","Május","Június",
        "Július","Augusztus","Szeptember","Október","November","December"
    };
    bool hu = (i18n_get_language() == LANG_HU);
    const char *const *months = hu ? months_hu : months_en;
    char hdr[48];
    ksnprintf(hdr, sizeof(hdr), "%s %u",
              months[(rt.month - 1) & 0x0F], rt.year);
    int hdr_w = gfx_string_pixel_width(hdr);
    gfx_draw_string(t, cal_x + (CAL_W - hdr_w) / 2,
                    cal_y + (CAL_HDR_H - FONT_GLYPH_H) / 2,
                    hdr, 0xFFFFFFFF, CAL_HDR_BOT);

    /* Day-of-week strip - locale specific abbreviations. */
    static const char *dow_en[7] = { "S", "M", "T", "W", "T", "F", "S" };
    static const char *dow_hu[7] = { "V", "H", "K", "Sze", "Cs", "P", "Szo" };
    const char *const *dow = hu ? dow_hu : dow_en;
    int dow_y = cal_y + CAL_HDR_H + 4;
    for (int i = 0; i < 7; i++) {
        int cx = cal_x + i * CAL_CELL_W;
        int lw = (int)strlen(dow[i]) * FONT_GLYPH_W;
        gfx_draw_string(t, cx + (CAL_CELL_W - lw) / 2, dow_y,
                        dow[i], 0xFF56637A, 0x00000000);
    }
    /* Separator under the DOW row. */
    gfx_draw_hline(t, cal_x + 4, dow_y + FONT_GLYPH_H + 2,
                   CAL_W - 8, 0xFFA0A0B0);

    /* Day grid. */
    int dim = days_in_month(rt.year, rt.month);
    int fw  = first_weekday(rt.year, rt.month);
    int grid_top = dow_y + FONT_GLYPH_H + 6;
    int cell_h   = (CAL_H - (grid_top - cal_y) - 6) / 6;
    for (int d = 1; d <= dim; d++) {
        int slot = fw + d - 1;
        int col  = slot % 7;
        int row  = slot / 7;
        int cx   = cal_x + col * CAL_CELL_W;
        int cy   = grid_top + row * cell_h;
        bool today = (d == rt.day);
        if (today) {
            /* Rounded glass accent pill under today's number. */
            gfx_fill_round_rect(t, cx + 1, cy, CAL_CELL_W - 2, cell_h - 1,
                                (cell_h - 1) / 2, GLASS_ACCENT);
            gfx_blend_round_rect(t, cx + 2, cy + 1, CAL_CELL_W - 4,
                                 (cell_h - 1) / 2, (cell_h - 1) / 2,
                                 0x48FFFFFFu);
        }
        char num[4];
        ksnprintf(num, sizeof(num), "%d", d);
        int lw = (int)strlen(num) * FONT_GLYPH_W;
        gfx_draw_string(t, cx + (CAL_CELL_W - lw) / 2,
                        cy + (cell_h - FONT_GLYPH_H) / 2,
                        num,
                        today ? 0xFFFFFFFF : 0xFF1C2836,
                        0x00000000);
    }
}

/* ---------- Taskbar context menu (TASK 9) ---------------------------- *
 * Right-click on a minimised window tile in the panel pops a menu with
 * Restore / Minimise / Bezárás (Close).  Because tiles only render for
 * non-protected windows, the shell window is never accidentally killed
 * through this path. */
static window_t *find_window_by_id(int id) {
    window_t *bins[WM_MAX_WINDOWS];
    int n = wm_get_windows(bins, WM_MAX_WINDOWS);
    for (int i = 0; i < n; i++) {
        if (bins[i]->in_use && bins[i]->id == id) return bins[i];
    }
    return NULL;
}

static void tile_ctx_callback(int id, void *user) {
    (void)user;
    window_t *w = find_window_by_id(g_ctx_tile_win_id);
    g_ctx_tile_win_id = 0;
    if (!w) return;
    switch (id) {
        case DT_CTX_TILE_RESTORE: wm_restore(w);        break;
        case DT_CTX_TILE_MIN:     wm_minimize(w);       break;
        case DT_CTX_TILE_CLOSE:   wm_destroy_window(w); break;
        default: break;
    }
}

bool desktop_handle_right_click_panel(int mx, int my) {
    if (my < desktop_panel_top()) return false;
    /* Tile geometry must mirror desktop_draw_panel: same start x, same
     * width, same gap.  When this drifts, the menu opens off-target. */
    int tb_x      = START_BTN_X + START_BTN_W + 8;
    int tb_w      = 140;
    window_t *bins[WM_MAX_WINDOWS];
    int n = wm_get_windows(bins, WM_MAX_WINDOWS);
    for (int i = 0; i < n; i++) {
        window_t *w = bins[i];
        if (!w->in_use || w->protected) continue;
        if (tb_x + tb_w > screen_width() - 220) break;
        if (mx >= tb_x && mx < tb_x + tb_w) {
            g_ctx_tile_win_id = w->id;
            ctxmenu_item_t items[5];
            int nitems = 0;
            push_item(items, &nitems,
                      w->minimized ? DT_CTX_TILE_RESTORE : DT_CTX_TILE_MIN,
                      w->minimized
                          ? i18n_or("btn.restore",  "Visszaallitas")
                          : i18n_or("btn.minimize", "Kicsinyit"),
                      false, false, false);
            push_item(items, &nitems, 0, "", true, false, false);
            push_item(items, &nitems, DT_CTX_TILE_CLOSE,
                      i18n_or("btn.close", "Bezaras"),
                      false, true, false);
            /* Menu opens above the click since the panel sits at the
             * bottom of the screen; pad by 4 px so the cursor doesn't
             * sit inside item 0 on open. */
            ctxmenu_open(mx + 4, my - 80, items, nitems,
                         tile_ctx_callback, NULL);
            return true;
        }
        tb_x += tb_w + 4;
    }
    return false;
}

/* ---------- Icon table ------------------------------------------------ */
/* Seed a fresh (empty) desktop with a few default shortcuts so it isn't
 * blank — and showcases the type-aware icons (app plaques + a .txt + .cfg). */
void desktop_seed_defaults(void) {
    for (int i = 0; i < DESKTOP_MAX_ICONS; i++)
        if (g_icons[i].in_use) return;          /* already populated */
    desktop_add_icon("Explorer",     "explorer");
    desktop_add_icon("Task Manager", "taskmgr");
    desktop_add_icon("NexSheet",     "nexsheet");
    desktop_add_icon("readme.txt",   "edit readme.txt");
    desktop_add_icon("system.cfg",   "edit system.cfg");
}

int desktop_add_icon(const char *label, const char *cmdline) {
    if (!label || !cmdline) return -1;
    size_t lblen = strlen(label);
    size_t cmlen = strlen(cmdline);
    if (lblen == 0 || lblen >= DESKTOP_ICON_LABEL_MAX) return -1;
    if (cmlen == 0 || cmlen >= DESKTOP_CMD_MAX) return -1;
    /* Reject labels containing the persistence delimiter so saved files
     * always parse back unambiguously. */
    if (strchr(label, '|') || strchr(label, '\n')) return -1;

    for (int i = 0; i < DESKTOP_MAX_ICONS; i++) {
        if (g_icons[i].in_use) continue;
        g_icons[i].in_use = true;
        strncpy(g_icons[i].label, label, DESKTOP_ICON_LABEL_MAX - 1);
        g_icons[i].label[DESKTOP_ICON_LABEL_MAX - 1] = 0;
        strncpy(g_icons[i].cmd, cmdline, DESKTOP_CMD_MAX - 1);
        g_icons[i].cmd[DESKTOP_CMD_MAX - 1] = 0;
        compute_icon_pos(i, &g_icons[i].x, &g_icons[i].y);
        debug_printf("[desktop] icon %d added: \"%s\" -> %s\n",
                     i, g_icons[i].label, g_icons[i].cmd);
        desktop_save_icons();
        return i;
    }
    return -1;
}

int desktop_remove_icon(int idx) {
    if (idx < 0 || idx >= DESKTOP_MAX_ICONS || !g_icons[idx].in_use) return -1;
    debug_printf("[desktop] icon %d \"%s\" removed\n",
                 idx, g_icons[idx].label);
    g_icons[idx].in_use = false;
    g_icons[idx].label[0] = 0;
    g_icons[idx].cmd[0]   = 0;
    if (g_selected_icon == idx) g_selected_icon = -1;
    /* Compact the table so deleted slots get reused immediately and the
     * grid layout stays tidy. */
    int dst = 0;
    for (int i = 0; i < DESKTOP_MAX_ICONS; i++) {
        if (g_icons[i].in_use) {
            if (dst != i) g_icons[dst] = g_icons[i];
            dst++;
        }
    }
    for (int i = dst; i < DESKTOP_MAX_ICONS; i++) {
        g_icons[i].in_use = false;
        g_icons[i].label[0] = 0;
        g_icons[i].cmd[0]   = 0;
    }
    for (int i = 0; i < DESKTOP_MAX_ICONS; i++) {
        if (g_icons[i].in_use) compute_icon_pos(i, &g_icons[i].x, &g_icons[i].y);
    }
    desktop_save_icons();
    return 0;
}

int desktop_update_icon(int idx, const char *label, const char *cmdline) {
    if (idx < 0 || idx >= DESKTOP_MAX_ICONS || !g_icons[idx].in_use) return -1;
    if (label && label[0] && strlen(label) < DESKTOP_ICON_LABEL_MAX) {
        if (strchr(label, '|') || strchr(label, '\n')) return -1;
        strncpy(g_icons[idx].label, label, DESKTOP_ICON_LABEL_MAX - 1);
        g_icons[idx].label[DESKTOP_ICON_LABEL_MAX - 1] = 0;
    }
    if (cmdline && cmdline[0] && strlen(cmdline) < DESKTOP_CMD_MAX) {
        strncpy(g_icons[idx].cmd, cmdline, DESKTOP_CMD_MAX - 1);
        g_icons[idx].cmd[DESKTOP_CMD_MAX - 1] = 0;
    }
    desktop_save_icons();
    return 0;
}

void desktop_clear_icons(void) {
    memset(g_icons, 0, sizeof(g_icons));
    g_selected_icon = -1;
    desktop_save_icons();
}

int desktop_icon_count(void) {
    int n = 0;
    for (int i = 0; i < DESKTOP_MAX_ICONS; i++) {
        if (g_icons[i].in_use) n++;
    }
    return n;
}

const char *desktop_drain_pending_command(void) {
    if (!g_have_pending) return NULL;
    g_have_pending = false;
    return g_pending_cmd;
}

bool desktop_has_pending_command(void) { return g_have_pending; }
