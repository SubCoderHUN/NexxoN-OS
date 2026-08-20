/* ============================================================================
 * NexxoN OS - Global localisation engine v2.0
 * ----------------------------------------------------------------------------
 * Two parallel arrays indexed by STR_* (one per language).  lang_get(id)
 * picks the active-language column with a single load.  Hungarian
 * translations are sized to fit inside the same UI widgets as the
 * English originals; longer-than-budget text is clipped at the render
 * layer via gfx_draw_string_clipped (Start Menu, tabs, button labels).
 *
 * Language change:
 *
 *   1. i18n_set_language() updates g_lang.
 *   2. The keyboard layout is re-bound to match (HU ↔ QWERTZ, EN ↔ QWERTY).
 *   3. Every registered lang_listener_t callback is fired exactly once.
 *   4. wm_force_global_repaint() invalidates every window so each one's
 *      next compose pass re-queries the new strings + recomputes its
 *      widget bounds with the new text widths.
 *
 * The legacy key-string i18n() lookup is reimplemented on top of the
 * enum table by carrying both representations side-by-side, so existing
 * modules that haven't been migrated keep working with zero changes.
 * ============================================================================ */
#include "i18n.h"
#include "string.h"
#include "debug.h"
#include "keyboard.h"
#include "window.h"

/* ---- EN column ------------------------------------------------------- */
static const char *const EN[STR__COUNT] = {
    /* App titles */
    [STR_APP_SHELL]        = "NexxoN Shell",
    [STR_APP_SETTINGS]     = "OS Settings",
    [STR_APP_TASKMGR]      = "Task Manager",
    [STR_APP_USERMGR]      = "User Manager",
    [STR_APP_EXPLORER]     = "NexxoN Explorer",
    [STR_APP_BROWSER]      = "NexxoN Browser",
    [STR_APP_EDITOR]       = "NexxoN Edit",
    [STR_APP_TRASH]        = "Recycle Bin",
    [STR_APP_DEVMGR]       = "Device Manager",
    [STR_APP_PONG]         = "Pong",
    [STR_APP_SCREENSAVER]  = "Screensaver",
    [STR_APP_NEXSHEET]     = "NexSheet",

    /* Generic UI verbs */
    [STR_BTN_OK]           = "OK",
    [STR_BTN_CANCEL]       = "Cancel",
    [STR_BTN_YES]          = "Yes",
    [STR_BTN_NO]           = "No",
    [STR_BTN_SAVE]         = "Save",
    [STR_BTN_LOAD]         = "Load",
    [STR_BTN_CLOSE]        = "Close",
    [STR_BTN_MINIMIZE]     = "Minimize",
    [STR_BTN_RESTORE]      = "Restore",
    [STR_BTN_DELETE]       = "Delete",
    [STR_BTN_RENAME]       = "Rename",
    [STR_BTN_OPEN]         = "Open",
    [STR_BTN_NEW]          = "New",
    [STR_BTN_EDIT]         = "Edit",
    [STR_BTN_PROPERTIES]   = "Properties",
    [STR_BTN_EMPTY_TRASH]  = "Empty Recycle Bin",
    [STR_BTN_KILL_PROCESS] = "End task",
    [STR_BTN_REFRESH]      = "Refresh",
    [STR_BTN_BROWSE]       = "Browse...",
    [STR_BTN_APPLY]        = "Apply",
    [STR_BTN_RESET]        = "Reset",
    [STR_BTN_CYCLE]        = "Cycle",
    [STR_BTN_TEST]         = "Test",
    [STR_BTN_VOLUME_UP]    = "Volume +",
    [STR_BTN_VOLUME_DOWN]  = "Volume -",
    [STR_BTN_MUTE]         = "Mute",
    [STR_BTN_UNMUTE]       = "Unmute",

    /* Login */
    [STR_LOGIN_WELCOME]    = "Welcome to NexxoN OS",
    [STR_LOGIN_USERNAME]   = "Username:",
    [STR_LOGIN_PASSWORD]   = "Password:",
    [STR_LOGIN_SIGNIN]     = "Sign in",
    [STR_LOGIN_BAD]        = "Invalid credentials",
    [STR_LOGIN_LOCKED]     = "Account locked",

    /* Start menu */
    [STR_STARTMENU_SHUTDOWN]      = "Shut down",
    [STR_STARTMENU_RESTART]       = "Restart",
    [STR_STARTMENU_SLEEP]         = "Sleep",
    [STR_STARTMENU_LOCK]          = "Lock",
    [STR_STARTMENU_LOGOFF]        = "Log off",
    [STR_STARTMENU_SETTINGS]      = "Settings",
    [STR_STARTMENU_USERS]         = "User Manager",
    [STR_STARTMENU_TASKMGR]       = "Task Manager",
    [STR_STARTMENU_DEVMGR]        = "Device Manager",
    [STR_STARTMENU_BROWSER]       = "Browser",
    [STR_STARTMENU_EXPLORER]      = "Explorer",
    [STR_STARTMENU_EDITOR]        = "Text Editor",
    [STR_STARTMENU_NEXSHEET]      = "NexSheet",
    [STR_STARTMENU_DOOM]          = "DOOM",
    [STR_STARTMENU_PROGRAMS]      = "Programs",
    [STR_APP_PROGRAMS]            = "Programs",
    [STR_PROG_HDR]                = "Installed programs",
    [STR_PROG_RUN]                = "Run",
    [STR_PROG_REMOVE]             = "Remove",
    [STR_PROG_INSTALL_DEMO]       = "Install demo",
    [STR_PROG_REFRESH]            = "Refresh",
    [STR_PROG_EMPTY]              = "No programs installed. Use 'Install demo' or put an ELF in /programs.",
    [STR_PROG_COUNT_FMT]          = "%d program(s) in /programs",
    [STR_STARTMENU_TRASH]         = "Recycle Bin",
    [STR_STARTMENU_SHELL]         = "Shell",
    [STR_STARTMENU_SEARCH_HINT]   = "Type to search...",
    [STR_STARTMENU_ALL_APPS]      = "All apps",
    [STR_STARTMENU_RECENT]        = "Recent",

    /* Taskbar */
    [STR_TASKBAR_START]    = "Start",
    [STR_TASKBAR_VOLUME]   = "Volume",
    [STR_TASKBAR_CLOCK]    = "Clock",
    [STR_TASKBAR_NETWORK]  = "Network",
    [STR_TASKBAR_BATTERY]  = "Battery",
    [STR_TASKBAR_CLOSE]    = "Close",

    /* Gephaz tabs */
    [STR_SETTINGS_TITLE]      = "OS Settings",
    [STR_SETTINGS_TAB_PERF]   = "Performance",
    [STR_SETTINGS_TAB_NET]    = "Network",
    [STR_SETTINGS_TAB_THEME]  = "Language",
    [STR_SETTINGS_TAB_STORE]  = "Storage",
    [STR_SETTINGS_TAB_AUDIO]  = "Audio",
    [STR_SETTINGS_TAB_SECURITY] = "Security",
    [STR_SETTINGS_TAB_LANG]   = "Language",
    [STR_SETTINGS_TAB_USERS]  = "Users",

    /* Gephaz: Performance */
    [STR_SET_PERF_HDR]     = "Resources (live monitor)",
    [STR_SET_PERF_CPU]     = "CPU",
    [STR_SET_PERF_RAM]     = "RAM (FB pool)",
    [STR_SET_PERF_UPTIME]  = "Uptime",
    [STR_SET_PERF_FBPOOL]  = "FB Pool",

    /* Gephaz: Network */
    [STR_SET_NET_HDR]      = "Network (Advanced)",
    [STR_SET_NET_MAC]      = "Link MAC",
    [STR_SET_NET_IPV4]     = "IPv4",
    [STR_SET_NET_NETMASK]  = "Netmask",
    [STR_SET_NET_GATEWAY]  = "Gateway",
    [STR_SET_NET_DNS]      = "DNS (1)",
    [STR_SET_NET_MODE]     = "Mode",
    [STR_SET_NET_DHCP]     = "DHCP",
    [STR_SET_NET_STATIC]   = "STATIC",
    [STR_SET_NET_BTN_DHCP_ON]  = "Switch to DHCP",
    [STR_SET_NET_BTN_DHCP_OFF] = "Switch to STATIC",
    [STR_SET_NET_BTN_RENEW]    = "Renew lease",
    [STR_SET_NET_BTN_SET_IP]   = "Set static IP...",
    [STR_SET_NET_BTN_SET_GW]   = "Set gateway...",
    [STR_SET_NET_BTN_SET_DNS]  = "Set primary DNS...",
    [STR_SET_NET_BTN_SET_DNS2] = "Set secondary DNS...",

    /* Gephaz: Themes */
    [STR_SET_THEME_HDR]        = "Display (Themes & Input)",
    [STR_SET_THEME_WALLPAPER]  = "Wallpaper",
    [STR_SET_THEME_ACCENT]     = "Theme accent",
    [STR_SET_THEME_NEXT_COLOR] = "Next theme color",
    [STR_SET_THEME_RELOAD_WP]  = "Reload wallpaper.bmp",
    [STR_SET_THEME_MOUSE]      = "Mouse sensitivity",
    [STR_SET_THEME_KEYBOARD]   = "Keyboard repeat",
    [STR_SET_THEME_RATE_SLOW]  = "Slow",
    [STR_SET_THEME_RATE_NORM]  = "Normal",
    [STR_SET_THEME_RATE_FAST]  = "Fast",
    [STR_SET_THEME_APPEARANCE] = "Appearance",
    [STR_SET_THEME_LIGHT]      = "Light",
    [STR_SET_THEME_DARK]       = "Dark",

    /* Gephaz: Storage */
    [STR_SET_STORE_HDR]    = "Storage",
    [STR_SET_STORE_INODES] = "NXFS inodes",
    [STR_SET_STORE_BLOCKS] = "NXFS blocks",
    [STR_SET_STORE_SATA]   = "SATA disk",
    [STR_SET_STORE_TIP]    = "Tip: /usb0 USB Mass-Storage volumes appear here when mounted.",

    /* Gephaz: Audio */
    [STR_SET_AUDIO_HDR]    = "Audio",
    [STR_SET_AUDIO_VOLUME] = "Master volume",
    [STR_SET_AUDIO_MUTED]  = "(muted)",

    /* Gephaz: Security */
    [STR_SET_SEC_HDR]         = "Security",
    [STR_SET_SEC_USER]        = "Current user",
    [STR_SET_SEC_ROLE]        = "Role",
    [STR_SET_SEC_TOTAL]       = "Total users",
    [STR_SET_SEC_ROLE_ADMIN]  = "admin",
    [STR_SET_SEC_ROLE_USER]   = "user",
    [STR_SET_SEC_OPEN_USERS]  = "Open User Manager...",
    [STR_SET_SEC_CHANGE_PW]   = "Change my password...",
    [STR_SET_SEC_LANG]        = "Language",
    [STR_SET_SEC_SWITCH_EN]   = "Switch to EN",
    [STR_SET_SEC_SWITCH_HU]   = "Switch to HU",
    [STR_GH_MOUSE_SEC]        = "Mouse",
    [STR_GH_MOUSE_SENS]       = "Pointer sensitivity",
    [STR_GH_SLOWER]           = "Slow",
    [STR_GH_FASTER]           = "Fast",

    /* Task Manager */
    [STR_TM_TAB_APPS]     = "Applications",
    [STR_TM_TAB_PROCS]    = "Processes",
    [STR_TM_TAB_SVC]      = "Services",
    [STR_TM_TAB_PERF]     = "Performance",
    [STR_TM_TAB_NET]      = "Networking",
    [STR_TM_TAB_USERS]    = "Users",
    [STR_TM_MENU_FILE]    = "File",
    [STR_TM_MENU_OPTIONS] = "Options",
    [STR_TM_MENU_VIEW]    = "View",
    [STR_TM_MENU_HELP]    = "Help",
    [STR_TM_COL_NAME]     = "Image name",
    [STR_TM_COL_USER]     = "User name",
    [STR_TM_COL_CPU]      = "CPU %",
    [STR_TM_COL_MEM]      = "Memory",
    [STR_TM_COL_DESC]     = "Description",
    [STR_TM_COL_PID]      = "PID",
    [STR_TM_COL_STATE]    = "State",
    [STR_TM_STATE_RUN]    = "Running",
    [STR_TM_STATE_HUNG]   = "Not responding",
    [STR_TM_STATE_STOPPED]= "Stopped",
    [STR_TM_KILL_PROMPT]  = "End the selected task?",
    [STR_TM_NEW_TASK]     = "New task...",

    /* Explorer */
    [STR_XP_PARENT]         = "Parent",
    [STR_XP_FOLDER]         = "folder",
    [STR_XP_FILE]           = "file",
    [STR_XP_NITEMS]         = "items",
    [STR_XP_SELECTED]       = "Selected:",
    [STR_XP_CTX_OPEN]       = "Open",
    [STR_XP_CTX_DELETE]     = "Delete",
    [STR_XP_CTX_RENAME]     = "Rename",
    [STR_XP_CTX_PROPERTIES] = "Properties",
    [STR_XP_CTX_NEW_FOLDER] = "New folder",
    [STR_XP_CTX_NEW_FILE]   = "New file",
    [STR_XP_SIDEBAR_HOME]   = "Home",
    [STR_XP_SIDEBAR_ROOT]   = "Root",
    [STR_XP_SIDEBAR_SYS]    = "System",
    [STR_XP_SIDEBAR_USB]    = "USB",
    [STR_XP_ADDR_LABEL]     = "Path:",

    /* Recycle Bin */
    [STR_TRASH_HDR]      = "Recycle Bin",
    [STR_TRASH_EMPTY]    = "Recycle Bin is empty",
    [STR_TRASH_RESTORE]  = "Restore",
    [STR_TRASH_PURGE]    = "Empty",
    [STR_TRASH_NITEMS]   = "items in bin",

    /* Editor */
    [STR_EDIT_NEW]            = "New",
    [STR_EDIT_OPEN]           = "Open...",
    [STR_EDIT_SAVE]           = "Save",
    [STR_EDIT_SAVED]          = "Saved",
    [STR_EDIT_MODIFIED]       = "Modified",
    [STR_EDIT_FIND]           = "Find",
    [STR_EDIT_FIND_PROMPT]    = "Find: type the search term and press Enter",
    [STR_EDIT_CUT]            = "Cut line",
    [STR_EDIT_COPY]           = "Copy",
    [STR_EDIT_PASTE]          = "Pasted",
    [STR_EDIT_CLOSE_CONFIRM]  = "Close without saving?",
    [STR_EDIT_LINE_NUM]       = "Line",

    /* Browser */
    [STR_BR_BACK]      = "Back",
    [STR_BR_FORWARD]   = "Forward",
    [STR_BR_RELOAD]    = "Reload",
    [STR_BR_HOME]      = "Home",
    [STR_BR_STOP]      = "Stop",
    [STR_BR_LOADING]   = "Loading...",
    [STR_BR_ADDR_HINT] = "Enter URL...",
    [STR_BR_ERR_DNS]   = "Host is not a numeric IPv4 (no DNS yet)",
    [STR_BR_ERR_TCP]   = "TCP connect failed",
    [STR_BR_IDLE]            = "Idle",
    [STR_BR_NAVIGATING]      = "Navigating to %s",
    [STR_BR_BACK_TO]         = "Back to %s",
    [STR_BR_REFRESHING]      = "Refreshing %s",
    [STR_BR_INVALID_IP]      = "Invalid IP address",
    [STR_BR_CONNECTING]      = "Connecting to %s:%d ...",
    [STR_BR_CONNECTED]       = "Connected to proxy",
    [STR_BR_CONN_LOST]       = "Connection lost",
    [STR_BR_JPEG_TRUNC]      = "JPEG truncated (%d bytes)",
    [STR_BR_JPEG_FAIL]       = "JPEG decode failed (%d bytes)",
    [STR_BR_BAD_JPEG]        = "Bad JPEG SOI (got %02x %02x)",
    [STR_BR_INCOMPLETE_JPEG] = "Incomplete JPEG (missing EOI)",
    [STR_BR_CONNECTED_SINCE] = "Connected (%ums since frame)",
    [STR_BR_DISCONNECTED]    = "Disconnected - retrying...",
    [STR_BR_PROXY_CHANGED]   = "Proxy changed to %s - reconnecting...",
    [STR_BR_CONN_FAILED]     = "Proxy connection failed (%s:%d)",
    [STR_BR_CONN_TIMEOUT]    = "Connection timed out (%s:%d)",
    [STR_BR_READY]           = "Ready.  Edit the URL above and press Enter (or Go).",
    [STR_BR_NO_FRAMES]       = "No frames from proxy - reconnecting...",
    [STR_BR_PROXY_SETTINGS]  = "Proxy Settings",
    [STR_BR_PROXY_IP_PROMPT] = "Proxy IP address:",
    [STR_BR_NEED_NIC1]       = "NexxoN Browser requires a network adapter.",
    [STR_BR_NEED_NIC2]       = "No E1000 (8086:100e) was discovered on the PCI bus.",
    [STR_BR_GO]              = "Go",

    /* Dialogs */
    [STR_DLG_INPUT_HINT]      = "Enter value:",
    [STR_DLG_CONFIRM_TITLE]   = "Confirm",
    [STR_DLG_ERROR_TITLE]     = "Error",
    [STR_DLG_INFO_TITLE]      = "Notice",
    [STR_DLG_UNSAVED_TITLE]   = "Unsaved changes",
    [STR_DLG_UNSAVED_BODY]    = "The file has been modified.",
    [STR_DLG_UNSAVED_QUESTION]= "Close without saving?",

    /* Status messages */
    [STR_MSG_READY]             = "Ready",
    [STR_MSG_LOADING]           = "Loading...",
    [STR_MSG_DONE]              = "Done",
    [STR_MSG_FAILED]            = "Failed",
    [STR_MSG_NO_NETWORK]        = "No network",
    [STR_MSG_NO_DEVICE]         = "No device",
    [STR_MSG_INSUFFICIENT_PERM] = "Insufficient permissions",
    [STR_MSG_INVALID_INPUT]     = "Invalid input",
    [STR_MSG_OPERATION_OK]      = "Operation completed",

    /* Boot banners */
    [STR_BOOT_WELCOME]         = "Welcome to NexxoN OS",
    [STR_BOOT_LOADING]         = "Loading...",
    [STR_BOOT_READY]           = "System ready",
    [STR_BOOT_LIVE_MODE]       = "Running in LIVE MODE (RAM only)",
    [STR_BOOT_INSTALLED_MODE]  = "Booting from installed NexxoN HDD",

    /* RSOD */
    [STR_PANIC_TITLE]          = "NexxoN OS  -  KERNEL PANIC  (RSOD)",
    [STR_PANIC_CAUSE_CPU]      = "Cause: CPU EXCEPTION (unhandled)",
    [STR_PANIC_CAUSE_KERNEL]   = "Cause: KERNEL PANIC",
    [STR_PANIC_HALTED]         = "System halted. Please reboot.",
    [STR_PANIC_RECOVERABLE]    = "NexxoN - Recoverable Error",
    [STR_PANIC_RECOVERED]      = "Shell recovered from a faulted subsystem.",

    /* Shell help */
    [STR_SHELL_PROMPT_USER]    = "user",
    [STR_SHELL_HELP_HEADER]    = "NexxoN Shell - command reference",
    [STR_SHELL_HELP_MORE]      = "-- press Space for more, q to quit --",
    [STR_SHELL_UNKNOWN_CMD]    = "unknown command",
    [STR_SHELL_CWD]            = "cwd",
    [STR_SHELL_HELP_FS]        = "File system",
    [STR_SHELL_HELP_NET]       = "Network",
    [STR_SHELL_HELP_SYS]       = "System",
    [STR_SHELL_HELP_APPS]      = "Applications",

    /* NexxStore */
    [STR_APP_NEXSTORE]         = "NexxStore",
    [STR_STARTMENU_NEXSTORE]   = "NexxStore",
    [STR_NXST_SEARCH]          = "Search packages...",
    [STR_NXST_INSTALL]         = "Install",
    [STR_NXST_UNINSTALL]       = "Uninstall",
    [STR_NXST_INSTALLED]       = "Installed",
    [STR_NXST_AVAILABLE]       = "Available",
    [STR_NXST_NO_NET]          = "No network connection",
    [STR_NXST_EMPTY]           = "No packages found",
    [STR_NXST_CONFIRM_INST]    = "Install this package?",
    [STR_NXST_CONFIRM_UNINST]  = "Uninstall this package?",
    [STR_NXST_SUCCESS]         = "Operation completed",

    /* Explorer USB */
    [STR_XP_SIDEBAR_DRIVES]    = "Drives",
    [STR_XP_USB_EJECT]         = "Eject",
    [STR_XP_USB_READONLY]      = "Read-only",
    [STR_XP_USB_COPIED]        = "Copied to USB",

    /* Audio Player */
    [STR_APP_AUDIOPLAYER]      = "NexxoN Music",
    [STR_STARTMENU_AUDIOPLAYER]= "Music Player",
    [STR_AP_PLAY]              = "Play",
    [STR_AP_PAUSE]             = "Pause",
    [STR_AP_STOP]              = "Stop",
    [STR_AP_OPEN]              = "Open File",
    [STR_AP_NO_FILE]           = "No file loaded",
    [STR_AP_PLAYING]           = "Playing",
    [STR_AP_PAUSED]            = "Paused",
    [STR_AP_STOPPED]           = "Stopped",
    [STR_AP_VOLUME]            = "Volume",
    [STR_AP_DURATION]          = "Duration",

    /* Power Management */
    [STR_PM_SUSPEND]           = "Suspend",
    [STR_PM_SUSPEND_CONFIRM]   = "Suspend system to RAM?",
    [STR_PM_CPUFREQ]           = "CPU Frequency",
    [STR_PM_FREQ_LOW]          = "Power Save",
    [STR_PM_FREQ_HIGH]         = "Performance",
    [STR_PM_FREQ_LABEL]        = "CPU Speed",

    /* Installer */
    [STR_APP_INSTALLER]        = "NexxoN Installer",
    [STR_STARTMENU_INSTALLER]  = "Install NexxoN",
    [STR_INST_TITLE]           = "NexxoN OS Setup",
    [STR_INST_WELCOME]         = "Welcome to the NexxoN OS installer. This wizard will guide you through installing NexxoN OS to your hard drive.",
    [STR_INST_SELECT_DISK]     = "Select installation target disk:",
    [STR_INST_CONFIRM]         = "WARNING: All data on the selected disk will be erased. Continue?",
    [STR_INST_PROGRESS]        = "Installing NexxoN OS...",
    [STR_INST_DONE]            = "Installation complete! You can now reboot.",
    [STR_INST_FAILED]          = "Installation failed. Check disk connection.",
    [STR_INST_NO_DISK]         = "No suitable disk found.",
    [STR_INST_ALREADY]         = "Already installed on this disk.",
    [STR_INST_BACK]            = "Back",
    [STR_INST_NEXT]            = "Next",
    [STR_INST_INSTALL]         = "Install Now",
    [STR_INST_STEP_WELCOME]    = "Welcome",
    [STR_INST_STEP_DISK]       = "Disk",
    [STR_INST_STEP_CONFIRM]    = "Confirm",
    [STR_INST_STEP_INSTALL]    = "Install",
    [STR_INST_STEP_DONE]       = "Done",

    /* Notifications */
    [STR_NOTIFY_DISMISS]       = "Dismiss",

    /* Shell */
    [STR_SHELL_PIPE_ERR]       = "pipe: command failed",
    [STR_SHELL_REDIR_ERR]      = "redirect: file error",

    /* Multi-monitor */
    [STR_STARTMENU_DISPLAYS]   = "Display Manager",

    /* Wi-Fi */
    [STR_STARTMENU_WIFI]       = "Wi-Fi Networks",

    /* Bluetooth */
    [STR_APP_BLUETOOTH]        = "Bluetooth",
    [STR_STARTMENU_BLUETOOTH]  = "Bluetooth Devices",
    [STR_BT_TITLE]             = "Bluetooth Devices",
    [STR_BT_NO_HW]             = "No Bluetooth hardware",
    [STR_BT_SCAN_PROMPT]       = "Press Scan to search",
    [STR_BT_PAIRED]            = "Paired",
    [STR_BT_BTN_SCAN]          = "Scan",
    [STR_BT_BTN_PAIR]          = "Pair",
    [STR_BT_CLS_PHONE]         = "[Phone]",
    [STR_BT_CLS_AUDIO]         = "[Audio]",
    [STR_BT_CLS_KEYS]          = "[Keys] ",
    [STR_BT_CLS_MOUSE]         = "[Mouse]",
    [STR_BT_CLS_PC]            = "[PC]   ",
    [STR_BT_CLS_UNK]           = "[?]    ",
    [STR_BT_NOTIFY_TITLE]      = "Bluetooth",
    [STR_BT_NOTIFY_SCAN_OK]    = "Scan complete",
    [STR_BT_NOTIFY_PAIRED]     = "Paired with ",
    [STR_BT_NOTIFY_FAIL]       = "Pairing failed",

    /* Display / Resolution */
    [STR_SETTINGS_TAB_DISPLAY] = "Screen",
    [STR_SETTINGS_TAB_MOUSE]   = "Mouse & Kbd",
    [STR_DISP_TITLE]           = "Screen Settings",
    [STR_DISP_RESOLUTION]      = "Resolution:",
    [STR_DISP_CURRENT]         = "Current:",
    [STR_DISP_APPLY]           = "Apply",
    [STR_DISP_NO_MODES]        = "No video modes available",
    [STR_DISP_KEEP_TITLE]      = "Keep display settings?",
    [STR_DISP_KEEP_MSG]        = "Reverting in %d seconds...",
    [STR_DISP_KEEP_BTN]        = "Keep changes",
    [STR_DISP_REVERT_BTN]      = "Revert",
    [STR_DISP_SWITCH_OK]       = "Resolution changed",
    [STR_DISP_SWITCH_FAIL]     = "Mode switch failed",
    [STR_DISP_REVERTED]        = "Resolution reverted",

    /* ---- Image Viewer ------------------------------------------------- */
    [STR_APP_IMGVIEW]          = "NexxoN Image Viewer",
    [STR_STARTMENU_IMGVIEW]    = "Image Viewer",
    [STR_IV_OPEN]              = "Open Image",
    [STR_IV_ZOOM_IN]           = "Zoom +",
    [STR_IV_ZOOM_OUT]          = "Zoom -",
    [STR_IV_FIT]               = "Fit",
    [STR_IV_ACTUAL]            = "1:1",
    [STR_IV_NO_FILE]           = "No image loaded. Click Open to browse.",
    [STR_IV_LOAD_ERR]          = "Cannot load image file.",
    [STR_IV_FORMAT_ERR]        = "Unsupported image format.",

    /* ---- Wi-Fi manager ----------------------------------------------- */
    [STR_WIFI_NO_HW]           = "No wireless hardware detected",
    [STR_WIFI_CONNECTED]       = "Connected",
    [STR_WIFI_NETS_FOUND]      = "network(s) found",
    [STR_WIFI_SCAN_HINT]       = "Press Scan to search",
    [STR_WIFI_PASSWORD]        = "Password:",
    [STR_WIFI_BTN_SCAN]        = "Scan",
    [STR_WIFI_BTN_CONNECT]     = "Connect",
    [STR_WIFI_BTN_DISCONNECT]  = "Disconnect",
    [STR_WIFI_SCAN_DONE]       = "Scan complete",
    [STR_WIFI_CONNECT_OK]      = "Connected to",
    [STR_WIFI_CONNECT_FAIL]    = "Connection failed",
    [STR_WIFI_DISCONNECTED]    = "Disconnected",
    [STR_WIFI_SEC_OPEN]        = "Open",

    /* ---- Display / Screen manager ------------------------------------ */
    [STR_DISP_MON_DETECTED]    = "monitor(s) detected",
    [STR_DISP_PRIMARY]         = "[Primary]",
    [STR_DISP_MODES]           = "modes",
    [STR_DISP_NOTIFY_MSG]      = "Mode change requested (takes effect next boot)",
    [STR_DISP_STATUS]          = "Manage connected displays and resolutions",

    /* ---- Screensaver -------------------------------------------------- */
    [STR_SCREENSAVER_HINT]     = "(any key or mouse movement returns to the desktop)",

    /* ---- About NexxoN OS ---------------------------------------------- */
    [STR_ABOUT_TITLE]          = "About NexxoN OS",
    [STR_ABOUT_TAGLINE]        = "x86_64 (64-bit) operating system",
    [STR_ABOUT_BUILD]          = "Build",
    [STR_ABOUT_CPUS]           = "Processors",
    [STR_ABOUT_MEMORY]         = "Memory",
    [STR_ABOUT_DISPLAY]        = "Display",
    [STR_ABOUT_FEATURES]       = "SMP, USB HID + storage, FAT/exFAT, AHCI/IDE, TCP/IP, NXFS",
    [STR_ABOUT_HINT]           = "Right-click the desktop or an icon for more actions.",

    /* USB removable storage + Explorer VFS operations */
    [STR_USB_TITLE]            = "Removable storage",
    [STR_USB_MOUNTED]          = "USB drive connected: %s",
    [STR_USB_REMOVED]          = "USB drive removed: %s",
    [STR_XP_CTX_COPY]          = "Copy",
    [STR_XP_CTX_PASTE]         = "Paste",
    [STR_XP_CTX_REFRESH]       = "Refresh",
    [STR_XP_NEWDIR_PROMPT]     = "Folder name:",
    [STR_XP_RENAME_PROMPT]     = "New name:",
    [STR_XP_DELETE_TITLE]      = "Confirm delete",
    [STR_XP_DELETE_Q]          = "Delete '%s' ?",
    [STR_XP_DELETE_RECURSIVE]  = "WARNING: removes ALL contents.",
    [STR_XP_OP_FAILED]         = "Operation failed",
    [STR_XP_NAME_TAKEN]        = "That name already exists.",
    [STR_XP_BAD_SHORTNAME]     = "Name not usable on this volume (8.3 only).",
    [STR_XP_FILE_TOO_BIG]      = "File too large for the copy buffer.",
    [STR_XP_COPIED]            = "Copied: %s",
    [STR_XP_PASTE_DONE]        = "Pasted: %s",
    [STR_XP_READ_ONLY]         = "This volume is read-only.",
    [STR_EDIT_SAVED_FMT]       = "Saved %u bytes.",
    [STR_EDIT_SAVE_FAIL_FMT]   = "Save FAILED (code=%d).",

    /* Browser proxy auto-discovery */
    [STR_BR_PROXY_TITLE]       = "NexxoN Browser",
    [STR_BR_PROXY_FOUND]       = "Proxy discovered: %s",

    /* Linux compatibility runtime + Programs status */
    [STR_LINUX_ALREADY_RUNNING]  = "A program is already running.",
    [STR_LINUX_LOAD_FAILED]      = "The Linux program could not be loaded.",
    [STR_LINUX_ENTRY_UNSUPPORTED]= "This program needs an isolated address space.",
    [STR_LINUX_STACK_FAILED]     = "The program stack could not be prepared.",
    [STR_LINUX_TERMINATED_FMT]   = "Program terminated safely (code %d).",
    [STR_LINUX_EXITED_FMT]       = "Program exited with code %d.",
    [STR_LINUX_NOT_FOUND_FMT]    = "Program not found: %s",
    [STR_LINUX_READ_FAILED]      = "The program file could not be read.",
    [STR_LINUX_IMAGE_TOO_LARGE]  = "The program file is too large.",
    [STR_LINUX_INTERP_FAILED]    = "The dynamic loader or libc could not be loaded.",
    [STR_LINUX_LOADING_FMT]      = "linux: loading %s ...\n",
    [STR_LINUX_RUNNING_DEMO_FMT] = "linux: running static-musl demo (%u bytes) ...\n",
    [STR_LINUX_RUNNING_DYN_FMT]  = "linux: running dynamic-musl probe (%u bytes) ...\n",
    [STR_LINUX_SUBSYSTEM_ENTER]  = "linux: entering interactive subsystem (/bin/sh) ...\n",
    [STR_LINUX_RESULT_FMT]       = "linux: %s\n",
    [STR_LINUX_OK]               = "completed",
    [STR_LINUX_FAILED]           = "failed",
    [STR_PROG_DIR_FAILED]        = "The /programs folder is unavailable.",
    [STR_PROG_CREATE_FAILED]     = "The program file could not be created.",
    [STR_PROG_INSTALLED_FMT]     = "Installed linuxdemo (%u B).",
    [STR_PROG_WRITE_FAILED]      = "The program could not be installed.",
    [STR_PROG_RESULT_FMT]        = "%s: %s",
    [STR_PROG_REMOVED_FMT]       = "Removed %s.",
    [STR_PROG_REMOVE_FAILED]     = "The program could not be removed.",
    [STR_PROG_SIZE_BYTES_FMT]    = "%u B",
    [STR_PROG_INSTALLED_TESTS_FMT]= "Installed linuxdemo and linuxexec (%u B).",
    [STR_PROG_INSTALLED_FULL_FMT] = "Installed Linux test programs and /lib (%u B).",
};

/* ---- HU column ------------------------------------------------------- *
 * NOTE: Source-level UTF-8 strings.  GCC stores them verbatim in the
 * binary and gfx_draw_string handles UTF-8 decoding -> 8x8 glyph lookup
 * via the ISO-8859-2 slots (0xC1..0xFC) populated in font.c.  Hungarian
 * accented characters render with pixel-perfect accuracy throughout the
 * entire GUI without source-level transliteration. */
static const char *const HU[STR__COUNT] = {
    [STR_APP_SHELL]        = "NexxoN Parancssor",
    [STR_APP_SETTINGS]     = "Gépház",
    [STR_APP_TASKMGR]      = "Feladatkezelő",
    [STR_APP_USERMGR]      = "Felhasználók",
    [STR_APP_EXPLORER]     = "NexxoN Intéző",
    [STR_APP_BROWSER]      = "NexxoN Böngésző",
    [STR_APP_EDITOR]       = "NexxoN Szerkesztő",
    [STR_APP_TRASH]        = "Lomtár",
    [STR_APP_DEVMGR]       = "Eszközkezelő",
    [STR_APP_PONG]         = "Pong",
    [STR_APP_SCREENSAVER]  = "Képernyővédő",
    [STR_APP_NEXSHEET]     = "NexSheet",

    [STR_BTN_OK]           = "OK",
    [STR_BTN_CANCEL]       = "Mégse",
    [STR_BTN_YES]          = "Igen",
    [STR_BTN_NO]           = "Nem",
    [STR_BTN_SAVE]         = "Mentés",
    [STR_BTN_LOAD]         = "Betöltés",
    [STR_BTN_CLOSE]        = "Bezárás",
    [STR_BTN_MINIMIZE]     = "Kicsinyít",
    [STR_BTN_RESTORE]      = "Visszaáll.",
    [STR_BTN_DELETE]       = "Törlés",
    [STR_BTN_RENAME]       = "Átnevezés",
    [STR_BTN_OPEN]         = "Megnyit",
    [STR_BTN_NEW]          = "Új",
    [STR_BTN_EDIT]         = "Szerk.",
    [STR_BTN_PROPERTIES]   = "Tulajdons.",
    [STR_BTN_EMPTY_TRASH]  = "Lomtár ürítése",
    [STR_BTN_KILL_PROCESS] = "Folyamat le",
    [STR_BTN_REFRESH]      = "Frissítés",
    [STR_BTN_BROWSE]       = "Tallóz...",
    [STR_BTN_APPLY]        = "Alkalmaz",
    [STR_BTN_RESET]        = "Visszaáll",
    [STR_BTN_CYCLE]        = "Vált",
    [STR_BTN_TEST]         = "Teszt",
    [STR_BTN_VOLUME_UP]    = "Hang +",
    [STR_BTN_VOLUME_DOWN]  = "Hang -",
    [STR_BTN_MUTE]         = "Némít",
    [STR_BTN_UNMUTE]       = "Visszahoz",

    [STR_LOGIN_WELCOME]    = "Üdvözli a NexxoN OS",
    [STR_LOGIN_USERNAME]   = "Felhasználó:",
    [STR_LOGIN_PASSWORD]   = "Jelszó:",
    [STR_LOGIN_SIGNIN]     = "Belépés",
    [STR_LOGIN_BAD]        = "Hibás adatok",
    [STR_LOGIN_LOCKED]     = "Fiók zárolva",

    [STR_STARTMENU_SHUTDOWN]      = "Leállítás",
    [STR_STARTMENU_RESTART]       = "Újraindít.",
    [STR_STARTMENU_SLEEP]         = "Alvás",
    [STR_STARTMENU_LOCK]          = "Zárolás",
    [STR_STARTMENU_LOGOFF]        = "Kijelentk.",
    [STR_STARTMENU_SETTINGS]      = "Beállítások",
    [STR_STARTMENU_USERS]         = "Felhasználók",
    [STR_STARTMENU_TASKMGR]       = "Feladatkez.",
    [STR_STARTMENU_DEVMGR]        = "Eszközkez.",
    [STR_STARTMENU_BROWSER]       = "Böngésző",
    [STR_STARTMENU_EXPLORER]      = "Intéző",
    [STR_STARTMENU_EDITOR]        = "Szerkesztő",
    [STR_STARTMENU_NEXSHEET]      = "NexSheet",
    [STR_STARTMENU_DOOM]          = "DOOM",
    [STR_STARTMENU_PROGRAMS]      = "Programok",
    [STR_APP_PROGRAMS]            = "Programok",
    [STR_PROG_HDR]                = "Telepített programok",
    [STR_PROG_RUN]                = "Futtatás",
    [STR_PROG_REMOVE]             = "Eltávolítás",
    [STR_PROG_INSTALL_DEMO]       = "Demó telepítése",
    [STR_PROG_REFRESH]            = "Frissítés",
    [STR_PROG_EMPTY]              = "Nincs telepített program. Használd a 'Demó telepítése' gombot, vagy tegyél ELF-et a /programs mappába.",
    [STR_PROG_COUNT_FMT]          = "%d program a /programs mappában",
    [STR_STARTMENU_TRASH]         = "Lomtár",
    [STR_STARTMENU_SHELL]         = "Parancssor",
    [STR_STARTMENU_SEARCH_HINT]   = "Keresési kifejezés...",
    [STR_STARTMENU_ALL_APPS]      = "Minden alkalm.",
    [STR_STARTMENU_RECENT]        = "Legutóbbi",

    [STR_TASKBAR_START]    = "Start",
    [STR_TASKBAR_VOLUME]   = "Hang",
    [STR_TASKBAR_CLOCK]    = "Óra",
    [STR_TASKBAR_NETWORK]  = "Hálózat",
    [STR_TASKBAR_BATTERY]  = "Akku",
    [STR_TASKBAR_CLOSE]    = "Bezár",

    [STR_SETTINGS_TITLE]        = "Gépház",
    [STR_SETTINGS_TAB_PERF]     = "Erőforrás",
    [STR_SETTINGS_TAB_NET]      = "Hálózat",
    [STR_SETTINGS_TAB_THEME]    = "Nyelv",
    [STR_SETTINGS_TAB_STORE]    = "Tárhely",
    [STR_SETTINGS_TAB_AUDIO]    = "Hang",
    [STR_SETTINGS_TAB_SECURITY] = "Biztonság",
    [STR_SETTINGS_TAB_LANG]     = "Nyelv",
    [STR_SETTINGS_TAB_USERS]    = "Felhaszn.",

    [STR_SET_PERF_HDR]     = "Erőforrások (figyelő)",
    [STR_SET_PERF_CPU]     = "CPU",
    [STR_SET_PERF_RAM]     = "RAM (FB)",
    [STR_SET_PERF_UPTIME]  = "Üzemidő",
    [STR_SET_PERF_FBPOOL]  = "FB Pool",

    [STR_SET_NET_HDR]      = "Hálózat (Részletes)",
    [STR_SET_NET_MAC]      = "MAC cím",
    [STR_SET_NET_IPV4]     = "IPv4",
    [STR_SET_NET_NETMASK]  = "Háló maszk",
    [STR_SET_NET_GATEWAY]  = "Átjáró",
    [STR_SET_NET_DNS]      = "DNS (1)",
    [STR_SET_NET_MODE]     = "Mód",
    [STR_SET_NET_DHCP]     = "DHCP",
    [STR_SET_NET_STATIC]   = "STATIKUS",
    [STR_SET_NET_BTN_DHCP_ON]  = "Vissza DHCP",
    [STR_SET_NET_BTN_DHCP_OFF] = "Vissza STATIC",
    [STR_SET_NET_BTN_RENEW]    = "Bérl. újítás",
    [STR_SET_NET_BTN_SET_IP]   = "Statikus IP...",
    [STR_SET_NET_BTN_SET_GW]   = "Átjáró beáll...",
    [STR_SET_NET_BTN_SET_DNS]  = "DNS 1 beáll...",
    [STR_SET_NET_BTN_SET_DNS2] = "DNS 2 beáll...",

    [STR_SET_THEME_HDR]        = "Megjelenés",
    [STR_SET_THEME_WALLPAPER]  = "Háttérkép",
    [STR_SET_THEME_ACCENT]     = "Téma szín",
    [STR_SET_THEME_NEXT_COLOR] = "Következő szín",
    [STR_SET_THEME_RELOAD_WP]  = "Háttér újraolv.",
    [STR_SET_THEME_MOUSE]      = "Egér érzékenység",
    [STR_SET_THEME_KEYBOARD]   = "Bill. ismétlés",
    [STR_SET_THEME_RATE_SLOW]  = "Lassú",
    [STR_SET_THEME_RATE_NORM]  = "Normál",
    [STR_SET_THEME_RATE_FAST]  = "Gyors",
    [STR_SET_THEME_APPEARANCE] = "Megjelenés",
    [STR_SET_THEME_LIGHT]      = "Világos",
    [STR_SET_THEME_DARK]       = "Sötét",

    [STR_SET_STORE_HDR]    = "Tárhely",
    [STR_SET_STORE_INODES] = "NXFS inode-ok",
    [STR_SET_STORE_BLOCKS] = "NXFS blokkok",
    [STR_SET_STORE_SATA]   = "SATA lemez",
    [STR_SET_STORE_TIP]    = "Tipp: /usb0 USB tárolók itt jelennek meg ha csatlakoztatva vannak.",

    [STR_SET_AUDIO_HDR]    = "Hang",
    [STR_SET_AUDIO_VOLUME] = "Fő hangerő",
    [STR_SET_AUDIO_MUTED]  = "(némítva)",

    [STR_SET_SEC_HDR]         = "Biztonság",
    [STR_SET_SEC_USER]        = "Jelenlegi",
    [STR_SET_SEC_ROLE]        = "Szerep",
    [STR_SET_SEC_TOTAL]       = "Összes felh.",
    [STR_SET_SEC_ROLE_ADMIN]  = "rendszergazda",
    [STR_SET_SEC_ROLE_USER]   = "felhasználó",
    [STR_SET_SEC_OPEN_USERS]  = "Felhasználókezelő...",
    [STR_SET_SEC_CHANGE_PW]   = "Jelszó csere...",
    [STR_SET_SEC_LANG]        = "Nyelv",
    [STR_SET_SEC_SWITCH_EN]   = "Vált: EN",
    [STR_SET_SEC_SWITCH_HU]   = "Vált: HU",
    [STR_GH_MOUSE_SEC]        = "Egér",
    [STR_GH_MOUSE_SENS]       = "Egér érzékenység",
    [STR_GH_SLOWER]           = "Lassú",
    [STR_GH_FASTER]           = "Gyors",

    [STR_TM_TAB_APPS]     = "Alkalmazások",
    [STR_TM_TAB_PROCS]    = "Folyamatok",
    [STR_TM_TAB_SVC]      = "Szolg.",
    [STR_TM_TAB_PERF]     = "Teljesítm.",
    [STR_TM_TAB_NET]      = "Hálózat",
    [STR_TM_TAB_USERS]    = "Felhaszn.",
    [STR_TM_MENU_FILE]    = "Fájl",
    [STR_TM_MENU_OPTIONS] = "Beáll.",
    [STR_TM_MENU_VIEW]    = "Nézet",
    [STR_TM_MENU_HELP]    = "Súgó",
    [STR_TM_COL_NAME]     = "Kód neve",
    [STR_TM_COL_USER]     = "Felhaszn.",
    [STR_TM_COL_CPU]      = "CPU %",
    [STR_TM_COL_MEM]      = "Memória",
    [STR_TM_COL_DESC]     = "Leírás",
    [STR_TM_COL_PID]      = "PID",
    [STR_TM_COL_STATE]    = "Állapot",
    [STR_TM_STATE_RUN]    = "Fut",
    [STR_TM_STATE_HUNG]   = "Nem válaszol",
    [STR_TM_STATE_STOPPED]= "Leállítva",
    [STR_TM_KILL_PROMPT]  = "Leállítja a feladatot?",
    [STR_TM_NEW_TASK]     = "Új feladat...",

    [STR_XP_PARENT]         = "Szülő",
    [STR_XP_FOLDER]         = "mappa",
    [STR_XP_FILE]           = "fájl",
    [STR_XP_NITEMS]         = "elem",
    [STR_XP_SELECTED]       = "Kijelölt:",
    [STR_XP_CTX_OPEN]       = "Megnyit",
    [STR_XP_CTX_DELETE]     = "Törlés",
    [STR_XP_CTX_RENAME]     = "Átnevezés",
    [STR_XP_CTX_PROPERTIES] = "Tulajdons.",
    [STR_XP_CTX_NEW_FOLDER] = "Új mappa",
    [STR_XP_CTX_NEW_FILE]   = "Új fájl",
    [STR_XP_SIDEBAR_HOME]   = "Saját",
    [STR_XP_SIDEBAR_ROOT]   = "Gyökér",
    [STR_XP_SIDEBAR_SYS]    = "Rendszer",
    [STR_XP_SIDEBAR_USB]    = "USB",
    [STR_XP_ADDR_LABEL]     = "Útvonal:",

    [STR_TRASH_HDR]      = "Lomtár",
    [STR_TRASH_EMPTY]    = "A lomtár üres",
    [STR_TRASH_RESTORE]  = "Visszaáll.",
    [STR_TRASH_PURGE]    = "Ürítés",
    [STR_TRASH_NITEMS]   = "elem a lomtárban",

    [STR_EDIT_NEW]            = "Új",
    [STR_EDIT_OPEN]           = "Megnyit...",
    [STR_EDIT_SAVE]           = "Mentés",
    [STR_EDIT_SAVED]          = "Mentve",
    [STR_EDIT_MODIFIED]       = "Módosítva",
    [STR_EDIT_FIND]           = "Keres",
    [STR_EDIT_FIND_PROMPT]    = "Keres: írd be a kifejezést és nyomj Entert",
    [STR_EDIT_CUT]            = "Sor kivágva",
    [STR_EDIT_COPY]           = "Másolás",
    [STR_EDIT_PASTE]          = "Beillesztve",
    [STR_EDIT_CLOSE_CONFIRM]  = "Bezárja mentés nélkül?",
    [STR_EDIT_LINE_NUM]       = "Sor",

    [STR_BR_BACK]      = "Vissza",
    [STR_BR_FORWARD]   = "Előre",
    [STR_BR_RELOAD]    = "Frissít",
    [STR_BR_HOME]      = "Kezdőlap",
    [STR_BR_STOP]      = "Stop",
    [STR_BR_LOADING]   = "Töltés...",
    [STR_BR_ADDR_HINT] = "URL...",
    [STR_BR_ERR_DNS]   = "Hosztnév nem számszerű IPv4 (nincs DNS)",
    [STR_BR_ERR_TCP]   = "TCP kapcsolat sikertelen",
    [STR_BR_IDLE]            = "Tétlen",
    [STR_BR_NAVIGATING]      = "Navigálás ide: %s",
    [STR_BR_BACK_TO]         = "Vissza ide: %s",
    [STR_BR_REFRESHING]      = "Frissítés: %s",
    [STR_BR_INVALID_IP]      = "Érvénytelen IP-cím",
    [STR_BR_CONNECTING]      = "Kapcsolódás: %s:%d ...",
    [STR_BR_CONNECTED]       = "Csatlakozva a proxyhoz",
    [STR_BR_CONN_LOST]       = "Kapcsolat megszakadt",
    [STR_BR_JPEG_TRUNC]      = "JPEG csonka (%d bájt)",
    [STR_BR_JPEG_FAIL]       = "JPEG dekódolás sikertelen (%d bájt)",
    [STR_BR_BAD_JPEG]        = "Hibás JPEG SOI (kapott %02x %02x)",
    [STR_BR_INCOMPLETE_JPEG] = "Hiányos JPEG (nincs EOI)",
    [STR_BR_CONNECTED_SINCE] = "Csatlakozva (%u ms az utolsó képkocka óta)",
    [STR_BR_DISCONNECTED]    = "Szétkapcsolva - újrapróbálkozás...",
    [STR_BR_PROXY_CHANGED]   = "Proxy módosítva: %s - újracsatlakozás...",
    [STR_BR_CONN_FAILED]     = "Proxy kapcsolat sikertelen (%s:%d)",
    [STR_BR_CONN_TIMEOUT]    = "Kapcsolat időtúllépés (%s:%d)",
    [STR_BR_READY]           = "Kész.  Írd át a fenti URL-t és nyomj Entert (vagy Mehet).",
    [STR_BR_NO_FRAMES]       = "Nincs képkocka a proxytól - újracsatlakozás...",
    [STR_BR_PROXY_SETTINGS]  = "Proxy beállítások",
    [STR_BR_PROXY_IP_PROMPT] = "Proxy IP-cím:",
    [STR_BR_NEED_NIC1]       = "A NexxoN Böngészőhöz hálózati adapter kell.",
    [STR_BR_NEED_NIC2]       = "Nem található E1000 (8086:100e) a PCI buszon.",
    [STR_BR_GO]              = "Mehet",

    [STR_DLG_INPUT_HINT]      = "Értéket adjon meg:",
    [STR_DLG_CONFIRM_TITLE]   = "Megerősítés",
    [STR_DLG_ERROR_TITLE]     = "Hiba",
    [STR_DLG_INFO_TITLE]      = "Értesítés",
    [STR_DLG_UNSAVED_TITLE]   = "Mentetlen vált.",
    [STR_DLG_UNSAVED_BODY]    = "Az állomány megváltozott.",
    [STR_DLG_UNSAVED_QUESTION]= "Bezárja mentés nélkül?",

    [STR_MSG_READY]             = "Kész",
    [STR_MSG_LOADING]           = "Töltés...",
    [STR_MSG_DONE]              = "Kész",
    [STR_MSG_FAILED]            = "Sikertelen",
    [STR_MSG_NO_NETWORK]        = "Nincs hálózat",
    [STR_MSG_NO_DEVICE]         = "Nincs eszköz",
    [STR_MSG_INSUFFICIENT_PERM] = "Nincs jogosultság",
    [STR_MSG_INVALID_INPUT]     = "Érvénytelen bemenet",
    [STR_MSG_OPERATION_OK]      = "Művelet kész",

    [STR_BOOT_WELCOME]         = "Üdvözli a NexxoN OS",
    [STR_BOOT_LOADING]         = "Töltés...",
    [STR_BOOT_READY]           = "Rendszer kész",
    [STR_BOOT_LIVE_MODE]       = "ÉLŐ MÓD (csak RAM)",
    [STR_BOOT_INSTALLED_MODE]  = "Boot telepített NexxoN HDD-ről",

    [STR_PANIC_TITLE]          = "NexxoN OS  -  RENDSZERHIBA  (RSOD)",
    [STR_PANIC_CAUSE_CPU]      = "Ok: CPU KIVÉTEL (nem kezelt)",
    [STR_PANIC_CAUSE_KERNEL]   = "Ok: KERNEL HIBA",
    [STR_PANIC_HALTED]         = "Rendszer leállt. Indítsd újra.",
    [STR_PANIC_RECOVERABLE]    = "NexxoN - Helyreállítható hiba",
    [STR_PANIC_RECOVERED]      = "A parancssor helyreállt.",

    [STR_SHELL_PROMPT_USER]    = "felh",
    [STR_SHELL_HELP_HEADER]    = "NexxoN Parancssor - parancs lista",
    [STR_SHELL_HELP_MORE]      = "-- Space: tovább, q: kilép --",
    [STR_SHELL_UNKNOWN_CMD]    = "ismeretlen parancs",
    [STR_SHELL_CWD]            = "könyvtár",
    [STR_SHELL_HELP_FS]        = "Fájlrendszer",
    [STR_SHELL_HELP_NET]       = "Hálózat",
    [STR_SHELL_HELP_SYS]       = "Rendszer",
    [STR_SHELL_HELP_APPS]      = "Alkalmazások",

    /* NexxStore */
    [STR_APP_NEXSTORE]         = "NexxStore",
    [STR_STARTMENU_NEXSTORE]   = "NexxStore",
    [STR_NXST_SEARCH]          = "Csomagok keresése...",
    [STR_NXST_INSTALL]         = "Telepítés",
    [STR_NXST_UNINSTALL]       = "Eltávolítás",
    [STR_NXST_INSTALLED]       = "Telepítve",
    [STR_NXST_AVAILABLE]       = "Elérhető",
    [STR_NXST_NO_NET]          = "Nincs hálózati kapc.",
    [STR_NXST_EMPTY]           = "Nincs csomag",
    [STR_NXST_CONFIRM_INST]    = "Telepíti a csomagot?",
    [STR_NXST_CONFIRM_UNINST]  = "Eltávolítja?",
    [STR_NXST_SUCCESS]         = "Művelet kész",

    /* Explorer USB */
    [STR_XP_SIDEBAR_DRIVES]    = "Meghajtók",
    [STR_XP_USB_EJECT]         = "Kiadás",
    [STR_XP_USB_READONLY]      = "Csak olvasható",
    [STR_XP_USB_COPIED]        = "Másolva USB-re",

    /* Audio Player */
    [STR_APP_AUDIOPLAYER]      = "NexxoN Zene",
    [STR_STARTMENU_AUDIOPLAYER]= "Zenelejátszó",
    [STR_AP_PLAY]              = "Lejátszás",
    [STR_AP_PAUSE]             = "Szünet",
    [STR_AP_STOP]              = "Megállítás",
    [STR_AP_OPEN]              = "Fájl megnyitása",
    [STR_AP_NO_FILE]           = "Nincs betöltött fájl",
    [STR_AP_PLAYING]           = "Lejátszás folyamatban",
    [STR_AP_PAUSED]            = "Szüneteltetve",
    [STR_AP_STOPPED]           = "Leállítva",
    [STR_AP_VOLUME]            = "Hangerő",
    [STR_AP_DURATION]          = "Hossz",

    /* Power Management */
    [STR_PM_SUSPEND]           = "Alvó mód",
    [STR_PM_SUSPEND_CONFIRM]   = "RAM-ba felfüggesztés?",
    [STR_PM_CPUFREQ]           = "CPU frekvencia",
    [STR_PM_FREQ_LOW]          = "Energiatakarékos",
    [STR_PM_FREQ_HIGH]         = "Teljesítmény",
    [STR_PM_FREQ_LABEL]        = "CPU sebesség",

    /* Installer */
    [STR_APP_INSTALLER]        = "NexxoN Telepítő",
    [STR_STARTMENU_INSTALLER]  = "NexxoN telepítése",
    [STR_INST_TITLE]           = "NexxoN OS Telepítő",
    [STR_INST_WELCOME]         = "Üdvözli a NexxoN OS telepítője. Ez a varázsló segít telepíteni a NexxoN OS-t a merevlemezre.",
    [STR_INST_SELECT_DISK]     = "Válassza ki a célmeghajtót:",
    [STR_INST_CONFIRM]         = "FIGYELEM: A kiválasztott lemezen lévő összes adat törlésre kerül. Folytatja?",
    [STR_INST_PROGRESS]        = "NexxoN OS telepítése...",
    [STR_INST_DONE]            = "Telepítés kész! Újraindíthat.",
    [STR_INST_FAILED]          = "Telepítés sikertelen. Ellenőrizze a lemez csatlakozást.",
    [STR_INST_NO_DISK]         = "Nem található megfelelő lemez.",
    [STR_INST_ALREADY]         = "Már telepítve van ezen a lemezen.",
    [STR_INST_BACK]            = "Vissza",
    [STR_INST_NEXT]            = "Következő",
    [STR_INST_INSTALL]         = "Telepítés",
    [STR_INST_STEP_WELCOME]    = "Üdvözlés",
    [STR_INST_STEP_DISK]       = "Lemez",
    [STR_INST_STEP_CONFIRM]    = "Megerősítés",
    [STR_INST_STEP_INSTALL]    = "Telepítés",
    [STR_INST_STEP_DONE]       = "Kész",

    /* Notifications */
    [STR_NOTIFY_DISMISS]       = "Bezárás",

    /* Shell */
    [STR_SHELL_PIPE_ERR]       = "cső: parancs sikertelen",
    [STR_SHELL_REDIR_ERR]      = "átirányítás: fájlhiba",

    /* Multi-monitor */
    [STR_STARTMENU_DISPLAYS]   = "Képernyőkezelő",

    /* Wi-Fi */
    [STR_STARTMENU_WIFI]       = "Wi-Fi hálózatok",

    /* Bluetooth */
    [STR_APP_BLUETOOTH]        = "Bluetooth",
    [STR_STARTMENU_BLUETOOTH]  = "Bluetooth eszközök",
    [STR_BT_TITLE]             = "Bluetooth eszközök",
    [STR_BT_NO_HW]             = "Nincs Bluetooth hardver",
    [STR_BT_SCAN_PROMPT]       = "Nyomja a Keresés gombot",
    [STR_BT_PAIRED]            = "Párosítva",
    [STR_BT_BTN_SCAN]          = "Keresés",
    [STR_BT_BTN_PAIR]          = "Párosítás",
    [STR_BT_CLS_PHONE]         = "[Telefon]",
    [STR_BT_CLS_AUDIO]         = "[Audio]",
    [STR_BT_CLS_KEYS]          = "[Bill.] ",
    [STR_BT_CLS_MOUSE]         = "[Egér]  ",
    [STR_BT_CLS_PC]            = "[PC]    ",
    [STR_BT_CLS_UNK]           = "[?]     ",
    [STR_BT_NOTIFY_TITLE]      = "Bluetooth",
    [STR_BT_NOTIFY_SCAN_OK]    = "Keresés kész",
    [STR_BT_NOTIFY_PAIRED]     = "Párosítva: ",
    [STR_BT_NOTIFY_FAIL]       = "Párosítás sikertelen",

    /* Display / Resolution */
    [STR_SETTINGS_TAB_DISPLAY] = "Képernyő",
    [STR_SETTINGS_TAB_MOUSE]   = "Egér/Bill.",
    [STR_DISP_TITLE]           = "Képernyő beállítások",
    [STR_DISP_RESOLUTION]      = "Felbontás:",
    [STR_DISP_CURRENT]         = "Jelenlegi:",
    [STR_DISP_APPLY]           = "Alkalmaz",
    [STR_DISP_NO_MODES]        = "Nincs videó mód",
    [STR_DISP_KEEP_TITLE]      = "Beállítások megtartása?",
    [STR_DISP_KEEP_MSG]        = "Visszaállítás %d mp múlva...",
    [STR_DISP_KEEP_BTN]        = "Megtart",
    [STR_DISP_REVERT_BTN]      = "Visszaáll",
    [STR_DISP_SWITCH_OK]       = "Felbontás módosítva",
    [STR_DISP_SWITCH_FAIL]     = "Módváltás sikertelen",
    [STR_DISP_REVERTED]        = "Felbontás visszaállítva",

    /* ---- Image Viewer ------------------------------------------------- */
    [STR_APP_IMGVIEW]          = "NexxoN Képnézegető",
    [STR_STARTMENU_IMGVIEW]    = "Képnézegető",
    [STR_IV_OPEN]              = "Kép megnyitása",
    [STR_IV_ZOOM_IN]           = "Nagyítás",
    [STR_IV_ZOOM_OUT]          = "Kicsinyítés",
    [STR_IV_FIT]               = "Illesztés",
    [STR_IV_ACTUAL]            = "1:1",
    [STR_IV_NO_FILE]           = "Nincs betöltött kép. Kattints a Megnyitásra.",
    [STR_IV_LOAD_ERR]          = "Képfájl nem nyitható meg.",
    [STR_IV_FORMAT_ERR]        = "Nem támogatott képformátum.",

    /* ---- Wi-Fi kezelő ------------------------------------------------ */
    [STR_WIFI_NO_HW]           = "Nincs vezeték nélküli hardver",
    [STR_WIFI_CONNECTED]       = "Csatlakozva",
    [STR_WIFI_NETS_FOUND]      = "hálózat található",
    [STR_WIFI_SCAN_HINT]       = "Nyomd meg a Keresést",
    [STR_WIFI_PASSWORD]        = "Jelszó:",
    [STR_WIFI_BTN_SCAN]        = "Keresés",
    [STR_WIFI_BTN_CONNECT]     = "Csatlakozás",
    [STR_WIFI_BTN_DISCONNECT]  = "Leválasztás",
    [STR_WIFI_SCAN_DONE]       = "Keresés kész",
    [STR_WIFI_CONNECT_OK]      = "Csatlakozva ehhez:",
    [STR_WIFI_CONNECT_FAIL]    = "Sikertelen csatlakozás",
    [STR_WIFI_DISCONNECTED]    = "Leválasztva",
    [STR_WIFI_SEC_OPEN]        = "Nyílt",

    /* ---- Képernyő kezelő --------------------------------------------- */
    [STR_DISP_MON_DETECTED]    = "monitor észlelve",
    [STR_DISP_PRIMARY]         = "[Elsődleges]",
    [STR_DISP_MODES]           = "mód",
    [STR_DISP_NOTIFY_MSG]      = "Módváltás kérve (a következő indításkor lép életbe)",
    [STR_DISP_STATUS]          = "Csatlakoztatott kijelzők és felbontások kezelése",

    /* ---- Képernyővédő ------------------------------------------------- */
    [STR_SCREENSAVER_HINT]     = "(bármely billentyű vagy egérmozgás visszatér az asztalra)",

    /* ---- A NexxoN OS névjegye ----------------------------------------- */
    [STR_ABOUT_TITLE]          = "A NexxoN OS névjegye",
    [STR_ABOUT_TAGLINE]        = "64 bites x86_64 operációs rendszer",
    [STR_ABOUT_BUILD]          = "Fordítás",
    [STR_ABOUT_CPUS]           = "Processzorok",
    [STR_ABOUT_MEMORY]         = "Memória",
    [STR_ABOUT_DISPLAY]        = "Kijelző",
    [STR_ABOUT_FEATURES]       = "SMP, USB HID + tároló, FAT/exFAT, AHCI/IDE, TCP/IP, NXFS",
    [STR_ABOUT_HINT]           = "Jobb klikk az asztalon vagy ikonon a további műveletekért.",

    /* USB cserélhető tároló + Intéző VFS műveletek */
    [STR_USB_TITLE]            = "Cserélhető tároló",
    [STR_USB_MOUNTED]          = "USB meghajtó csatlakozott: %s",
    [STR_USB_REMOVED]          = "USB meghajtó eltávolítva: %s",
    [STR_XP_CTX_COPY]          = "Másolás",
    [STR_XP_CTX_PASTE]         = "Beillesztés",
    [STR_XP_CTX_REFRESH]       = "Frissítés",
    [STR_XP_NEWDIR_PROMPT]     = "Mappa neve:",
    [STR_XP_RENAME_PROMPT]     = "Új név:",
    [STR_XP_DELETE_TITLE]      = "Törlés megerősítése",
    [STR_XP_DELETE_Q]          = "Törlöd: '%s' ?",
    [STR_XP_DELETE_RECURSIVE]  = "FIGYELEM: a teljes tartalom törlődik.",
    [STR_XP_OP_FAILED]         = "A művelet nem sikerült",
    [STR_XP_NAME_TAKEN]        = "Ez a név már foglalt.",
    [STR_XP_BAD_SHORTNAME]     = "A név nem használható ezen a köteten (csak 8.3).",
    [STR_XP_FILE_TOO_BIG]      = "A fájl túl nagy a másolási pufferhez.",
    [STR_XP_COPIED]            = "Másolva: %s",
    [STR_XP_PASTE_DONE]        = "Beillesztve: %s",
    [STR_XP_READ_ONLY]         = "Ez a kötet csak olvasható.",
    [STR_EDIT_SAVED_FMT]       = "Mentve: %u bájt.",
    [STR_EDIT_SAVE_FAIL_FMT]   = "A mentés NEM sikerült (kód=%d).",

    /* Browser proxy auto-felderites */
    [STR_BR_PROXY_TITLE]       = "NexxoN Browser",
    [STR_BR_PROXY_FOUND]       = "Proxy megtalálva: %s",

    /* Linux kompatibilitási réteg + Programok állapot */
    [STR_LINUX_ALREADY_RUNNING]  = "Már fut egy program.",
    [STR_LINUX_LOAD_FAILED]      = "A Linux program nem tölthető be.",
    [STR_LINUX_ENTRY_UNSUPPORTED]= "Ehhez a programhoz izolált címtér szükséges.",
    [STR_LINUX_STACK_FAILED]     = "A program vermének előkészítése sikertelen.",
    [STR_LINUX_TERMINATED_FMT]   = "A program biztonságosan leállt (%d kód).",
    [STR_LINUX_EXITED_FMT]       = "A program %d kóddal kilépett.",
    [STR_LINUX_NOT_FOUND_FMT]    = "A program nem található: %s",
    [STR_LINUX_READ_FAILED]      = "A programfájl nem olvasható.",
    [STR_LINUX_IMAGE_TOO_LARGE]  = "A programfájl túl nagy.",
    [STR_LINUX_INTERP_FAILED]    = "A dinamikus betöltő vagy a libc nem tölthető be.",
    [STR_LINUX_LOADING_FMT]      = "linux: %s betöltése ...\n",
    [STR_LINUX_RUNNING_DEMO_FMT] = "linux: static-musl demó futtatása (%u bájt) ...\n",
    [STR_LINUX_RUNNING_DYN_FMT]  = "linux: dynamic-musl próba futtatása (%u bájt) ...\n",
    [STR_LINUX_SUBSYSTEM_ENTER]  = "linux: interaktív alrendszer (/bin/sh) ...\n",
    [STR_LINUX_RESULT_FMT]       = "linux: %s\n",
    [STR_LINUX_OK]               = "befejezve",
    [STR_LINUX_FAILED]           = "sikertelen",
    [STR_PROG_DIR_FAILED]        = "A /programs mappa nem érhető el.",
    [STR_PROG_CREATE_FAILED]     = "A programfájl nem hozható létre.",
    [STR_PROG_INSTALLED_FMT]     = "A linuxdemo telepítve (%u B).",
    [STR_PROG_WRITE_FAILED]      = "A program telepítése sikertelen.",
    [STR_PROG_RESULT_FMT]        = "%s: %s",
    [STR_PROG_REMOVED_FMT]       = "%s eltávolítva.",
    [STR_PROG_REMOVE_FAILED]     = "A program eltávolítása sikertelen.",
    [STR_PROG_SIZE_BYTES_FMT]    = "%u B",
    [STR_PROG_INSTALLED_TESTS_FMT]= "A linuxdemo és linuxexec telepítve (%u B).",
    [STR_PROG_INSTALLED_FULL_FMT] = "Linux tesztprogramok és /lib telepítve (%u B).",
};

static const char *fallback_for(lang_id_t id) {
    if ((int)id < 0 || id >= STR__COUNT) return "?";
    if (EN[id]) return EN[id];
    /* Static buffer used only when an EN entry is missing - keeps a
     * meaningful sentinel visible during development. */
    static char placeholder[16];
    ksnprintf(placeholder, sizeof(placeholder), "?%d?", (int)id);
    return placeholder;
}

/* ---- Listener table ------------------------------------------------- */
#define I18N_MAX_LISTENERS  16
static lang_listener_t g_listeners[I18N_MAX_LISTENERS];
static int             g_n_listeners = 0;
static lang_t          g_lang = LANG_EN;

/* ---- Legacy key strings (preserved for not-yet-migrated callers) --- */
typedef struct {
    const char *key;
    lang_id_t   id;
} legacy_row_t;

static const legacy_row_t g_legacy[] = {
    /* App titles */
    { "app.settings.title",      STR_APP_SETTINGS  },
    { "app.taskmgr.title",       STR_APP_TASKMGR   },
    { "app.usermgr.title",       STR_APP_USERMGR   },
    { "app.explorer.title",      STR_APP_EXPLORER  },
    { "app.browser.title",       STR_APP_BROWSER   },
    { "app.editor.title",        STR_APP_EDITOR    },
    { "app.nexsheet.title",      STR_APP_NEXSHEET  },
    { "app.trash.title",         STR_APP_TRASH     },
    { "app.devmgr.title",        STR_APP_DEVMGR    },
    /* Gephaz tabs */
    { "settings.tab.perf",       STR_SETTINGS_TAB_PERF     },
    { "settings.tab.net",        STR_SETTINGS_TAB_NET      },
    { "settings.tab.theme",      STR_SETTINGS_TAB_THEME    },
    { "settings.tab.store",      STR_SETTINGS_TAB_STORE    },
    { "settings.tab.lang",       STR_SETTINGS_TAB_LANG     },
    { "settings.tab.users",      STR_SETTINGS_TAB_USERS    },
    { "settings.tab.audio",      STR_SETTINGS_TAB_AUDIO    },
    { "settings.tab.security",   STR_SETTINGS_TAB_SECURITY },
    /* Buttons */
    { "btn.ok",                  STR_BTN_OK            },
    { "btn.cancel",              STR_BTN_CANCEL        },
    { "btn.yes",                 STR_BTN_YES           },
    { "btn.no",                  STR_BTN_NO            },
    { "btn.save",                STR_BTN_SAVE          },
    { "btn.close",               STR_BTN_CLOSE         },
    { "btn.minimize",            STR_BTN_MINIMIZE      },
    { "btn.delete",              STR_BTN_DELETE        },
    { "btn.rename",              STR_BTN_RENAME        },
    { "btn.restore",             STR_BTN_RESTORE       },
    { "btn.empty_trash",         STR_BTN_EMPTY_TRASH   },
    { "btn.kill_process",        STR_BTN_KILL_PROCESS  },
    /* Login */
    { "login.welcome",           STR_LOGIN_WELCOME   },
    { "login.username",          STR_LOGIN_USERNAME  },
    { "login.password",          STR_LOGIN_PASSWORD  },
    { "login.signin",            STR_LOGIN_SIGNIN    },
    { "login.bad",               STR_LOGIN_BAD       },
    /* Task Manager */
    { "taskmgr.tab.apps",        STR_TM_TAB_APPS    },
    { "taskmgr.tab.procs",       STR_TM_TAB_PROCS   },
    { "taskmgr.tab.svc",         STR_TM_TAB_SVC     },
    { "taskmgr.tab.perf",        STR_TM_TAB_PERF    },
    { "taskmgr.tab.net",         STR_TM_TAB_NET     },
    { "taskmgr.tab.users",       STR_TM_TAB_USERS   },
    { "taskmgr.menu.file",       STR_TM_MENU_FILE   },
    { "taskmgr.menu.options",    STR_TM_MENU_OPTIONS},
    { "taskmgr.menu.view",       STR_TM_MENU_VIEW   },
    { "taskmgr.menu.help",       STR_TM_MENU_HELP   },
    { "taskmgr.col.name",        STR_TM_COL_NAME    },
    { "taskmgr.col.user",        STR_TM_COL_USER    },
    { "taskmgr.col.cpu",         STR_TM_COL_CPU     },
    { "taskmgr.col.mem",         STR_TM_COL_MEM     },
    { "taskmgr.col.desc",        STR_TM_COL_DESC    },
    { "taskmgr.state.run",       STR_TM_STATE_RUN   },
    { "taskmgr.state.hung",      STR_TM_STATE_HUNG  },
    /* Start menu */
    { "startmenu.shutdown",      STR_STARTMENU_SHUTDOWN },
    { "startmenu.restart",       STR_STARTMENU_RESTART  },
    { "startmenu.settings",      STR_STARTMENU_SETTINGS },
    { "startmenu.users",         STR_STARTMENU_USERS    },
    { "startmenu.taskmgr",       STR_STARTMENU_TASKMGR  },
    { "startmenu.browser",       STR_STARTMENU_BROWSER  },
    { "startmenu.explorer",      STR_STARTMENU_EXPLORER },
    { "startmenu.editor",        STR_STARTMENU_EDITOR   },
    { "startmenu.nexsheet",      STR_STARTMENU_NEXSHEET },
    { "startmenu.trash",         STR_STARTMENU_TRASH    },
    /* Messages */
    { "msg.ready",               STR_MSG_READY        },
    { "msg.loading",             STR_MSG_LOADING      },
    { "msg.no_network",          STR_MSG_NO_NETWORK   },
    { "msg.trash_empty",         STR_TRASH_EMPTY      },
    { NULL,                      STR__COUNT           },
};

/* ============================================================================
 * Public API
 * ============================================================================ */
void i18n_init(void) {
    g_lang = LANG_EN;
    g_n_listeners = 0;
    for (int i = 0; i < I18N_MAX_LISTENERS; i++) g_listeners[i] = NULL;
    debug_printf("[i18n] v2.0 table ready (%d STR_ entries), default = EN\n",
                 (int)STR__COUNT);
}

lang_t i18n_get_language(void) { return g_lang; }

void i18n_set_language(lang_t lang) {
    if (lang == g_lang) return;
    g_lang = lang;
    /* Re-bind the keyboard layout so the layout-table swap happens
     * atomically with the visible-language swap. */
    keyboard_set_layout(lang == LANG_HU ? KBD_LAYOUT_HU : KBD_LAYOUT_US);
    /* Notify every registered listener.  Each gets exactly one call. */
    for (int i = 0; i < g_n_listeners; i++) {
        if (g_listeners[i]) g_listeners[i](lang);
    }
    /* Force the WM to re-run every window's resize_cb (which by
     * convention is also the "redraw everything from scratch" entry
     * point) so chrome + tabs + buttons + status bars refresh in the
     * new language inside a single compose pass.  Layout-engine bounds
     * checking (gfx_draw_string_clipped) handles longer HU strings. */
    wm_force_global_repaint();
    debug_printf("[i18n] language switched -> %s; %d listener(s) notified\n",
                 lang == LANG_HU ? "HU" : "EN", g_n_listeners);
}

const char *lang_get(lang_id_t id) {
    if ((int)id < 0 || id >= STR__COUNT) return "?";
    const char *s = (g_lang == LANG_HU) ? HU[id] : EN[id];
    if (!s) return fallback_for(id);
    return s;
}

void lang_register_cb(lang_listener_t cb) {
    if (!cb || g_n_listeners >= I18N_MAX_LISTENERS) return;
    /* De-dupe: ignore duplicate registrations. */
    for (int i = 0; i < g_n_listeners; i++) {
        if (g_listeners[i] == cb) return;
    }
    g_listeners[g_n_listeners++] = cb;
}

/* Map a dotted-namespace legacy key to its lang_id_t. */
static lang_id_t legacy_lookup(const char *key) {
    for (int i = 0; g_legacy[i].key; i++) {
        if (strcmp(g_legacy[i].key, key) == 0) return g_legacy[i].id;
    }
    return STR__COUNT;
}

const char *i18n(const char *key) {
    if (!key) return "";
    lang_id_t id = legacy_lookup(key);
    if (id == STR__COUNT) return key;     /* surfaces missing keys */
    return lang_get(id);
}

const char *i18n_or(const char *key, const char *fallback) {
    if (!key) return fallback ? fallback : "";
    lang_id_t id = legacy_lookup(key);
    if (id == STR__COUNT) return fallback ? fallback : key;
    return lang_get(id);
}
