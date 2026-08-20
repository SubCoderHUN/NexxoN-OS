/* ============================================================================
 * NexxoN OS - Global localisation engine (v2.0)
 * ----------------------------------------------------------------------------
 * Refactor over v1.0:
 *
 *   * String IDs are now compile-time `lang_id_t` enum constants
 *     (STR_*) instead of dotted-namespace strings.  The lookup table
 *     is a flat array indexed by the enum, so dispatch is O(1) and
 *     missing IDs become compile errors instead of silent fallbacks.
 *
 *   * Every visible string in the OS funnels through lang_get(id) or
 *     the equivalent shorter macro L(id).  Drawing primitives, dialog
 *     builders, taskbar tiles, menu entries, status bars, kernel boot
 *     messages and panic-screen text all use the same path.
 *
 *   * Listener registration: any subsystem can call lang_register_cb()
 *     to receive a callback on every language change.  i18n_set_language
 *     fires every registered callback exactly once, then triggers the
 *     WM's global repaint so the new strings appear within the next
 *     compose tick.
 *
 *   * Hungarian translations are kept the same byte length or shorter
 *     than the English originals wherever possible, and clip points
 *     in the rendering layer use gfx_draw_string_clipped so longer
 *     HU strings truncate with "..." rather than overflowing widget
 *     bounds.
 *
 * Backward-compatible: the old i18n(key) string lookup is still
 * supported during the migration so subsystems that haven't been
 * converted yet keep working.
 * ============================================================================ */
#ifndef NEXXON_I18N_H
#define NEXXON_I18N_H

#include "types.h"

typedef enum {
    LANG_EN = 0,
    LANG_HU = 1,
} lang_t;

/* ============================================================================
 * String ID table - one entry per visible string in the OS.
 *
 * Convention: STR_<area>_<role>.  Order matters only for the table
 * lookup (every STR_* enum value indexes the same row in the EN/HU
 * arrays); new entries always go at the bottom, immediately above
 * STR__COUNT, so existing translations don't shift.
 * ============================================================================ */
typedef enum {
    /* ---- App titles -------------------------------------------------- */
    STR_APP_SHELL,
    STR_APP_SETTINGS,
    STR_APP_TASKMGR,
    STR_APP_USERMGR,
    STR_APP_EXPLORER,
    STR_APP_BROWSER,
    STR_APP_EDITOR,
    STR_APP_TRASH,
    STR_APP_DEVMGR,
    STR_APP_PONG,
    STR_APP_SCREENSAVER,
    STR_APP_NEXSHEET,

    /* ---- Generic UI verbs ------------------------------------------- */
    STR_BTN_OK,
    STR_BTN_CANCEL,
    STR_BTN_YES,
    STR_BTN_NO,
    STR_BTN_SAVE,
    STR_BTN_LOAD,
    STR_BTN_CLOSE,
    STR_BTN_MINIMIZE,
    STR_BTN_RESTORE,
    STR_BTN_DELETE,
    STR_BTN_RENAME,
    STR_BTN_OPEN,
    STR_BTN_NEW,
    STR_BTN_EDIT,
    STR_BTN_PROPERTIES,
    STR_BTN_EMPTY_TRASH,
    STR_BTN_KILL_PROCESS,
    STR_BTN_REFRESH,
    STR_BTN_BROWSE,
    STR_BTN_APPLY,
    STR_BTN_RESET,
    STR_BTN_CYCLE,
    STR_BTN_TEST,
    STR_BTN_VOLUME_UP,
    STR_BTN_VOLUME_DOWN,
    STR_BTN_MUTE,
    STR_BTN_UNMUTE,

    /* ---- Login / auth ----------------------------------------------- */
    STR_LOGIN_WELCOME,
    STR_LOGIN_USERNAME,
    STR_LOGIN_PASSWORD,
    STR_LOGIN_SIGNIN,
    STR_LOGIN_BAD,
    STR_LOGIN_LOCKED,

    /* ---- Start menu (TASK 10) --------------------------------------- */
    STR_STARTMENU_SHUTDOWN,
    STR_STARTMENU_RESTART,
    STR_STARTMENU_SLEEP,
    STR_STARTMENU_LOCK,
    STR_STARTMENU_LOGOFF,
    STR_STARTMENU_SETTINGS,
    STR_STARTMENU_USERS,
    STR_STARTMENU_TASKMGR,
    STR_STARTMENU_DEVMGR,
    STR_STARTMENU_BROWSER,
    STR_STARTMENU_EXPLORER,
    STR_STARTMENU_EDITOR,
    STR_STARTMENU_NEXSHEET,
    STR_STARTMENU_DOOM,
    STR_STARTMENU_PROGRAMS,
    STR_APP_PROGRAMS,
    STR_PROG_HDR,
    STR_PROG_RUN,
    STR_PROG_REMOVE,
    STR_PROG_INSTALL_DEMO,
    STR_PROG_REFRESH,
    STR_PROG_EMPTY,
    STR_PROG_COUNT_FMT,
    STR_STARTMENU_TRASH,
    STR_STARTMENU_SHELL,
    STR_STARTMENU_SEARCH_HINT,
    STR_STARTMENU_ALL_APPS,
    STR_STARTMENU_RECENT,

    /* ---- Taskbar ---------------------------------------------------- */
    STR_TASKBAR_START,
    STR_TASKBAR_VOLUME,
    STR_TASKBAR_CLOCK,
    STR_TASKBAR_NETWORK,
    STR_TASKBAR_BATTERY,
    STR_TASKBAR_CLOSE,

    /* ---- Settings (Gephaz) tabs ------------------------------------ */
    STR_SETTINGS_TITLE,
    STR_SETTINGS_TAB_PERF,
    STR_SETTINGS_TAB_NET,
    STR_SETTINGS_TAB_THEME,
    STR_SETTINGS_TAB_STORE,
    STR_SETTINGS_TAB_AUDIO,
    STR_SETTINGS_TAB_SECURITY,
    STR_SETTINGS_TAB_LANG,
    STR_SETTINGS_TAB_USERS,

    /* ---- Settings: Performance tab --------------------------------- */
    STR_SET_PERF_HDR,
    STR_SET_PERF_CPU,
    STR_SET_PERF_RAM,
    STR_SET_PERF_UPTIME,
    STR_SET_PERF_FBPOOL,

    /* ---- Settings: Network tab ------------------------------------- */
    STR_SET_NET_HDR,
    STR_SET_NET_MAC,
    STR_SET_NET_IPV4,
    STR_SET_NET_NETMASK,
    STR_SET_NET_GATEWAY,
    STR_SET_NET_DNS,
    STR_SET_NET_MODE,
    STR_SET_NET_DHCP,
    STR_SET_NET_STATIC,
    STR_SET_NET_BTN_DHCP_ON,
    STR_SET_NET_BTN_DHCP_OFF,
    STR_SET_NET_BTN_RENEW,
    STR_SET_NET_BTN_SET_IP,
    STR_SET_NET_BTN_SET_GW,
    STR_SET_NET_BTN_SET_DNS,
    STR_SET_NET_BTN_SET_DNS2,

    /* ---- Settings: Themes tab -------------------------------------- */
    STR_SET_THEME_HDR,
    STR_SET_THEME_WALLPAPER,
    STR_SET_THEME_ACCENT,
    STR_SET_THEME_NEXT_COLOR,
    STR_SET_THEME_RELOAD_WP,
    STR_SET_THEME_MOUSE,
    STR_SET_THEME_KEYBOARD,
    STR_SET_THEME_RATE_SLOW,
    STR_SET_THEME_RATE_NORM,
    STR_SET_THEME_RATE_FAST,
    STR_SET_THEME_APPEARANCE,
    STR_SET_THEME_LIGHT,
    STR_SET_THEME_DARK,

    /* ---- Settings: Storage tab ------------------------------------- */
    STR_SET_STORE_HDR,
    STR_SET_STORE_INODES,
    STR_SET_STORE_BLOCKS,
    STR_SET_STORE_SATA,
    STR_SET_STORE_TIP,

    /* ---- Settings: Audio tab --------------------------------------- */
    STR_SET_AUDIO_HDR,
    STR_SET_AUDIO_VOLUME,
    STR_SET_AUDIO_MUTED,

    /* ---- Settings: Security tab ------------------------------------ */
    STR_SET_SEC_HDR,
    STR_SET_SEC_USER,
    STR_SET_SEC_ROLE,
    STR_SET_SEC_TOTAL,
    STR_SET_SEC_ROLE_ADMIN,
    STR_SET_SEC_ROLE_USER,
    STR_SET_SEC_OPEN_USERS,
    STR_SET_SEC_CHANGE_PW,
    STR_SET_SEC_LANG,
    STR_SET_SEC_SWITCH_EN,
    STR_SET_SEC_SWITCH_HU,
    STR_GH_MOUSE_SEC,         /* "Mouse" / "Egér" group title           */
    STR_GH_MOUSE_SENS,        /* "Pointer sensitivity" / "Egér érzék."  */
    STR_GH_SLOWER,            /* "Slow" / "Lassú"                       */
    STR_GH_FASTER,            /* "Fast" / "Gyors"                       */

    /* ---- Task Manager (TASK 15) ------------------------------------ */
    STR_TM_TAB_APPS,
    STR_TM_TAB_PROCS,
    STR_TM_TAB_SVC,
    STR_TM_TAB_PERF,
    STR_TM_TAB_NET,
    STR_TM_TAB_USERS,
    STR_TM_MENU_FILE,
    STR_TM_MENU_OPTIONS,
    STR_TM_MENU_VIEW,
    STR_TM_MENU_HELP,
    STR_TM_COL_NAME,
    STR_TM_COL_USER,
    STR_TM_COL_CPU,
    STR_TM_COL_MEM,
    STR_TM_COL_DESC,
    STR_TM_COL_PID,
    STR_TM_COL_STATE,
    STR_TM_STATE_RUN,
    STR_TM_STATE_HUNG,
    STR_TM_STATE_STOPPED,
    STR_TM_KILL_PROMPT,
    STR_TM_NEW_TASK,

    /* ---- Explorer (TASK 17) ---------------------------------------- */
    STR_XP_PARENT,
    STR_XP_FOLDER,
    STR_XP_FILE,
    STR_XP_NITEMS,
    STR_XP_SELECTED,
    STR_XP_CTX_OPEN,
    STR_XP_CTX_DELETE,
    STR_XP_CTX_RENAME,
    STR_XP_CTX_PROPERTIES,
    STR_XP_CTX_NEW_FOLDER,
    STR_XP_CTX_NEW_FILE,
    STR_XP_SIDEBAR_HOME,
    STR_XP_SIDEBAR_ROOT,
    STR_XP_SIDEBAR_SYS,
    STR_XP_SIDEBAR_USB,
    STR_XP_ADDR_LABEL,

    /* ---- Recycle Bin (TASK 16) ------------------------------------- */
    STR_TRASH_HDR,
    STR_TRASH_EMPTY,
    STR_TRASH_RESTORE,
    STR_TRASH_PURGE,
    STR_TRASH_NITEMS,

    /* ---- Editor (TASK 18) ------------------------------------------ */
    STR_EDIT_NEW,
    STR_EDIT_OPEN,
    STR_EDIT_SAVE,
    STR_EDIT_SAVED,
    STR_EDIT_MODIFIED,
    STR_EDIT_FIND,
    STR_EDIT_FIND_PROMPT,
    STR_EDIT_CUT,
    STR_EDIT_COPY,
    STR_EDIT_PASTE,
    STR_EDIT_CLOSE_CONFIRM,
    STR_EDIT_LINE_NUM,

    /* ---- Browser (TASK 7) ------------------------------------------ */
    STR_BR_BACK,
    STR_BR_FORWARD,
    STR_BR_RELOAD,
    STR_BR_HOME,
    STR_BR_STOP,
    STR_BR_LOADING,
    STR_BR_ADDR_HINT,
    STR_BR_ERR_DNS,
    STR_BR_ERR_TCP,
    /* Browser status / dialog strings (i18n sweep — were hardcoded EN). */
    STR_BR_IDLE,
    STR_BR_NAVIGATING,
    STR_BR_BACK_TO,
    STR_BR_REFRESHING,
    STR_BR_INVALID_IP,
    STR_BR_CONNECTING,
    STR_BR_CONNECTED,
    STR_BR_CONN_LOST,
    STR_BR_JPEG_TRUNC,
    STR_BR_JPEG_FAIL,
    STR_BR_BAD_JPEG,
    STR_BR_INCOMPLETE_JPEG,
    STR_BR_CONNECTED_SINCE,
    STR_BR_DISCONNECTED,
    STR_BR_PROXY_CHANGED,
    STR_BR_CONN_FAILED,
    STR_BR_CONN_TIMEOUT,
    STR_BR_READY,
    STR_BR_NO_FRAMES,
    STR_BR_PROXY_SETTINGS,
    STR_BR_PROXY_IP_PROMPT,
    STR_BR_NEED_NIC1,
    STR_BR_NEED_NIC2,
    STR_BR_GO,

    /* ---- Dialog generic -------------------------------------------- */
    STR_DLG_INPUT_HINT,
    STR_DLG_CONFIRM_TITLE,
    STR_DLG_ERROR_TITLE,
    STR_DLG_INFO_TITLE,
    STR_DLG_UNSAVED_TITLE,
    STR_DLG_UNSAVED_BODY,
    STR_DLG_UNSAVED_QUESTION,

    /* ---- Status / generic messages --------------------------------- */
    STR_MSG_READY,
    STR_MSG_LOADING,
    STR_MSG_DONE,
    STR_MSG_FAILED,
    STR_MSG_NO_NETWORK,
    STR_MSG_NO_DEVICE,
    STR_MSG_INSUFFICIENT_PERM,
    STR_MSG_INVALID_INPUT,
    STR_MSG_OPERATION_OK,

    /* ---- Boot / kernel banners ------------------------------------- */
    STR_BOOT_WELCOME,
    STR_BOOT_LOADING,
    STR_BOOT_READY,
    STR_BOOT_LIVE_MODE,
    STR_BOOT_INSTALLED_MODE,

    /* ---- Panic / RSOD ---------------------------------------------- */
    STR_PANIC_TITLE,
    STR_PANIC_CAUSE_CPU,
    STR_PANIC_CAUSE_KERNEL,
    STR_PANIC_HALTED,
    STR_PANIC_RECOVERABLE,
    STR_PANIC_RECOVERED,

    /* ---- Shell help / outputs -------------------------------------- */
    STR_SHELL_PROMPT_USER,
    STR_SHELL_HELP_HEADER,
    STR_SHELL_HELP_MORE,
    STR_SHELL_UNKNOWN_CMD,
    STR_SHELL_CWD,
    STR_SHELL_HELP_FS,
    STR_SHELL_HELP_NET,
    STR_SHELL_HELP_SYS,
    STR_SHELL_HELP_APPS,

    /* ---- NexxStore (package manager GUI) ------------------------------- */
    STR_APP_NEXSTORE,
    STR_STARTMENU_NEXSTORE,
    STR_NXST_SEARCH,
    STR_NXST_INSTALL,
    STR_NXST_UNINSTALL,
    STR_NXST_INSTALLED,
    STR_NXST_AVAILABLE,
    STR_NXST_NO_NET,
    STR_NXST_EMPTY,
    STR_NXST_CONFIRM_INST,
    STR_NXST_CONFIRM_UNINST,
    STR_NXST_SUCCESS,

    /* ---- Explorer: USB / external drives ------------------------------ */
    STR_XP_SIDEBAR_DRIVES,
    STR_XP_USB_EJECT,
    STR_XP_USB_READONLY,
    STR_XP_USB_COPIED,

    /* ---- Audio Player ------------------------------------------------- */
    STR_APP_AUDIOPLAYER,
    STR_STARTMENU_AUDIOPLAYER,
    STR_AP_PLAY,
    STR_AP_PAUSE,
    STR_AP_STOP,
    STR_AP_OPEN,
    STR_AP_NO_FILE,
    STR_AP_PLAYING,
    STR_AP_PAUSED,
    STR_AP_STOPPED,
    STR_AP_VOLUME,
    STR_AP_DURATION,

    /* ---- Power Management --------------------------------------------- */
    STR_PM_SUSPEND,
    STR_PM_SUSPEND_CONFIRM,
    STR_PM_CPUFREQ,
    STR_PM_FREQ_LOW,
    STR_PM_FREQ_HIGH,
    STR_PM_FREQ_LABEL,

    /* ---- Graphical Installer ------------------------------------------ */
    STR_APP_INSTALLER,
    STR_STARTMENU_INSTALLER,
    STR_INST_TITLE,
    STR_INST_WELCOME,
    STR_INST_SELECT_DISK,
    STR_INST_CONFIRM,
    STR_INST_PROGRESS,
    STR_INST_DONE,
    STR_INST_FAILED,
    STR_INST_NO_DISK,
    STR_INST_ALREADY,
    STR_INST_BACK,
    STR_INST_NEXT,
    STR_INST_INSTALL,
    /* Wizard step-indicator labels (were hardcoded EN). */
    STR_INST_STEP_WELCOME,
    STR_INST_STEP_DISK,
    STR_INST_STEP_CONFIRM,
    STR_INST_STEP_INSTALL,
    STR_INST_STEP_DONE,

    /* ---- Notifications (action buttons) ------------------------------- */
    STR_NOTIFY_DISMISS,

    /* ---- Shell (pipe / redirect) -------------------------------------- */
    STR_SHELL_PIPE_ERR,
    STR_SHELL_REDIR_ERR,

    /* ---- Multi-monitor ------------------------------------------------ */
    STR_STARTMENU_DISPLAYS,

    /* ---- Wi-Fi -------------------------------------------------------- */
    STR_STARTMENU_WIFI,

    /* ---- Bluetooth ---------------------------------------------------- */
    STR_APP_BLUETOOTH,
    STR_STARTMENU_BLUETOOTH,
    STR_BT_TITLE,
    STR_BT_NO_HW,
    STR_BT_SCAN_PROMPT,
    STR_BT_PAIRED,
    STR_BT_BTN_SCAN,
    STR_BT_BTN_PAIR,
    STR_BT_CLS_PHONE,
    STR_BT_CLS_AUDIO,
    STR_BT_CLS_KEYS,
    STR_BT_CLS_MOUSE,
    STR_BT_CLS_PC,
    STR_BT_CLS_UNK,
    STR_BT_NOTIFY_TITLE,
    STR_BT_NOTIFY_SCAN_OK,
    STR_BT_NOTIFY_PAIRED,
    STR_BT_NOTIFY_FAIL,

    /* ---- Display / Resolution ----------------------------------------- */
    STR_SETTINGS_TAB_DISPLAY,
    STR_SETTINGS_TAB_MOUSE,       /* "Mouse & Kbd" / "Egér/Bill." tab        */
    STR_DISP_TITLE,
    STR_DISP_RESOLUTION,
    STR_DISP_CURRENT,
    STR_DISP_APPLY,
    STR_DISP_NO_MODES,
    STR_DISP_KEEP_TITLE,
    STR_DISP_KEEP_MSG,
    STR_DISP_KEEP_BTN,
    STR_DISP_REVERT_BTN,
    STR_DISP_SWITCH_OK,
    STR_DISP_SWITCH_FAIL,
    STR_DISP_REVERTED,

    /* ---- Image Viewer ------------------------------------------------- */
    STR_APP_IMGVIEW,
    STR_STARTMENU_IMGVIEW,
    STR_IV_OPEN,
    STR_IV_ZOOM_IN,
    STR_IV_ZOOM_OUT,
    STR_IV_FIT,
    STR_IV_ACTUAL,
    STR_IV_NO_FILE,
    STR_IV_LOAD_ERR,
    STR_IV_FORMAT_ERR,

    /* ---- Wi-Fi manager (de-hardcoded) --------------------------------- */
    STR_WIFI_NO_HW,
    STR_WIFI_CONNECTED,
    STR_WIFI_NETS_FOUND,
    STR_WIFI_SCAN_HINT,
    STR_WIFI_PASSWORD,
    STR_WIFI_BTN_SCAN,
    STR_WIFI_BTN_CONNECT,
    STR_WIFI_BTN_DISCONNECT,
    STR_WIFI_SCAN_DONE,
    STR_WIFI_CONNECT_OK,
    STR_WIFI_CONNECT_FAIL,
    STR_WIFI_DISCONNECTED,
    STR_WIFI_SEC_OPEN,

    /* ---- Display / Screen manager (de-hardcoded) ---------------------- */
    STR_DISP_MON_DETECTED,
    STR_DISP_PRIMARY,
    STR_DISP_MODES,
    STR_DISP_NOTIFY_MSG,
    STR_DISP_STATUS,

    /* ---- Screensaver -------------------------------------------------- */
    STR_SCREENSAVER_HINT,

    /* ---- About NexxoN OS ---------------------------------------------- */
    STR_ABOUT_TITLE,
    STR_ABOUT_TAGLINE,
    STR_ABOUT_BUILD,
    STR_ABOUT_CPUS,
    STR_ABOUT_MEMORY,
    STR_ABOUT_DISPLAY,
    STR_ABOUT_FEATURES,
    STR_ABOUT_HINT,

    /* ---- USB removable storage + Explorer VFS operations -------------- */
    STR_USB_TITLE,            /* notification title                        */
    STR_USB_MOUNTED,          /* "USB drive connected: %s"                 */
    STR_USB_REMOVED,          /* "USB drive removed: %s"                   */
    STR_XP_CTX_COPY,
    STR_XP_CTX_PASTE,
    STR_XP_CTX_REFRESH,
    STR_XP_NEWDIR_PROMPT,
    STR_XP_RENAME_PROMPT,
    STR_XP_DELETE_TITLE,
    STR_XP_DELETE_Q,          /* "Delete '%s' ?"                           */
    STR_XP_DELETE_RECURSIVE,
    STR_XP_OP_FAILED,
    STR_XP_NAME_TAKEN,
    STR_XP_BAD_SHORTNAME,
    STR_XP_FILE_TOO_BIG,
    STR_XP_COPIED,            /* "Copied: %s"                              */
    STR_XP_PASTE_DONE,        /* "Pasted: %s"                              */
    STR_XP_READ_ONLY,
    STR_EDIT_SAVED_FMT,       /* "Saved %u bytes."                         */
    STR_EDIT_SAVE_FAIL_FMT,   /* "Save FAILED (code=%d)."                  */

    /* ---- Browser proxy auto-discovery ---------------------------------- */
    STR_BR_PROXY_TITLE,       /* notification title                        */
    STR_BR_PROXY_FOUND,       /* "Proxy discovered: %s"                    */

    /* ---- Linux compatibility runtime + Programs status ---------------- */
    STR_LINUX_ALREADY_RUNNING,
    STR_LINUX_LOAD_FAILED,
    STR_LINUX_ENTRY_UNSUPPORTED,
    STR_LINUX_STACK_FAILED,
    STR_LINUX_TERMINATED_FMT,
    STR_LINUX_EXITED_FMT,
    STR_LINUX_NOT_FOUND_FMT,
    STR_LINUX_READ_FAILED,
    STR_LINUX_IMAGE_TOO_LARGE,
    STR_LINUX_INTERP_FAILED,
    STR_LINUX_LOADING_FMT,
    STR_LINUX_RUNNING_DEMO_FMT,
    STR_LINUX_RUNNING_DYN_FMT,
    STR_LINUX_SUBSYSTEM_ENTER,
    STR_LINUX_RESULT_FMT,
    STR_LINUX_OK,
    STR_LINUX_FAILED,
    STR_PROG_DIR_FAILED,
    STR_PROG_CREATE_FAILED,
    STR_PROG_INSTALLED_FMT,
    STR_PROG_WRITE_FAILED,
    STR_PROG_RESULT_FMT,
    STR_PROG_REMOVED_FMT,
    STR_PROG_REMOVE_FAILED,
    STR_PROG_SIZE_BYTES_FMT,
    STR_PROG_INSTALLED_TESTS_FMT,
    STR_PROG_INSTALLED_FULL_FMT,

    /* Sentinel - keep last. */
    STR__COUNT,
} lang_id_t;

/* ---- Public API ---------------------------------------------------- */
void   i18n_init        (void);
lang_t i18n_get_language(void);
void   i18n_set_language(lang_t lang);

/* New unified lookup: STR_* enum -> active-language string.  Never
 * returns NULL; missing entries fall back to the EN column, and a
 * missing EN entry falls back to "?<id>?" so the gap is visible. */
const char *lang_get    (lang_id_t id);

/* Short alias for inline use: L(STR_BTN_OK). */
#define L(id)           lang_get(id)

/* Listener registration.  Every callback fires exactly once when the
 * active language changes.  Maximum 16 listeners. */
typedef void (*lang_listener_t)(lang_t new_lang);
void   lang_register_cb (lang_listener_t cb);

/* Legacy key-string API.  Kept for subsystems mid-migration; new code
 * should use lang_get(STR_*).  Both routes hit the same backing table. */
const char *i18n        (const char *key);
const char *i18n_or     (const char *key, const char *fallback);

#endif /* NEXXON_I18N_H */
