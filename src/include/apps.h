/* ============================================================================
 * NexxoN OS - User applications (GUI windows hosted by the WM)  (v1.0)
 * ----------------------------------------------------------------------------
 * Bundles the open-entry points of every shipping app so the shell + start
 * menu + double-clicked desktop icons can all dispatch through a single
 * header.  Each app is a singleton (the WM has only 8 slots), reopens on
 * a second call refocus the existing instance.
 *
 * Ring-3 launch policy (v1.1)
 * ---------------------------
 * Every program in NexxoN OS now starts at CPL=3.  Callers — start menu,
 * desktop icons, shell command dispatcher, keyboard shortcuts — must
 * route through `apps_launch()` instead of calling `<app>_open()`
 * directly.  apps_launch drops to ring 3, lets the CPU re-enter kernel
 * space through SYS_INVOKE for the actual work, and returns the value
 * the app produced.  The brief CPL=3 trip catches any privileged
 * instruction the app may have introduced on the boot path and keeps
 * the ring transition exercised on every launch.
 * ============================================================================ */
#ifndef NEXXON_APPS_H
#define NEXXON_APPS_H

#include "types.h"

/* Centralised ring-3 launcher.  `open_fn` is a public app entry point
 * such as `browser_open` / `gephaz_open` / `taskmgr_open` — anything
 * whose body returns bool and takes no arguments.  Returns whatever the
 * app returned.  Safe to call from any kernel context that has the
 * shell-owned ESP0 stack underneath it (i.e. anything reached from the
 * shell idle loop or the desktop dispatcher). */
typedef bool (*app_open_fn_t)(void);
bool apps_launch(app_open_fn_t open_fn, const char *name);

/* Graphical task manager.  Lists every live WM window and offers a "Kill"
 * button per row.  Killing a window destroys its slot through wm_destroy_
 * window() so the resources go straight back to the pool.  Replacement
 * for the legacy `taskmgr` system-monitor text dump (which now lives as
 * a tab inside this window). */
bool taskmgr_open    (void);
void taskmgr_tick    (void);      /* once-per-second clock + refresh   */

/* PCI device manager.  Enumerates the entire bus once at open time and
 * renders the result as a scrollable table:  Bus:Dev.Fn  Vendor:Device
 * Class  Subclass  Prog-IF  Description. */
bool devmgr_open     (void);

/* Pong.  Two paddles, one ball, mouse controls the right paddle, AI runs
 * the left.  First to 5 wins.  Used as a stress test for the compositor
 * and mouse-driven input. */
bool pong_open       (void);
void pong_tick       (void);

/* Screensaver.  Activated automatically when keyboard + mouse have been
 * idle for IDLE_TIMEOUT_MS milliseconds.  Renders a bouncing "NexxoN OS"
 * logo in a full-screen modal until the user touches an input device. */
void screensaver_tick      (void);
void screensaver_kick      (void);   /* called from any input IRQ      */
bool screensaver_active    (void);

/* NexxoN Browser - minimal HTTP/1.1 GET client + text/HTML renderer
 * driven by an address bar and a single render pane.  TCP socket
 * machinery lives in net.c. */
bool browser_open          (void);
void browser_tick          (void);

/* Gépház - central system configuration "Settings" panel.  Tabs:
 *   Display  (wallpaper / theme)
 *   Network  (DHCP / static IP / mask / gateway)
 *   Input    (mouse sensitivity / keyboard repeat).
 * Changes auto-persist into /sys/desktop.cfg through the desktop module. */
bool gephaz_open           (void);
void gephaz_tick           (void);
/* Persistence: save current settings to /sys/gephaz.cfg, and re-load
 * them at boot (called before the login screen).  Both no-op silently
 * if NXFS isn't mounted yet. */
void gephaz_save_settings  (void);
void gephaz_load_settings  (void);

/* Login + User Manager (auth-backed). */
void login_show            (void);
bool usermgr_open          (void);

/* NexSheet — Excel-style spreadsheet editor.  Empty argument opens a
 * fresh untitled workbook; pass an NXFS path / inode through the
 * separate nexsheet_open_file() to load an existing .xlsx or .csv. */
bool nexsheet_open         (void);
void nexsheet_open_file    (const char *display_name);
void nexsheet_tick         (void);

/* Recycle bin. */
bool trash_open            (void);
int  trash_move_in         (const char *src_path);
int  trash_restore         (int idx);
int  trash_empty           (void);
int  trash_count           (void);

/* Audio Player. */
bool audioplayer_open      (void);
void audioplayer_tick      (void);
void audioplayer_open_file (const char *path);

/* Graphical Installer. */
bool installer_open  (void);

/* Image Viewer — displays BMP / PNG / JPEG images from NXFS. */
bool imgview_open      (void);
void imgview_tick      (void);
void imgview_open_file (const char *path);

#endif /* NEXXON_APPS_H */
