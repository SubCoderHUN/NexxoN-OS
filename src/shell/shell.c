/* ============================================================================
 * NexxoN OS - Interactive shell  (v2.0)
 * ----------------------------------------------------------------------------
 *  Upgrades over v1:
 *
 *   * Command-line history (Up/Down arrows recall previous lines).
 *   * efile launches the new full-screen NexxoN Edit editor.
 *   * Reacts to keyboard-layout switches (AltGr+H / AltGr+A) by printing a
 *     notification line between command prompts.
 *   * New commands: tree, echo, beep, reboot, kbd.
 *
 * Architecture stays the same: read a line, tokenise on whitespace, run a
 * handler that owns the side-effects.  The editor and PC-speaker submodules
 * are linked in but only invoked from here.
 * ============================================================================ */
#include "sysdisk.h"
#include "shell.h"
#include "terminal.h"
#include "keyboard.h"
#include "editor.h"
#include "doomapp.h"
#include "linuxsys.h"
#include "programs.h"
#include "speaker.h"
#include "string.h"
#include "nxfs.h"
#include "ahci.h"
#include "pit.h"
#include "debug.h"
#include "vga.h"
#include "io.h"
#include "window.h"
#include "mouse.h"
#include "mtrr.h"
#include "vmm.h"
#include "driver.h"
#include "nxscript.h"
#include "gfx.h"
#include "font.h"
#include "acpi.h"
#include "dialogs.h"
#include "rtc.h"
#include "tasktimer.h"
#include "panic.h"
#include "desktop.h"
#include "explorer.h"
#include "nexstore.h"
#include "audioplayer.h"
#include "imgview.h"
#include "installer.h"
#include "multimon.h"
#include "wifi.h"
#include "bluetooth.h"
#include "apps.h"
#include "usermode.h"
#include "net.h"
#include "netif.h"
#include "e1000.h"
#include "i18n.h"
#include "auth.h"
#include "clipboard.h"
#include "theme.h"
#include "pnp.h"
#include "vfs.h"
#include "usb.h"

#define SHELL_LINE_MAX   256
#define SHELL_PROMPT_FG  VGA_GREEN
#define SHELL_TEXT_FG    VGA_LTGRAY

/* ---------- Command history (ring buffer) ------------------------------- */
#define HIST_SIZE     32
static char     g_hist[HIST_SIZE][SHELL_LINE_MAX];
static int      g_hist_count = 0;          /* total saved (0..HIST_SIZE) */
static int      g_hist_next  = 0;          /* next write slot */

static void hist_push(const char *line) {
    if (!line[0]) return;
    if (g_hist_count > 0) {
        int last = (g_hist_next - 1 + HIST_SIZE) % HIST_SIZE;
        if (strcmp(g_hist[last], line) == 0) return;   /* skip dup of last */
    }
    strncpy(g_hist[g_hist_next], line, SHELL_LINE_MAX - 1);
    g_hist[g_hist_next][SHELL_LINE_MAX - 1] = 0;
    g_hist_next = (g_hist_next + 1) % HIST_SIZE;
    if (g_hist_count < HIST_SIZE) g_hist_count++;
}

/* offset_back: 1 = most recent, 2 = second most recent, ... */
static const char *hist_get(int offset_back) {
    if (offset_back <= 0 || offset_back > g_hist_count) return NULL;
    int idx = (g_hist_next - offset_back + HIST_SIZE) % HIST_SIZE;
    return g_hist[idx];
}

/* ---------- System Monitor shim -----------------------------------------
 * Legacy `taskmgr` rendered a text-only window; the new graphical
 * implementation lives in src/drivers/taskmgr.c (apps.h).  We keep a
 * thin maybe_update_taskmgr() that just polls the new module's tick so
 * existing call sites in read_line and shell_run don't have to change. */
static void maybe_update_taskmgr(void) {
    taskmgr_tick();
}

static void cmd_taskmgr(void) {
    if (apps_launch(taskmgr_open, "taskmgr")) {
        term_printf("taskmgr: Task Manager opened.\n");
    } else {
        term_printf("taskmgr: out of window slots\n");
    }
}

static void cmd_devmgr(void) {
    if (apps_launch(devmgr_open, "devmgr")) {
        term_printf("devmgr: Device Manager opened.\n");
    } else {
        term_printf("devmgr: out of window slots\n");
    }
}

static void cmd_pong(void) {
    if (apps_launch(pong_open, "pong")) {
        term_printf("pong: NexxoN Pong opened.  Mouse controls right paddle.\n");
    } else {
        term_printf("pong: out of window slots\n");
    }
}

static void cmd_ping(int argc, char **argv) {
    if (!e1000_present()) {
        term_printf("ping: no NIC detected (E1000 missing)\n");
        return;
    }
    if (argc < 2) { term_printf("usage: ping <ip>\n"); return; }
    uint32_t ip = net_ip_aton(argv[1]);
    if (ip == 0) { term_printf("ping: invalid IP '%s'\n", argv[1]); return; }
    char addr[32];
    net_ip_ntoa(ip, addr, sizeof(addr));
    term_printf("PING %s: 32 data bytes\n", addr);
    for (int i = 0; i < 4; i++) {
        int rtt = net_ping(ip, 1500);
        if (rtt < 0) {
            term_set_color(VGA_YELLOW, VGA_BLACK);
            term_printf("  seq=%d  timeout\n", i);
            term_set_color(VGA_LTGRAY, VGA_BLACK);
        } else {
            term_set_color(VGA_GREEN, VGA_BLACK);
            term_printf("  seq=%d  rtt=%d ms\n", i, rtt);
            term_set_color(VGA_LTGRAY, VGA_BLACK);
        }
        pit_sleep(200);
    }
}

static void cmd_ifconfig(int argc, char **argv) {
    if (!e1000_present()) {
        term_printf("ifconfig: no NIC\n");
        return;
    }
    /* `ifconfig -v` dumps the raw NIC register/ring state (link, RX/TX rings)
     * so a bare-metal NIC that won't get a DHCP lease can be diagnosed from a
     * single screenshot instead of guessing. */
    if (argc >= 2 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--verbose") == 0)) {
        static char nd[1600];
        e1000_diag(nd, sizeof(nd));
        term_printf("%s", nd);
    }
    net_config_t c;
    net_get_config(&c);
    char ip[32], gw[32], mask[32], dns[32];
    net_ip_ntoa(c.ip, ip, sizeof(ip));
    net_ip_ntoa(c.gateway, gw, sizeof(gw));
    net_ip_ntoa(c.netmask, mask, sizeof(mask));
    net_ip_ntoa(c.dns, dns, sizeof(dns));
    term_printf("eth0:\n");
    term_printf("    MAC      : %02x:%02x:%02x:%02x:%02x:%02x\n",
                c.mac[0], c.mac[1], c.mac[2], c.mac[3], c.mac[4], c.mac[5]);
    term_printf("    IPv4     : %s\n", ip);
    term_printf("    Netmask  : %s\n", mask);
    term_printf("    Gateway  : %s\n", gw);
    term_printf("    DNS      : %s\n", dns);
    term_printf("    Mode     : %s\n", c.dhcp ? "DHCP" : "STATIC");
}

/* TASK 3/4: USB controller + device enumeration. */
static void cmd_lsusb(void) {
    int nc = usb_controller_count();
    if (nc == 0) {
        term_printf("lsusb: no USB host controllers detected.\n");
        return;
    }
    term_printf("USB host controllers (%d):\n", nc);
    for (int i = 0; i < nc; i++) {
        usb_controller_t c;
        if (!usb_get_controller(i, &c)) continue;
        const char *k = (c.kind == USB_CTRL_UHCI) ? "UHCI" :
                        (c.kind == USB_CTRL_OHCI) ? "OHCI" :
                        (c.kind == USB_CTRL_EHCI) ? "EHCI" :
                        (c.kind == USB_CTRL_XHCI) ? "xHCI" : "?";
        term_printf("  Bus %02d  %s  %04x:%04x  ports=%d  irq=%d\n",
                    i, k, c.vendor_id, c.device_id, c.num_ports, c.irq);
        for (int p = 0; p < c.num_ports; p++) {
            term_printf("    Port %d  %s\n", p,
                        c.port_attached[p] ? "[attached]" : "[empty]");
        }
    }
    int nd = usb_device_count();
    term_printf("\nUSB devices (%d):\n", nd);
    for (int i = 0; i < nd; i++) {
        usb_device_t *d = usb_get_device(i);
        if (!d) continue;
        char line[100];
        usb_describe(d, line, sizeof(line));
        term_printf("  %s\n", line);
    }
}

/* TASK 26: VFS mount table dump. */
static void cmd_mounts(void) {
    int n = vfs_mount_count();
    term_printf("Mount table (%d external mounts):\n", n);
    term_printf("  /              NXFS  (root, internal storage)\n");
    char info[80];
    for (int i = 0; i < n; i++) {
        vfs_mount_t *m = vfs_get_mount(i);
        if (!m) continue;
        info[0] = 0;
        if (m->info) m->info(m, info, sizeof(info));
        term_printf("  /%-12s  %s  '%s'\n", m->mountpoint, info, m->label);
    }
    if (pnp_mount_count() > 0) {
        term_printf("\nUSB volumes (%d):\n", pnp_mount_count());
        char line[80];
        for (int i = 0; i < pnp_mount_count(); i++) {
            if (pnp_mount_describe(i, line, sizeof(line)) > 0)
                term_printf("  %s\n", line);
        }
    }
}

/* TASK 26: external-FS directory listing. */
static void cmd_ls_usb(int argc, char **argv) {
    const char *path = (argc >= 2) ? argv[1] : "/usb0/";
    vfs_entry_t entries[64];
    int n = vfs_list(path, entries, 64);
    if (n < 0) {
        term_printf("ls: cannot list '%s' (mount or path not found)\n", path);
        return;
    }
    if (n == 0) {
        term_printf("(empty)\n");
        return;
    }
    term_printf("Listing of %s (%d entries):\n", path, n);
    for (int i = 0; i < n; i++) {
        term_printf("  %c  %-32s  %u bytes\n",
                    entries[i].is_dir ? 'D' : 'F',
                    entries[i].name, entries[i].size);
    }
}

/* utest-write: quick VFS write/read round-trip test on an exFAT USB mount. */
static void cmd_utest_write(int argc, char **argv) {
    const char *mount = (argc >= 2) ? argv[1] : "/usb0";
    char wpath[64], rpath[64];
    ksnprintf(wpath, sizeof(wpath), "%s/WRTEST.TXT", mount);
    ksnprintf(rpath, sizeof(rpath), "%s/TESTDIR",    mount);

    static const char PAYLOAD[] = "NexxoN-OS exFAT write test OK";
    uint32_t plen = (uint32_t)(sizeof(PAYLOAD) - 1);

    term_printf("utest-write: writing '%s' ...\n", wpath);
    int wr = vfs_write_file(wpath, PAYLOAD, plen);
    if (wr != 0) { term_printf("utest-write: FAIL (write returned %d)\n", wr); return; }

    static char rbuf[128];
    int rd = vfs_read(wpath, rbuf, sizeof(rbuf) - 1);
    if (rd < 0) { term_printf("utest-write: FAIL (read returned %d)\n", rd); return; }
    rbuf[rd] = 0;

    if ((uint32_t)rd != plen || memcmp(rbuf, PAYLOAD, plen) != 0)
        term_printf("utest-write: FAIL (data mismatch: got '%s')\n", rbuf);
    else
        term_printf("utest-write: PASS (write+read OK, %u bytes)\n", plen);

    term_printf("utest-write: mkdir '%s' ...\n", rpath);
    int mk = vfs_mkdir_path(rpath);
    term_printf("utest-write: mkdir -> %d\n", mk);

    term_printf("utest-write: delete '%s' ...\n", wpath);
    int del = vfs_delete(wpath);
    term_printf("utest-write: delete -> %d\n", del);
}

/* TASK 31: dark mode CLI parity for the Gephaz toggle. */
static void cmd_theme(int argc, char **argv) {
    if (argc < 2) {
        term_printf("theme: current = %s   (usage: theme light|dark)\n",
                    theme_is_dark() ? "dark" : "light");
        return;
    }
    if (strcmp(argv[1], "dark") == 0) {
        theme_set_dark(true);
        wm_mark_dirty();
        term_printf("theme: switched to dark\n");
    } else if (strcmp(argv[1], "light") == 0) {
        theme_set_dark(false);
        wm_mark_dirty();
        term_printf("theme: switched to light\n");
    } else {
        term_printf("theme: unknown mode '%s' (use light|dark)\n", argv[1]);
    }
}

/* TASK 25: clipboard CLI bindings.  `copy <text...>` stores arguments
 * joined with spaces; `paste` prints what's currently in the buffer. */
static void cmd_copy(int argc, char **argv) {
    if (argc < 2) {
        clipboard_set_text("", 0);
        term_printf("copy: clipboard cleared\n");
        return;
    }
    char buf[CLIPBOARD_MAX_BYTES];
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        int len = (int)strlen(argv[i]);
        if (pos + len + 1 >= (int)sizeof(buf)) break;
        if (i > 1) buf[pos++] = ' ';
        memcpy(buf + pos, argv[i], (size_t)len);
        pos += len;
    }
    buf[pos] = 0;
    uint32_t n = clipboard_set_text(buf, (uint32_t)pos);
    term_printf("copy: stored %u byte%s\n", n, n == 1 ? "" : "s");
}

static void cmd_paste(void) {
    uint32_t n = clipboard_len();
    if (n == 0) {
        term_printf("paste: clipboard is empty\n");
        return;
    }
    term_printf("paste (%u bytes): %s\n", n, clipboard_peek_text());
}

/* `ntp [ip]`: synchronous SNTPv4 query.  Pure read — RTC is not
 * updated because the CMOS driver is read-only in this build — but
 * the result is printed in UTC and you can use it to sanity-check a
 * skewed BIOS clock. */
static void cmd_ntp(int argc, char **argv) {
    if (!netif_present()) {
        term_printf("ntp: no NIC\n");
        return;
    }
    uint32_t ip = 0;
    if (argc >= 2) {
        ip = net_ip_aton(argv[1]);
        if (ip == 0) {
            term_printf("ntp: invalid IP '%s'\n", argv[1]);
            return;
        }
    }
    term_printf("ntp: querying %s (timeout 2000 ms)...\n",
                argc >= 2 ? argv[1] : "default server");
    uint32_t unix_secs = 0;
    if (ntp_query_unix(ip, 2000, &unix_secs) != 0) {
        term_set_color(VGA_YELLOW, VGA_BLACK);
        term_printf("ntp: query failed\n");
        term_set_color(VGA_LTGRAY, VGA_BLACK);
        return;
    }
    /* Quick UTC breakdown (no leap-second handling). */
    uint32_t day = unix_secs / 86400u;
    uint32_t r   = unix_secs % 86400u;
    uint32_t hh  = r / 3600u, mm = (r / 60u) % 60u, ss = r % 60u;
    static const uint8_t mdays_normal[12] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    uint32_t year = 1970;
    while (1) {
        bool leap = ((year & 3) == 0 && (year % 100 != 0)) ||
                    (year % 400 == 0);
        uint32_t ylen = leap ? 366u : 365u;
        if (day < ylen) break;
        day -= ylen;
        year++;
    }
    bool leap = ((year & 3) == 0 && (year % 100 != 0)) || (year % 400 == 0);
    uint32_t month = 1;
    for (uint32_t m = 0; m < 12; m++) {
        uint32_t mlen = mdays_normal[m] + ((m == 1 && leap) ? 1u : 0u);
        if (day < mlen) { month = m + 1u; break; }
        day -= mlen;
    }
    term_set_color(VGA_GREEN, VGA_BLACK);
    term_printf("ntp: %04u-%02u-%02u %02u:%02u:%02u UTC  (unix=%u)\n",
                year, month, (uint32_t)(day + 1u), hh, mm, ss, unix_secs);
    term_set_color(VGA_LTGRAY, VGA_BLACK);
}

/* `dhcp`: request a fresh lease from the local DHCP server.  Updates
 * the global net_config so subsequent ping / browser calls use the new
 * address.  Acts as the CLI counterpart to the Gephaz Network tab's
 * Renew button (CLI parity, TASK 19). */
static void cmd_dhcp(void) {
    if (!netif_present()) {
        term_printf("dhcp: no NIC available\n");
        return;
    }
    term_printf("dhcp: requesting lease (timeout 3000 ms)...\n");
    int r = dhcp_request(3000);
    if (r != 0) {
        term_set_color(VGA_YELLOW, VGA_BLACK);
        term_printf("dhcp: no response from DHCP server\n");
        term_set_color(VGA_LTGRAY, VGA_BLACK);
        return;
    }
    term_set_color(VGA_GREEN, VGA_BLACK);
    term_printf("dhcp: lease acquired\n");
    term_set_color(VGA_LTGRAY, VGA_BLACK);
    cmd_ifconfig(0, NULL);
}

/* Legacy text-only System Monitor kept around for the old `sysmon`
 * command and the screensaver kick test plumbing.  Pre-existing callers
 * outside this file (kernel debug prints, panic recovery) still
 * reference the static window pointers; keep them defined as no-ops. */

/* ---------- Prompt ------------------------------------------------------- */
static void prompt(void) {
    term_pin_to_bottom();   /* keep prompt pinned to last row of window */
    char path[256];
    if (nxfs_pwd_path(path, sizeof(path)) != NXFS_OK) strcpy(path, "/");
    vga_color_t fg, bg;
    term_get_color(&fg, &bg);
    const char *lay = (keyboard_get_layout() == KBD_LAYOUT_HU) ? "HU" : "EN";
    bool live      = (nxfs_mode() == NXFS_MODE_LIVE);
    bool installed = nxfs_is_installed();

    /* "nexxon" in green */
    term_set_color(SHELL_PROMPT_FG, bg);
    term_printf("nexxon[%s", lay);

    /* Mode tag: LIVE in yellow when running from RAMFS, HDD in cyan
     * when running from a stamped installation - so the user always
     * sees at a glance whether their changes are persistent.  We DON'T
     * tag a non-installed SATA boot - that's a transient state during
     * the first installsys run and the lack of tag is its own signal. */
    if (live) {
        term_set_color(VGA_YELLOW, bg);
        term_printf("|LIVE");
        term_set_color(SHELL_PROMPT_FG, bg);
    } else if (installed) {
        term_set_color(VGA_CYAN, bg);
        term_printf("|HDD");
        term_set_color(SHELL_PROMPT_FG, bg);
    }
    term_printf("]:%s$ ", path);

    term_set_color(fg, bg);
    term_idle_show();
}

/* ---------- Display a layout-change notification if pending ------------- */
static void maybe_print_layout_change(void) {
    const char *n = keyboard_pop_layout_change();
    if (!n) return;
    vga_color_t fg, bg;
    term_get_color(&fg, &bg);
    term_set_color(VGA_CYAN, bg);
    term_printf("\n%s\n", n);
    term_set_color(fg, bg);
}

/* ---------- read_line: line editing with cursor movement ---------------- */
/* Erase the entire current input (go to end, then backspace-erase to 0). */
static void rl_erase(char *buf, int *n, int *cur) {
    for (int i = *cur; i < *n; i++) term_putc(buf[i]); /* advance to end */
    for (int i = 0; i < *n; i++) term_putc('\b');       /* erase backwards */
    *n = 0; *cur = 0;
}

/* ---------- TAB auto-complete (TASK 11) ---------------------------------
 *  Matching strategy:
 *      - If the cursor is at the start or on the first token of the line,
 *        complete against the static list of shell commands.
 *      - Otherwise, complete against the cwd NXFS entries (folders + files).
 *  When a single candidate matches, insert the remainder.  When several
 *  share a common prefix, extend the prefix and beep.  When more than one
 *  candidate exists, double-TAB prints them on a new line and reprints
 *  the prompt + buffer.                                                  */
static const char *const g_shell_commands[] = {
    "lf", "tree", "cfile", "dfile", "efile", "rfile", "cat",
    "cdir", "ddir", "edir", "cd", "pwd",
    "copy", "paste", "grep", "wc", "tee",
    "trash", "trash-in", "trash-empty",
    "inf", "sysrep", "usbnative", "drivers", "mouse-reset", "kbd", "lang",
    "theme", "whoami", "mounts", "lsusb", "ls-usb",
    "echo", "beep", "clear", "cpufreq", "suspend",
    "installsys", "reinstallsys",
    "reboot", "shutdown",
    "run", "script", "taskmgr", "devmgr", "pong",
    "ping", "ifconfig", "dhcp", "ntp", "browser", "wifi", "bluetooth", "bt",
    "settings", "gephaz", "ossettings", "usermgr", "usermode",
    "music", "audioplayer", "imgview", "nexstore", "store", "installer",
    "displays", "multimon",
    "time", "date", "tasktimer",
    "addicon", "explorer", "utest-write",
    "nexsheet", "sheet",
    "help",
    NULL
};

/* Return the start of the token currently under the cursor.  A "token"
 * stops at whitespace.  cur is the cursor's position within buf. */
static int rl_token_start(const char *buf, int cur) {
    int s = cur;
    while (s > 0 && buf[s - 1] != ' ' && buf[s - 1] != '\t') s--;
    return s;
}

/* True iff the entire prefix of buf[0..cur) contains no whitespace.
 * Used to decide whether the user is on the first token of the line
 * (and therefore wants command completion). */
static bool rl_in_first_token(const char *buf, int cur) {
    for (int i = 0; i < cur; i++) {
        if (buf[i] == ' ' || buf[i] == '\t') return false;
    }
    return true;
}

#define RL_MATCH_MAX 32
typedef struct {
    const char *items[RL_MATCH_MAX];
    char        storage[RL_MATCH_MAX][NXFS_NAME_MAX];
    int         count;
} rl_match_t;

static void rl_add_match(rl_match_t *m, const char *s) {
    if (m->count >= RL_MATCH_MAX) return;
    strncpy(m->storage[m->count], s, NXFS_NAME_MAX - 1);
    m->storage[m->count][NXFS_NAME_MAX - 1] = 0;
    m->items[m->count] = m->storage[m->count];
    m->count++;
}

static bool rl_prefix(const char *s, const char *p) {
    while (*p) { if (*s++ != *p++) return false; }
    return true;
}

static void rl_collect_commands(const char *prefix, rl_match_t *m) {
    for (int i = 0; g_shell_commands[i]; i++) {
        if (rl_prefix(g_shell_commands[i], prefix)) {
            rl_add_match(m, g_shell_commands[i]);
        }
    }
}

static void rl_collect_cwd_cb(const nxfs_inode_t *node, void *user) {
    rl_match_t *m = (rl_match_t *)user;
    rl_add_match(m, node->name);
}

static void rl_collect_files(const char *prefix, rl_match_t *m) {
    rl_match_t all = { .count = 0 };
    nxfs_list(nxfs_cwd(), rl_collect_cwd_cb, &all);
    for (int i = 0; i < all.count; i++) {
        if (rl_prefix(all.items[i], prefix)) {
            rl_add_match(m, all.items[i]);
        }
    }
}

/* Find the longest common prefix among the strings in `m`. */
static int rl_lcp_len(const rl_match_t *m) {
    if (m->count == 0) return 0;
    int i = 0;
    while (1) {
        char c = m->items[0][i];
        if (!c) return i;
        for (int j = 1; j < m->count; j++) {
            if (m->items[j][i] != c) return i;
        }
        i++;
    }
}

static void rl_insert_text(char *buf, int *n, int *cur, int max_len,
                           const char *src, int src_len) {
    for (int i = 0; i < src_len; i++) {
        if (*n + 1 >= max_len) break;
        memmove(buf + *cur + 1, buf + *cur, (size_t)(*n - *cur));
        buf[*cur] = src[i];
        (*n)++;
        for (int j = *cur; j < *n; j++) term_putc(buf[j]);
        (*cur)++;
        for (int j = *cur; j < *n; j++) term_cursor_move_left();
    }
}

static void rl_complete(char *buf, int *n, int *cur, int max_len,
                        int *consecutive_tabs) {
    /* Identify the active token. */
    int tstart = rl_token_start(buf, *cur);
    int tlen   = *cur - tstart;
    if (tlen < 0) tlen = 0;
    char prefix[NXFS_NAME_MAX];
    int copy = tlen < (int)(sizeof(prefix) - 1) ? tlen : (int)(sizeof(prefix) - 1);
    memcpy(prefix, buf + tstart, (size_t)copy);
    prefix[copy] = 0;

    rl_match_t m = { .count = 0 };
    if (rl_in_first_token(buf, *cur)) {
        rl_collect_commands(prefix, &m);
    } else {
        rl_collect_files(prefix, &m);
    }

    if (m.count == 0) {
        /* No match - beep softly. */
        speaker_beep(800, 30);
        return;
    }

    if (m.count == 1) {
        /* Unique completion: insert the missing tail. */
        const char *full = m.items[0];
        int full_len = (int)strlen(full);
        if (full_len > tlen) {
            rl_insert_text(buf, n, cur, max_len, full + tlen, full_len - tlen);
        }
        /* A trailing space helps the user start the next argument. */
        if (*n + 1 < max_len && (*cur == *n || buf[*cur] != ' ')) {
            rl_insert_text(buf, n, cur, max_len, " ", 1);
        }
        *consecutive_tabs = 0;
        return;
    }

    /* Multiple matches: extend by the longest common prefix. */
    int lcp = rl_lcp_len(&m);
    if (lcp > tlen) {
        rl_insert_text(buf, n, cur, max_len, m.items[0] + tlen, lcp - tlen);
    }

    /* On a second consecutive TAB: print the candidate list and reprompt. */
    (*consecutive_tabs)++;
    if (*consecutive_tabs >= 2) {
        term_putc('\n');
        for (int i = 0; i < m.count; i++) {
            term_printf("  %s", m.items[i]);
            if ((i & 3) == 3) term_putc('\n');
        }
        if ((m.count & 3) != 0) term_putc('\n');
        prompt();
        for (int i = 0; i < *n; i++) term_putc(buf[i]);
        for (int i = *cur; i < *n; i++) term_cursor_move_left();
        *consecutive_tabs = 0;
    } else {
        speaker_beep(1200, 20);
    }
}

static int read_line(char *buf, int max_len) {
    int n = 0;      /* number of chars in buf */
    int cur = 0;    /* cursor position within buf, 0..n */
    int hist_pos = 0;
    int tab_count = 0;     /* consecutive TAB presses for double-tab list */
    char saved[SHELL_LINE_MAX];
    saved[0] = 0;

    while (1) {
        /* Idle: drive WM + taskmgr + screensaver while waiting.  The
         * condition explicitly stays in the idle loop whenever a
         * non-shell window has keyboard focus — that ensures keys typed
         * into the browser / editor / etc never bubble up to the shell's
         * read_line buffer, even when the buffered key arrives in the
         * tiny window between wm_dispatch_keys() and the loop check. */
        while (!keyboard_has_data() || !wm_is_shell_focused() ||
               desktop_search_active()) {
            wm_tick();
            /* #10: while the Start-menu search box is focused it owns the
             * keyboard -- feed bytes to it instead of the shell prompt. */
            if (desktop_search_active() && keyboard_has_data()) {
                desktop_handle_key(keyboard_getc());
            }
            maybe_update_taskmgr();
            taskmgr_tick();
            pong_tick();
            browser_tick();
            doomapp_tick();
            nexsheet_tick();
            audioplayer_tick();
            imgview_tick();
            gephaz_tick();
            net_tick();
            screensaver_tick();
            tasktimer_tick();
            pnp_tick();
            /* Win+Del global shortcut (Task 33) - open Task Manager. */
            if (keyboard_taskmgr_requested()) (void)apps_launch(taskmgr_open, "taskmgr");
            /* Windows key tapped on its own toggles the Start menu. */
            if (keyboard_startmenu_requested()) desktop_toggle_menu();
            /* Route buffered keys to the focused window's installed
             * handler (editor, browser, future text-input apps).  The
             * shell window is protected and never installs a handler,
             * so read_line only sees keys when the shell is focused. */
            (void)wm_dispatch_keys();
            if (keyboard_has_data() && !wm_is_shell_focused()) {
                /* Focused window declined / had no handler — drain the
                 * byte so it doesn't leak into the next shell prompt. */
                (void)keyboard_getc();
            }
            /* If the user double-clicked a desktop icon while we were
             * waiting at an EMPTY prompt, break out of the idle loop so
             * the shell main loop can drain the queued command.  When
             * the user has already typed something we defer the icon
             * action until they finish (Enter / Ctrl+C) so we don't
             * mash a half-typed line on top of icon output. */
            if (n == 0 && desktop_has_pending_command()) {
                buf[0] = 0;
                return 0;
            }
            pit_idle_hlt();
        }
        int c = keyboard_wait_getc();
        if (c == 0) continue;

        maybe_print_layout_change();
        term_idle_hide();

        /* Ctrl+C: drop the line in progress and start fresh.  The shell
         * doesn't print a fancy ^C glyph here - it just abandons the
         * current input so the next prompt comes up clean. */
        if (c == KEY_CTRL_C) {
            keyboard_clear_abort();
            term_putc('\n');
            buf[0] = 0;
            return 0;
        }

        /* ----- Special keys -------------------------------------------- */
        if (c == KEY_ENTER) {
            /* Advance cursor to end so the newline is emitted correctly. */
            for (int i = cur; i < n; i++) term_putc(buf[i]);
            term_putc('\n');
            buf[n] = 0;
            return n;
        }

        if (c == KEY_BACKSPACE) {
            if (cur > 0) {
                memmove(buf + cur - 1, buf + cur, (size_t)(n - cur));
                n--; cur--;
                term_cursor_move_left();
                for (int i = cur; i < n; i++) term_putc(buf[i]);
                term_putc(' ');                          /* erase ghost char */
                for (int i = cur; i <= n; i++) term_cursor_move_left();
            }
            term_idle_show();
            continue;
        }

        if (c == KEY_LEFT) {
            if (cur > 0) { cur--; term_cursor_move_left(); }
            term_idle_show();
            continue;
        }

        if (c == KEY_RIGHT) {
            if (cur < n) { term_putc(buf[cur]); cur++; }
            term_idle_show();
            continue;
        }

        if (c == KEY_HOME) {
            while (cur > 0) { cur--; term_cursor_move_left(); }
            term_idle_show();
            continue;
        }

        if (c == KEY_END) {
            while (cur < n) { term_putc(buf[cur]); cur++; }
            term_idle_show();
            continue;
        }

        if (c == KEY_UP) {
            if (hist_pos == 0) { memcpy(saved, buf, (size_t)n); saved[n] = 0; }
            const char *h = hist_get(hist_pos + 1);
            if (h) {
                hist_pos++;
                rl_erase(buf, &n, &cur);
                strncpy(buf, h, (size_t)(max_len - 1));
                buf[max_len - 1] = 0;
                n = cur = (int)strlen(buf);
                term_puts(buf);
            }
            term_idle_show();
            continue;
        }

        if (c == KEY_DOWN) {
            if (hist_pos > 0) {
                hist_pos--;
                rl_erase(buf, &n, &cur);
                const char *src = (hist_pos == 0) ? saved : hist_get(hist_pos);
                if (!src) src = "";
                strncpy(buf, src, (size_t)(max_len - 1));
                buf[max_len - 1] = 0;
                n = cur = (int)strlen(buf);
                term_puts(buf);
            }
            term_idle_show();
            continue;
        }

        /* TASK 9: PgUp/PgDn pan the terminal scrollback ring without
         * disturbing the in-progress edit buffer.  Half-screen jumps so
         * a few presses traverse the full 256-row history quickly. */
        if (c == KEY_PGUP) {
            term_scroll_up((int)(term_rows() / 2));
            continue;
        }
        if (c == KEY_PGDN) {
            term_scroll_down((int)(term_rows() / 2));
            continue;
        }

        /* TASK 25: Ctrl+V pastes clipboard text at the cursor.  Newlines
         * inside the clipboard are translated to spaces — pressing Enter
         * is the user's responsibility, not the paste payload's.  For
         * pastes in the middle of the line we redraw the entire line so
         * the displayed text stays in sync with the buffer. */
        if (c == KEY_CTRL_V) {
            uint32_t clen = clipboard_len();
            if (clen == 0) { term_idle_show(); continue; }
            const char *clip = clipboard_peek_text();
            int before_cur = cur;
            for (uint32_t i = 0; i < clen && n < max_len - 1; i++) {
                char ch = clip[i];
                if (ch == '\n' || ch == '\r') ch = ' ';
                if (ch < 0x20 && ch != '\t') continue;
                for (int j = n; j > cur; j--) buf[j] = buf[j - 1];
                buf[cur] = ch;
                n++; cur++;
            }
            buf[n] = 0;
            (void)before_cur;
            if (cur == n) {
                /* paste landed at end of line — just print new bytes. */
                for (int i = before_cur; i < cur; i++) term_putc(buf[i]);
            } else {
                /* paste in middle — redraw the whole tail and back the
                 * terminal cursor up to the logical insertion point. */
                for (int i = before_cur; i < n; i++) term_putc(buf[i]);
                for (int i = cur; i < n; i++) term_cursor_move_left();
            }
            term_idle_show();
            continue;
        }

        /* TAB: command / filename auto-complete. */
        if (c == KEY_TAB) {
            rl_complete(buf, &n, &cur, max_len, &tab_count);
            term_idle_show();
            continue;
        }
        /* Any other key resets the consecutive-tab counter. */
        if (c != 0) tab_count = 0;

        /* ----- Printable insert at cursor ------------------------------ */
        /* Accept 0x20-0x7E (ASCII) and 0xA0-0xFF (ISO-8859-2 extended).
         * 0x7F-0x9F is excluded: 0x7F=DEL, 0x80-0x9F=KEY_* private codes. */
        if ((c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF)) {
            if (n + 1 < max_len) {
                memmove(buf + cur + 1, buf + cur, (size_t)(n - cur));
                buf[cur] = (char)c;
                n++;
                for (int i = cur; i < n; i++) term_putc(buf[i]);
                cur++;
                for (int i = cur; i < n; i++) term_cursor_move_left();
            }
        }
        /* Other special keys (F-keys, etc.) are silently ignored. */

        term_idle_show();
    }
}

/* ---------- Tokeniser --------------------------------------------------- */
static int tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    int i = 0;
    while (line[i] && argc < max_args) {
        while (line[i] == ' ' || line[i] == '\t') i++;
        if (!line[i]) break;
        argv[argc++] = &line[i];
        while (line[i] && line[i] != ' ' && line[i] != '\t') i++;
        if (line[i]) { line[i] = 0; i++; }
    }
    return argc;
}

/* ---------- nxfs error -> string ---------------------------------------- */
static const char *nxfs_strerror(int r) {
    switch (r) {
        case NXFS_OK:           return "ok";
        case NXFS_ERR_NOSPACE:  return "out of inodes";
        case NXFS_ERR_NOTFOUND: return "not found";
        case NXFS_ERR_EXISTS:   return "already exists";
        case NXFS_ERR_NOTDIR:   return "not a directory";
        case NXFS_ERR_NOTFILE:  return "not a file";
        case NXFS_ERR_NAME:     return "invalid name";
        case NXFS_ERR_FULL:     return "directory full";
        case NXFS_ERR_IO:       return "disk I/O error";
        case NXFS_ERR_TOOBIG:   return "file too large";
        default:                return "unknown error";
    }
}

/* ---------- Command: lf ------------------------------------------------- */
static void cb_list(const nxfs_inode_t *node, void *user) {
    (void)user;
    const char *kind = (node->type == NXFS_TYPE_DIR) ? "DIR " : "FILE";
    vga_color_t fg, bg;
    term_get_color(&fg, &bg);
    term_set_color(node->type == NXFS_TYPE_DIR ? VGA_CYAN : VGA_LTGRAY, bg);
    term_printf("  %s  %-32s", kind, node->name);
    term_set_color(VGA_GRAY, bg);
    if (node->type == NXFS_TYPE_FILE) {
        term_printf("  %u bytes\n", node->size);
    } else {
        term_printf("  %u entries\n", node->size);
    }
    term_set_color(fg, bg);
}

static void cmd_lf(void) {
    nxfs_inode_t cur;
    if (nxfs_read_inode(nxfs_cwd(), &cur) != NXFS_OK) {
        term_printf("lf: failed to read directory inode\n");
        return;
    }
    if (cur.child_count == 0) {
        term_printf("(empty)\n");
        return;
    }
    nxfs_list(nxfs_cwd(), cb_list, NULL);
}

/* ---------- Command: cfile / dfile -------------------------------------- */
static void cmd_cfile(int argc, char **argv) {
    if (argc < 2) { term_printf("usage: cfile <name>\n"); return; }
    uint32_t ino;
    int r = nxfs_create_file(nxfs_cwd(), argv[1], &ino);
    if (r != NXFS_OK) term_printf("cfile: %s\n", nxfs_strerror(r));
    else              term_printf("created file '%s' (inode %u)\n", argv[1], ino);
}

static void cmd_dfile(int argc, char **argv) {
    if (argc < 2) { term_printf("usage: dfile <name>\n"); return; }
    /* #2: route deletions through the Recycle Bin so they are restorable,
     * matching the file-manager behaviour.  Fall back to a hard unlink only
     * if the move fails. */
    if (trash_move_in(argv[1]) == 0) {
        term_printf("moved '%s' to Recycle Bin\n", argv[1]);
        return;
    }
    int r = nxfs_delete_file(nxfs_cwd(), argv[1]);
    if (r != NXFS_OK) term_printf("dfile: %s\n", nxfs_strerror(r));
    else              term_printf("deleted file '%s'\n", argv[1]);
}

/* ---------- Command: cdir / ddir / edir --------------------------------- */
static void cmd_cdir(int argc, char **argv) {
    if (argc < 2) { term_printf("usage: cdir <name>\n"); return; }
    uint32_t ino;
    int r = nxfs_create_dir(nxfs_cwd(), argv[1], &ino);
    if (r != NXFS_OK) term_printf("cdir: %s\n", nxfs_strerror(r));
    else              term_printf("created dir '%s' (inode %u)\n", argv[1], ino);
}

static void cmd_ddir(int argc, char **argv) {
    if (argc < 2) { term_printf("usage: ddir <name>\n"); return; }
    int r = nxfs_delete_dir(nxfs_cwd(), argv[1]);
    if (r != NXFS_OK) term_printf("ddir: %s\n", nxfs_strerror(r));
    else              term_printf("deleted dir '%s'\n", argv[1]);
}

static void cmd_edir(int argc, char **argv) {
    if (argc < 3) { term_printf("usage: edir <old> <new>\n"); return; }
    int r = nxfs_rename_dir(nxfs_cwd(), argv[1], argv[2]);
    if (r != NXFS_OK) term_printf("edir: %s\n", nxfs_strerror(r));
    else              term_printf("renamed '%s' -> '%s'\n", argv[1], argv[2]);
}

/* ---------- Command: cd / pwd ------------------------------------------- */
static void cmd_cd(int argc, char **argv) {
    if (argc < 2) { term_printf("usage: cd <name>\n"); return; }
    uint32_t ino;
    int r = nxfs_resolve(nxfs_cwd(), argv[1], &ino);
    if (r != NXFS_OK) { term_printf("cd: %s\n", nxfs_strerror(r)); return; }
    r = nxfs_set_cwd(ino);
    if (r != NXFS_OK) term_printf("cd: %s\n", nxfs_strerror(r));
}

static void cmd_pwd(void) {
    char p[256];
    nxfs_pwd_path(p, sizeof(p));
    term_printf("%s\n", p);
}

/* ---------- Command: efile (launches full-screen editor) ---------------- */
/* ---------- Command: linux (run an x86_64 Linux ELF) -------------------- */
extern const uint8_t _binary_build_userland_linuxdemo_elf_start[];
extern const uint8_t _binary_build_userland_linuxdemo_elf_end[];
extern const uint8_t _binary_build_userland_linuxdyn_elf_start[];
extern const uint8_t _binary_build_userland_linuxdyn_elf_end[];
static void cmd_linux(int argc, char **argv) {
    char msg[96] = {0};
    int rc;
    if (argc >= 2 && strcmp(argv[1], "dyn") == 0) {
        uint32_t len = (uint32_t)(_binary_build_userland_linuxdyn_elf_end -
                                  _binary_build_userland_linuxdyn_elf_start);
        term_printf(L(STR_LINUX_RUNNING_DYN_FMT), len);
        rc = linux_run_image(_binary_build_userland_linuxdyn_elf_start, len,
                             "linuxdyn", msg, sizeof(msg));
    } else if (argc >= 2) {
        term_printf(L(STR_LINUX_LOADING_FMT), argv[1]);
        rc = linux_run_path_args(argv[1], argc - 1,
                                 (const char *const *)&argv[1],
                                 msg, sizeof(msg));
    } else {
        term_printf(L(STR_LINUX_SUBSYSTEM_ENTER));
        rc = linux_subsystem_enter(msg, sizeof(msg));
    }
    term_set_color(rc >= 0 ? VGA_GREEN : VGA_RED, VGA_BLACK);
    term_printf(L(STR_LINUX_RESULT_FMT),
                msg[0] ? msg : L(rc >= 0 ? STR_LINUX_OK : STR_LINUX_FAILED));
    term_set_color(SHELL_TEXT_FG, VGA_BLACK);
}

static void cmd_efile(int argc, char **argv) {
    if (argc < 2) { term_printf("usage: efile <name>\n"); return; }
    uint32_t ino;
    int r = nxfs_resolve(nxfs_cwd(), argv[1], &ino);
    if (r != NXFS_OK) {
        r = nxfs_create_file(nxfs_cwd(), argv[1], &ino);
        if (r != NXFS_OK) {
            term_printf("efile: %s\n", nxfs_strerror(r));
            return;
        }
        term_printf("(file '%s' did not exist - created)\n", argv[1]);
    }
    nxfs_inode_t f;
    if (nxfs_read_inode(ino, &f) != NXFS_OK) { term_printf("efile: io\n"); return; }
    if (f.type != NXFS_TYPE_FILE) { term_printf("efile: not a file\n"); return; }

    editor_run(ino, argv[1]);
}

/* ---------- Command: rfile --------------------------------------------- */
static void cmd_rfile(int argc, char **argv) {
    if (argc < 2) { term_printf("usage: rfile <name>\n"); return; }
    uint32_t ino;
    int r = nxfs_resolve(nxfs_cwd(), argv[1], &ino);
    if (r != NXFS_OK) { term_printf("rfile: %s\n", nxfs_strerror(r)); return; }

    static char buf[NXFS_MAX_BLOCKS * NXFS_SECTOR_SIZE];
    uint32_t got = 0;
    r = nxfs_read_file(ino, buf, sizeof(buf), &got);
    if (r != NXFS_OK) { term_printf("rfile: %s\n", nxfs_strerror(r)); return; }
    term_printf("--- %s (%u bytes) ---\n", argv[1], got);
    for (uint32_t i = 0; i < got; i++) term_putc(buf[i]);
    if (got && buf[got - 1] != '\n') term_putc('\n');
    term_printf("---\n");
}

/* ---------- Command: tree (recursive directory listing) ----------------- */
static void tree_print_node(uint32_t inode, int depth) {
    nxfs_inode_t n;
    if (nxfs_read_inode(inode, &n) != NXFS_OK) return;

    /* Indentation: 4 spaces per depth level, with a vertical-bar guide. */
    for (int i = 0; i < depth; i++) term_printf(" |  ");
    if (depth > 0) {
        vga_color_t fg, bg;
        term_get_color(&fg, &bg);
        term_set_color(n.type == NXFS_TYPE_DIR ? VGA_CYAN : VGA_LTGRAY, bg);
        if (n.type == NXFS_TYPE_DIR) term_printf("[%s]\n", n.name);
        else                         term_printf("%s  (%u B)\n", n.name, n.size);
        term_set_color(fg, bg);
    } else {
        vga_color_t fg, bg;
        term_get_color(&fg, &bg);
        term_set_color(VGA_CYAN, bg);
        term_printf("[/]\n");
        term_set_color(fg, bg);
    }

    if (n.type == NXFS_TYPE_DIR) {
        for (uint32_t i = 0; i < n.child_count; i++) {
            tree_print_node(n.children[i], depth + 1);
        }
    }
}

static void cmd_tree(void) {
    tree_print_node(nxfs_cwd(), 0);
}

/* ---------- Command: echo ----------------------------------------------- */
static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        term_printf("%s%s", argv[i], (i + 1 < argc) ? " " : "");
    }
    term_printf("\n");
}

/* ---------- Command: beep ----------------------------------------------- */
static uint32_t parse_uint(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

static void cmd_beep(int argc, char **argv) {
    uint32_t hz = 880;     /* nice A5 by default */
    uint32_t ms = 150;
    if (argc >= 2) hz = parse_uint(argv[1]);
    if (argc >= 3) ms = parse_uint(argv[2]);
    if (hz == 0) hz = 880;
    if (ms == 0) ms = 150;
    term_printf("beep: %u Hz for %u ms\n", hz, ms);
    speaker_beep(hz, ms);
}

/* ---------- Command: fsx (FS write-path exerciser) -----------------------
 * Scripted QEMU/bare-metal diagnostics for BOTH filesystem stacks:
 * paths whose first component is a VFS mount ("/usb0/...") exercise the
 * USB FAT/exFAT path; everything else ("/big.bin") goes to NXFS v3 via
 * the streaming write + 64-bit read-at-offset API.  Deterministic
 * pattern files (byte i = i mod 251) prove on-disk content end to end. */
static uint8_t g_fsx_buf[64 * 1024];

static bool fsx_is_vfs(const char *path) {
    const char *p = path;
    while (*p == '/') p++;
    char first[VFS_NAME_MAX];
    int n = 0;
    while (p[n] && p[n] != '/' && n < VFS_NAME_MAX - 1) { first[n] = p[n]; n++; }
    first[n] = 0;
    return vfs_find_mount(first) != NULL;
}

/* Split an NXFS path into (parent inode, leaf name). */
static int fsx_nxfs_parent(const char *path, uint32_t *out_parent,
                           char *leaf, int leaf_cap) {
    const char *last = NULL;
    const char *p = path;
    while (*p == '/') p++;
    for (const char *q = p; *q; q++) if (*q == '/') last = q;
    if (!last) {
        strncpy(leaf, p, leaf_cap - 1);
        leaf[leaf_cap - 1] = 0;
        *out_parent = 0;
        return NXFS_OK;
    }
    char parent[256];
    int plen = (int)(last - p);
    if (plen >= (int)sizeof(parent)) plen = sizeof(parent) - 1;
    memcpy(parent, p, plen);
    parent[plen] = 0;
    strncpy(leaf, last + 1, leaf_cap - 1);
    leaf[leaf_cap - 1] = 0;
    return nxfs_resolve_path(parent, out_parent);
}

static void fsx_nxfs_ls_cb(const nxfs_inode_t *node, void *user) {
    (void)user;
    term_printf("  %s%s  %u\n", node->name,
                node->type == NXFS_TYPE_DIR ? "/" : "", node->size);
}

static void cmd_fsx(int argc, char **argv) {
    if (argc < 3) {
        term_printf("usage: fsx w <path> <KiB> | v <path> | rm <path> |\n");
        term_printf("       fsx ren <path> <newname> | md <path> |\n");
        term_printf("       fsx ls <path> | cp <src> <dst>\n");
        term_printf("  paths on a VFS mount (/usb0/..) use FAT/exFAT,\n");
        term_printf("  anything else uses NXFS v3.\n");
        return;
    }
    const char *op   = argv[1];
    const char *path = argv[2];
    bool vfs = fsx_is_vfs(path);

    if (strcmp(op, "w") == 0) {
        uint32_t kb = (argc >= 4) ? parse_uint(argv[3]) : 4;
        if (kb == 0) kb = 4;
        uint32_t total = kb * 1024u;
        uint32_t nx_ino = 0;
        if (vfs) {
            if (vfs_write_open(path) != 0) {
                term_printf("fsx: write_open failed: %s\n", path);
                return;
            }
        } else {
            uint32_t parent; char leaf[64];
            if (fsx_nxfs_parent(path, &parent, leaf, sizeof(leaf)) != NXFS_OK) {
                term_printf("fsx: no such NXFS dir for %s\n", path);
                return;
            }
            if (nxfs_resolve(parent, leaf, &nx_ino) != NXFS_OK &&
                nxfs_create_file(parent, leaf, &nx_ino) != NXFS_OK) {
                term_printf("fsx: create failed: %s\n", path);
                return;
            }
            if (nxfs_write_begin(nx_ino) != NXFS_OK) {
                term_printf("fsx: write_begin failed: %s\n", path);
                return;
            }
        }
        uint32_t off = 0;
        uint32_t t0 = pit_ms();
        while (off < total) {
            uint32_t chunk = total - off;
            if (chunk > sizeof(g_fsx_buf)) chunk = sizeof(g_fsx_buf);
            for (uint32_t i = 0; i < chunk; i++)
                g_fsx_buf[i] = (uint8_t)((off + i) % 251u);
            int r = vfs ? vfs_write_append(path, g_fsx_buf, chunk)
                        : nxfs_write_append(g_fsx_buf, chunk);
            if (r != 0) {
                term_printf("fsx: append failed at %u (%d)\n", off, r);
                if (!vfs) nxfs_write_end(false);
                return;
            }
            off += chunk;
            wm_tick(); pnp_tick();
        }
        int rc = vfs ? vfs_write_close(path, true) : nxfs_write_end(true);
        if (rc != 0) {
            term_printf("fsx: close/commit failed (%d)\n", rc);
            return;
        }
        term_printf("fsx: wrote %u bytes -> %s (%u ms)\n",
                    total, path, pit_ms() - t0);
    } else if (strcmp(op, "v") == 0) {
        uint32_t nx_ino = 0;
        if (!vfs && nxfs_resolve_path(path, &nx_ino) != NXFS_OK) {
            term_printf("fsx: not found: %s\n", path);
            return;
        }
        uint32_t off = 0, bad = 0xFFFFFFFFu;
        for (;;) {
            int n = vfs ? vfs_read_at(path, off, g_fsx_buf, sizeof(g_fsx_buf))
                        : nxfs_read_at(nx_ino, off, g_fsx_buf, sizeof(g_fsx_buf));
            if (n < 0) { term_printf("fsx: read_at failed at %u\n", off); return; }
            if (n == 0) break;
            for (int i = 0; i < n; i++) {
                if (g_fsx_buf[i] != (uint8_t)((off + (uint32_t)i) % 251u)) {
                    if (bad == 0xFFFFFFFFu) bad = off + (uint32_t)i;
                }
            }
            off += (uint32_t)n;
            wm_tick(); pnp_tick();
            if ((uint32_t)n < sizeof(g_fsx_buf)) break;
        }
        if (bad != 0xFFFFFFFFu)
            term_printf("fsx: VERIFY BAD at byte %u (%u bytes read)\n", bad, off);
        else
            term_printf("fsx: verify OK, %u bytes\n", off);
    } else if (strcmp(op, "rm") == 0) {
        if (vfs) {
            term_printf("fsx: delete %s -> %d\n", path, vfs_delete(path));
        } else {
            uint32_t parent; char leaf[64];
            if (fsx_nxfs_parent(path, &parent, leaf, sizeof(leaf)) != NXFS_OK) {
                term_printf("fsx: no such NXFS dir\n");
                return;
            }
            int r = nxfs_delete_file(parent, leaf);
            if (r == NXFS_ERR_NOTFILE) r = nxfs_delete_dir(parent, leaf);
            term_printf("fsx: delete %s -> %d\n", path, r);
        }
    } else if (strcmp(op, "ren") == 0) {
        if (argc < 4) { term_printf("fsx: ren needs <newname>\n"); return; }
        if (vfs) {
            term_printf("fsx: rename %s -> '%s': %d\n",
                        path, argv[3], vfs_rename(path, argv[3]));
        } else {
            uint32_t parent; char leaf[64];
            if (fsx_nxfs_parent(path, &parent, leaf, sizeof(leaf)) != NXFS_OK) {
                term_printf("fsx: no such NXFS dir\n");
                return;
            }
            term_printf("fsx: rename %s -> '%s': %d\n",
                        path, argv[3], nxfs_rename(parent, leaf, argv[3]));
        }
    } else if (strcmp(op, "md") == 0) {
        if (vfs) {
            term_printf("fsx: mkdir %s -> %d\n", path, vfs_mkdir_path(path));
        } else {
            uint32_t parent; char leaf[64];
            if (fsx_nxfs_parent(path, &parent, leaf, sizeof(leaf)) != NXFS_OK) {
                term_printf("fsx: no such NXFS dir\n");
                return;
            }
            term_printf("fsx: mkdir %s -> %d\n",
                        path, nxfs_create_dir(parent, leaf, NULL));
        }
    } else if (strcmp(op, "ls") == 0) {
        if (vfs) {
            static vfs_entry_t ents[64];
            int n = vfs_list(path, ents, 64);
            if (n < 0) { term_printf("fsx: list failed: %s\n", path); return; }
            for (int i = 0; i < n; i++)
                term_printf("  %s%s  %u\n", ents[i].name,
                            ents[i].is_dir ? "/" : "", ents[i].size);
            term_printf("fsx: %d entries\n", n);
        } else {
            uint32_t ino;
            if (nxfs_resolve_path(path, &ino) != NXFS_OK) {
                term_printf("fsx: not found: %s\n", path);
                return;
            }
            nxfs_list(ino, fsx_nxfs_ls_cb, NULL);
        }
    } else if (strcmp(op, "cp") == 0) {
        if (argc < 4) { term_printf("fsx: cp needs <dst>\n"); return; }
        extern void pnp_tick(void);
        int r = vfs_copy_streamed(path, argv[3], g_fsx_buf,
                                  sizeof(g_fsx_buf), NULL);
        term_printf("fsx: copy %s -> %s: %d bytes\n", path, argv[3], r);
    } else if (strcmp(op, "many") == 0) {
        /* Create <count> 1 KiB pattern files with longish names inside
         * <path> - forces directory cluster extension. */
        uint32_t count = (argc >= 4) ? parse_uint(argv[3]) : 8;
        uint32_t made = 0;
        for (uint32_t i = 0; i < count; i++) {
            char fp[300];
            ksnprintf(fp, sizeof(fp),
                      "%s/long-directory-stress-entry-number-%03u.bin",
                      path, i);
            if (vfs_write_open(fp) != 0) break;
            for (uint32_t k = 0; k < 1024; k++)
                g_fsx_buf[k] = (uint8_t)(k % 251u);
            if (vfs_write_append(fp, g_fsx_buf, 1024) != 0) break;
            if (vfs_write_close(fp, true) != 0) break;
            made++;
            wm_tick(); pnp_tick();
        }
        term_printf("fsx: created %u/%u files in %s\n", made, count, path);
    } else {
        term_printf("fsx: unknown op '%s'\n", op);
    }
}

/* ---------- Command: nxfsck (NXFS v3 consistency check) ------------------ */
static void cmd_nxfsck(int argc, char **argv) {
    bool repair = (argc >= 2 && strcmp(argv[1], "repair") == 0);
    char report[160];
    report[0] = 0;
    uint32_t t0 = pit_ms();
    int r = nxfs_check(repair, report, sizeof(report));
    if (r < 0) {
        term_printf("nxfsck: I/O error (%d)\n", r);
        return;
    }
    term_set_color(r == 0 ? VGA_GREEN : VGA_YELLOW, VGA_BLACK);
    term_printf("nxfsck: %s\n", report);
    term_set_color(SHELL_TEXT_FG, VGA_BLACK);
    term_printf("nxfsck: finished in %u ms (%s)\n", pit_ms() - t0,
                repair ? "repair" : "read-only");
}

/* ---------- Command: shutdown ------------------------------------------- */
static void cmd_shutdown(void) {
    term_printf("Shutting down via ACPI...\n");
    /* Let the message render before we tear the rest of the system down.
     * pit_sleep() is safe here because interrupts are still on; acpi_shutdown
     * disables them itself before talking to the power register. */
    pit_sleep(200);
    acpi_shutdown();   /* does not return */
}

/* ---------- Command: run (execute an NXScript file) --------------------- */
static void cmd_run(int argc, char **argv) {
    if (argc < 2) { term_printf("usage: run <file>\n"); return; }
    uint32_t ino;
    int r = nxfs_resolve(nxfs_cwd(), argv[1], &ino);
    if (r != NXFS_OK) { term_printf("run: %s\n", nxfs_strerror(r)); return; }

    static char script_buf[NXFS_MAX_BLOCKS * NXFS_SECTOR_SIZE];
    uint32_t got = 0;
    r = nxfs_read_file(ino, script_buf, sizeof(script_buf) - 1, &got);
    if (r != NXFS_OK) { term_printf("run: read failed (%s)\n", nxfs_strerror(r)); return; }
    script_buf[got] = '\0';

    r = nxscript_eval(script_buf);
    if (r != NX_OK) {
        term_set_color(VGA_RED, VGA_BLACK);
        term_printf("run: NXScript error: %s\n", nxscript_last_error());
        term_set_color(SHELL_TEXT_FG, VGA_BLACK);
    }
}

/* ---------- Command: reboot ---------------------------------------------
 * Delegates to acpi_reboot() which tries a sequence of bounded reset
 * mechanisms (PCI 0xCF9 -> 8042 0xFE -> Fast Reset 0x92 -> Triple Fault).
 * The previous implementation could spin forever waiting for an 8042
 * input-buffer drain that never happened on some VM configs, which was
 * the root cause of the "VirtualBox eats 10 GB of host RAM" hang. */
static void cmd_reboot(void) {
    term_printf("Rebooting (PCI 0xCF9 -> 8042 -> Fast Reset -> Triple Fault) ...\n");
    pit_sleep(300);     /* let the message render */
    acpi_reboot();      /* does not return */
}

/* ---------- Command: installsys (RAMFS -> SATA installer) ---------------
 * Refuses unless we are running in Live Mode AND an AHCI SATA disk is
 * present.  Prompts for explicit Y/N confirmation, then writes a 1-
 * partition MBR + dumps the live NXFS image onto the disk's first
 * partition (LBA 2048..).  After this completes, the next reboot will
 * find a valid NXFS superblock and mount from disk. */
/* Read a single Y/N (or Esc / Ctrl+C) from the user.  Returns:
 *      1  - user accepted (Y / y)
 *      0  - user declined (N / n / Esc / Ctrl+C / abort flag)
 *
 * Defensive hardening done HERE rather than at every call site:
 *   - keyboard_drain() up front so a stray Enter / Y queued by the
 *     previous command can NEVER auto-answer this prompt;
 *   - wm_refocus_shell() so the console always owns input even if a
 *     mouse-driven focus change put another window on top;
 *   - every received byte is echoed back, so the user sees their
 *     keystroke land even when it's not a valid answer (silently
 *     dropped chars used to look like "the terminal is dead");
 *   - KEY_CTRL_C and the abort flag are treated as "no", so the user
 *     always has an out without having to reach for the power button.
 */
static int wait_yes_no(void) {
    keyboard_drain();
    wm_refocus_shell();
    keyboard_clear_abort();
    debug_printf("[shell] wait_yes_no: entering input loop\n");

    while (1) {
        /* Tick the WM while waiting so the compositor keeps painting and
         * mouse stays responsive - otherwise the screen looks frozen. */
        while (!keyboard_has_data()) {
            if (keyboard_abort_requested()) {
                keyboard_clear_abort();
                term_printf("^C (aborted)\n");
                return 0;
            }
            wm_tick();
            pit_idle_hlt();
        }
        int c = keyboard_wait_getc();
        if (c == 0) continue;

        /* Ctrl+C: treat as decline-and-abort. */
        if (c == KEY_CTRL_C) {
            keyboard_clear_abort();
            term_printf("^C\n");
            return 0;
        }
        if (c == 'Y' || c == 'y') { term_printf("Y\n"); return 1; }
        if (c == 'N' || c == 'n' || c == KEY_ESCAPE) {
            term_printf("N\n");
            return 0;
        }
        /* Anything else: echo so the user knows the prompt is alive,
         * then keep waiting.  ASCII printables go through term_putc
         * directly; control codes and out-of-band keys are summarised. */
        if (c >= 0x20 && c < 0x7F) {
            term_putc((char)c);
        } else {
            term_printf("[?]");
        }
    }
}

/* ---------- Shared installer body --------------------------------------
 * Both `installsys` (first-time install) and `reinstallsys` (overwrite an
 * existing install) want exactly the same on-disk result.  The only thing
 * that differs is the wording of the warning banner and the post-success
 * message.  This helper does the work; the two commands are thin wrappers
 * that pass the printable verb and a bool selecting which copy to render.
 *
 * Hard guard: BOTH commands refuse unless nxfs_mode() == NXFS_MODE_LIVE.
 * That means an already-installed disk cannot accidentally reformat itself
 * just because someone typed `installsys` at the shell.  To wipe an
 * existing install you must boot the LIVE CD/USB - which is exactly what
 * a "reinstallsys" workflow expects. */
static void do_install_to_disk(const char *verb, bool reinstall) {
    if (nxfs_mode() != NXFS_MODE_LIVE) {
        term_set_color(VGA_YELLOW, VGA_BLACK);
        term_printf("%s: this command only works from the LIVE boot CD/USB.\n", verb);
        term_printf("%s: you are running off the installed disk - boot the LIVE\n", verb);
        term_printf("%s: media first if you want to (re)install.\n", verb);
        term_set_color(VGA_LTGRAY, VGA_BLACK);
        return;
    }
    if (!sysdisk_present()) {
        term_printf("%s: no SATA disk detected by the AHCI driver.\n", verb);
        return;
    }

    uint32_t sectors = sysdisk_sector_count();
    term_set_color(VGA_RED, VGA_BLACK);
    term_printf("\n==========================  WARNING  ===========================\n");
    if (reinstall) {
        term_printf(" REINSTALL: this will WIPE the existing NexxoN installation\n");
        term_printf(" on the SATA disk and replace it with a fresh image built\n");
        term_printf(" from the LIVE filesystem you are currently running.\n");
    } else {
        term_printf(" This will COMPLETELY ERASE the SATA disk\n");
    }
    term_printf("   AHCI port %u   size = %u sectors (%u MiB)\n",
                ahci_port_index(), sectors, sectors / 2048);
    term_printf(" and write a new MBR + NXFS partition starting at LBA %u.\n",
                (uint32_t)NXFS_PARTITION_OFFSET);
    term_printf(" Every file and directory currently visible in the LIVE\n");
    term_printf(" filesystem will be copied 1:1 onto the new partition.\n");
    term_printf("================================================================\n");
    term_set_color(VGA_LTGRAY, VGA_BLACK);
    term_printf("Continue? (Y/N): ");
    if (!wait_yes_no()) {
        term_printf("%s: aborted.\n", verb);
        return;
    }

    term_set_color(VGA_CYAN, VGA_BLACK);
    term_printf("[%s] step 1/3  -  writing MBR + stage2 + kernel ...\n", verb);
    term_printf("[%s] step 2/3  -  formatting NXFS v3 + copying the live tree ...\n",
                verb);
    term_set_color(VGA_LTGRAY, VGA_BLACK);

    int r = nxfs_install_to_sata();
    if (r != NXFS_OK) {
        term_set_color(VGA_RED, VGA_BLACK);
        term_printf("[%s] FAILED (code %d).  The disk may be in an "
                    "inconsistent state.\n", verb, r);
        term_set_color(VGA_LTGRAY, VGA_BLACK);
        return;
    }

    term_set_color(VGA_GREEN, VGA_BLACK);
    term_printf("[%s] step 3/3  -  done.  %s complete.\n",
                verb, reinstall ? "Reinstallation" : "Installation");
    term_printf("[%s] Disk signature stamped.  Remove the LIVE CD/USB,\n", verb);
    term_printf("              reboot, and the BIOS will hand off directly to\n");
    term_printf("              the SATA disk.  Stage-2 loads the new kernel\n");
    term_printf("              and the shell prompt switches from |LIVE to |HDD.\n");
    term_set_color(VGA_LTGRAY, VGA_BLACK);
}

static void cmd_installsys(void) {
    do_install_to_disk("installsys", false);
}

static void cmd_reinstallsys(void) {
    do_install_to_disk("reinstallsys", true);
}

/* ---------- Command: script (NXScript REPL) -----------------------------
 * Enters a one-line REPL.  Each line is fed to nxscript_eval() until the
 * user types `.exit`.  Multi-line task definitions can be entered with
 * semicolons / braces on one line, just like in the dispatch path. */
static void cmd_script(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[SHELL_LINE_MAX];
    term_set_color(VGA_CYAN, VGA_BLACK);
    term_printf("NXScript REPL ready.  Type '.exit' to leave, '.reset' to clear.\n");
    term_printf("Keywords: task num txt numdec bit if else loop step pick out next stop\n");
    term_set_color(SHELL_TEXT_FG, VGA_BLACK);
    while (1) {
        term_set_color(VGA_MAGENTA, VGA_BLACK);
        term_printf("nx> ");
        term_set_color(SHELL_TEXT_FG, VGA_BLACK);
        int n = read_line(line, sizeof(line));
        if (n <= 0) continue;
        if (strcmp(line, ".exit")  == 0) { term_printf("(leaving NXScript REPL)\n"); return; }
        if (strcmp(line, ".reset") == 0) { nxscript_reset(); term_printf("[NXScript state cleared]\n"); continue; }
        int r = nxscript_eval(line);
        if (r != NX_OK) {
            term_set_color(VGA_RED, VGA_BLACK);
            term_printf("NXScript error: %s\n", nxscript_last_error());
            term_set_color(SHELL_TEXT_FG, VGA_BLACK);
        }
    }
}

/* ---------- Command: kbd (switch layout from the shell) ----------------- */
static void cmd_kbd(int argc, char **argv) {
    if (argc < 2) {
        term_printf("layout: %s\n",
                    keyboard_get_layout() == KBD_LAYOUT_HU ? "HU" : "EN");
        term_printf("usage: kbd <en|hu>   (or use AltGr+A / AltGr+H)\n");
        return;
    }
    if (strcmp(argv[1], "hu") == 0 || strcmp(argv[1], "HU") == 0) {
        keyboard_set_layout(KBD_LAYOUT_HU);
    } else if (strcmp(argv[1], "en") == 0 || strcmp(argv[1], "EN") == 0 ||
               strcmp(argv[1], "us") == 0 || strcmp(argv[1], "US") == 0) {
        keyboard_set_layout(KBD_LAYOUT_US);
    } else {
        term_printf("kbd: unknown layout '%s'\n", argv[1]);
    }
}

/* ---------- Command: inf ------------------------------------------------- */
static void cmd_inf(void) {
    uint32_t inodes_used, blocks_used;
    nxfs_stats(&inodes_used, &blocks_used);

    term_printf("==================  NexxoN OS - System Info v2.0  ==================\n");
    term_printf(" Kernel build  : %s %s\n", __DATE__, __TIME__);
    term_printf(" Architecture  : x86_64 (64-bit), ring 0 monolithic kernel\n");
    term_printf(" Bootloader    : Multiboot v1 compliant (GRUB)\n");
    term_printf(" Display       : VESA linear framebuffer %ux%u @ %u bpp\n",
                vga_width(), vga_height(), vga_bpp());
    term_printf(" Font          : built-in 8x8 monochrome bitmap (256 glyphs, ISO-8859-2)\n");
    term_printf(" Terminal      : %u cols x %u rows (8x8 cells)\n",
                term_cols(), term_rows());
    term_printf(" Uptime        : %u ms (%u ticks)\n", pit_ms(), pit_ticks());
    {
        rtc_time_t rt; rtc_now(&rt);
        char tbuf[24];
        rtc_format_datetime(&rt, tbuf, sizeof(tbuf));
        term_printf(" Wall clock    : %s (CMOS RTC)\n", tbuf);
    }
    term_printf(" Keyboard      : PS/2 (set 1 + set 2 auto), layout = %s (AltGr+H/A toggle)\n",
                keyboard_get_layout() == KBD_LAYOUT_HU ? "HU" : "EN");
    {
        /* Input/USB diagnostic: lets a bare-metal user confirm whether the
         * firmware streams the USB mouse as PS/2.  Move the mouse, run `inf`:
         * aux bytes climbing => stream works; stuck at 0 => firmware does not
         * emulate the mouse (needs native USB HID). */
        uint32_t mb = 0, mp = 0; bool wheel = false;
        mouse_diag(&mb, &mp, &wheel);
        term_printf(" Mouse         : PS/2 aux, %s, bytes=%u packets=%u\n",
                    wheel ? "wheel" : "3-byte", mb, mp);
        term_printf(" USB stack     : %d controller(s), %s\n",
                    usb_controller_count(),
                    usb_legacy_preserved()
                      ? "BIOS-legacy preserved (input via SMM->PS/2)"
                      : "native (host-driven)");
    }
    if (sysdisk_present()) {
        uint32_t sectors = sysdisk_sector_count();
        term_printf(" Disk          : SATA via AHCI (port %u), %u sectors (%u MiB)\n",
                    ahci_port_index(), sectors, sectors / 2048);
    } else {
        term_printf(" Disk          : (no SATA disk attached)\n");
    }
    term_printf(" FS backend    : %s\n", nxfs_backend_name());
    if (nxfs_mode() == NXFS_MODE_LIVE) {
        term_printf(" FS mode       : LIVE (RAMFS, not persistent)\n");
    } else if (nxfs_is_installed()) {
        term_printf(" FS mode       : INSTALLED (SATA, persistent) - "
                    "stamped by installsys\n");
    } else {
        term_printf(" FS mode       : SATA (persistent, no install signature)\n");
    }
    term_printf(" Filesystem    : NXFS v3   %u inodes used   %u/%u data blocks (4 KiB)\n",
                inodes_used, blocks_used, nxfs_total_blocks());
    term_printf(" Idle indicator: animated |/-\\ (driven by PIT @ 100 Hz)\n");
    term_printf(" PC speaker    : PIT channel 2 (try 'beep 1000 200')\n");
    term_printf("====================================================================\n");
}

/* ---------- Command: sysrep (deep diagnostic report) -------------------- *
 * One-shot, detailed system + hardware report aimed at bare-metal post-
 * mortems (input quirks, USB topology, storage detection, and especially the
 * framebuffer cache type that decides whether rendering crawls).  Everything
 * is printed to the terminal (and mirrors to COM1 via debug_printf) so a
 * single run captures the state needed to diagnose a fault. */
static void rep_line(const char *s) {
    term_printf("%s", s);
    /* Mirror to serial so a captured COM1 log carries the same report. */
    debug_printf("%s", s);
}

static void cmd_sysrep(void) {
    char buf[320];

    rep_line("================  NexxoN OS - sysrep diagnostic  ================\n");
    ksnprintf(buf, sizeof(buf), " Build        : %s %s   uptime=%u ms (%u ticks)\n",
              __DATE__, __TIME__, pit_ms(), pit_ticks());
    rep_line(buf);

    /* ---- CPU ---- */
    {
        uint32_t a, b, c, d;
        char vendor[13];
        __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
        *(uint32_t *)&vendor[0] = b;
        *(uint32_t *)&vendor[4] = d;
        *(uint32_t *)&vendor[8] = c;
        vendor[12] = 0;
        uint32_t sig, feat_d, feat_c;
        __asm__ volatile ("cpuid" : "=a"(sig), "=b"(b), "=c"(feat_c), "=d"(feat_d)
                          : "a"(1) : );
        ksnprintf(buf, sizeof(buf),
                  " CPU          : %s  fam=%u model=%u step=%u  (MTRR=%d PAT=%d SSE2=%d)\n",
                  vendor, (sig >> 8) & 0xF, (sig >> 4) & 0xF, sig & 0xF,
                  (feat_d & (1u << 12)) ? 1 : 0, (feat_d & (1u << 16)) ? 1 : 0,
                  (feat_d & (1u << 26)) ? 1 : 0);
        rep_line(buf);
    }

    /* ---- Framebuffer + cache type (the perf-critical bit) ---- */
    {
        uint32_t fb = (uint32_t)(uintptr_t)vga_framebuffer();
        /* PAT overrides the MTRR, so report the PAT-resolved type when PAT is
         * driving the LFB (the MTRR may still read UC because a BIOS range
         * clobbers it — that's exactly why we use PAT). */
        int pat = vmm_fb_pat_type(fb);
        int eff = (pat >= 0) ? pat : mtrr_effective_type(fb);
        const char *src = (pat >= 0) ? "PAT" : "MTRR";
        const char *et = (eff < 0) ? "unknown"
                       : (eff == MTRR_MEMTYPE_WC) ? "WC (fast)"
                       : (eff == MTRR_MEMTYPE_UC) ? "UC (SLOW! - blits uncached)"
                       : (eff == MTRR_MEMTYPE_WB) ? "WB" : "WT/other";
        ksnprintf(buf, sizeof(buf),
                  " Framebuffer  : phys=0x%08x %ux%u @%ubpp  cache=%s (%s)\n",
                  fb, vga_width(), vga_height(), vga_bpp(), et, src);
        rep_line(buf);

        /* Direct throughput probe: time full-screen fills to the LFB.
         * WC ~ hundreds of MiB/s; UC crawls to a few.  Adaptive: after each
         * frame, stop once we've measured >=120 ms so slow (UC) hardware
         * doesn't sit on a multi-second probe.  Restores the desktop after. */
        volatile uint32_t *p = (volatile uint32_t *)vga_framebuffer();
        uint32_t px = vga_width() * vga_height();
        uint32_t t0 = pit_ms(), dt = 0;
        int frames = 0;
        while (frames < 16) {
            for (uint32_t i = 0; i < px; i++) p[i] = 0x00101010u;
            frames++;
            dt = pit_ms() - t0;
            if (dt >= 120) break;   /* enough signal; don't stall on UC */
        }
        uint32_t mb = (uint32_t)(((uint64_t)px * 4u * (uint32_t)frames) >> 20);
        ksnprintf(buf, sizeof(buf),
                  " FB write     : %u MiB in %u ms  (~%u MiB/s) %s\n",
                  mb, dt, dt ? (mb * 1000u / dt) : 0,
                  (dt && (mb * 1000u / dt) < 60u)
                      ? "<- WRITE-COMBINING NOT ACTIVE (this is the lag)" : "");
        rep_line(buf);
        wm_invalidate_shadow();  /* we wrote the LFB directly above */
        wm_present();            /* repaint the desktop over our probe fills */
    }

    /* ---- MTRR table ---- */
    {
        char mr[512];
        mtrr_report(mr, sizeof(mr));
        rep_line(mr);
    }

    /* ---- Memory ---- */
    {
        uint32_t used = 0, total = 0;
        vmm_frame_stats(&used, &total);
        ksnprintf(buf, sizeof(buf),
                  " VMM frames   : %u/%u KiB used   paging=PSE 4MiB identity\n",
                  used / 1024u, total / 1024u);
        rep_line(buf);
    }

    /* ---- Input ---- */
    {
        uint32_t mb = 0, mp = 0; bool wheel = false;
        mouse_diag(&mb, &mp, &wheel);
        ksnprintf(buf, sizeof(buf),
                  " Keyboard     : PS/2 set1+set2-auto, layout=%s\n"
                  " Mouse        : PS/2 aux %s, bytes=%u packets=%u%s\n",
                  keyboard_get_layout() == KBD_LAYOUT_HU ? "HU" : "EN",
                  wheel ? "wheel" : "3-byte", mb, mp,
                  (mb == 0) ? "  <- firmware not streaming the USB mouse" : "");
        rep_line(buf);
    }

    /* ---- USB ---- */
    {
        ksnprintf(buf, sizeof(buf), " USB stack    : %d controller(s), %s\n",
                  usb_controller_count(),
                  usb_legacy_preserved() ? "BIOS-legacy preserved (SMM->PS/2)"
                                         : "native (host-driven)");
        rep_line(buf);
        int nd = usb_device_count();
        for (int i = 0; i < nd; i++) {
            usb_device_t *d = usb_get_device(i);
            if (!d) continue;
            char dl[120];
            usb_describe(d, dl, sizeof(dl));
            ksnprintf(buf, sizeof(buf), "   usb%d: %s\n", i, dl);
            rep_line(buf);
        }
    }

    /* ---- Storage + FS ---- */
    {
        if (sysdisk_present()) {
            uint32_t s = sysdisk_sector_count();
            ksnprintf(buf, sizeof(buf), " Storage      : %s, %u sectors (%u MiB)\n",
                      sysdisk_kind(), s, s / 2048u);
        } else {
            ksnprintf(buf, sizeof(buf), " Storage      : none bound\n");
        }
        rep_line(buf);
        ksnprintf(buf, sizeof(buf), " AHCI probe   : %s\n", ahci_diag());
        rep_line(buf);
        uint32_t iu = 0, bu = 0;
        nxfs_stats(&iu, &bu);
        ksnprintf(buf, sizeof(buf), " Filesystem   : %s, %s\n",
                  nxfs_backend_name(),
                  nxfs_mode() == NXFS_MODE_LIVE ? "LIVE (RAMFS, volatile)"
                                                : "persistent");
        rep_line(buf);
    }
    rep_line("================================================================\n");
}

/* ---------- Command: usbnative (leave SMM legacy -> native USB) ---------- *
 * Brings the USB stack up natively at runtime so a USB MOUSE (and keyboard,
 * and storage) work the way they do under Windows/Linux: the OS hands the
 * controllers off from firmware SMM and drives the devices itself.  On a board
 * whose BIOS emulates only the legacy keyboard this is the only way to get the
 * mouse.  If something goes wrong, a reboot returns to the SMM keyboard. */
static void cmd_usbnative(void) {
    if (!usb_legacy_preserved()) {
        term_printf("usbnative: USB is already running natively.\n");
        return;
    }
    term_set_color(VGA_YELLOW, VGA_BLACK);
    term_printf("usbnative: taking the USB controllers over from BIOS SMM...\n");
    term_printf("           (input may blink; reboot restores the SMM keyboard)\n");
    term_set_color(VGA_LTGRAY, VGA_BLACK);

    int n = usb_force_native();

    /* Port map first (this is the key diagnostic — photograph THIS line):
     * shows which controller/port has a connection and at what speed, so we
     * can tell direct-attach vs behind-an-external-hub vs nothing-detected. */
    term_printf("usbnative: ports %s\n", usb_takeover_diag());
    term_printf("usbnative: %d USB device(s) enumerated:\n", n);
    int shown = 0;
    for (int i = 0; i < usb_device_count(); i++) {
        usb_device_t *d = usb_get_device(i);
        if (!d) continue;
        char dl[120];
        usb_describe(d, dl, sizeof(dl));
        term_printf("   usb%d: %s\n", i, dl);
        shown++;
    }
    if (shown == 0) {
        term_set_color(VGA_YELLOW, VGA_BLACK);
        term_printf("   no devices enumerated. If a port shows a connection\n");
        term_printf("   above, the device is likely behind an external hub\n");
        term_printf("   (needs xHCI hub routing). Try plugging the mouse\n");
        term_printf("   DIRECTLY into a motherboard USB port and re-run.\n");
        term_set_color(VGA_LTGRAY, VGA_BLACK);
        return;
    }

    /* HID interrupt self-test: poll the freshly-attached HID endpoints for a
     * few seconds and count the reports that arrive.  This needs no keyboard
     * (which the takeover just dropped), so it works even when input is down.
     * Move the mouse during the window: reports>0 => polling works (cursor
     * should track); reports==0 => the interrupt-IN path is the problem. */
    {
        extern void pnp_tick(void);
        uint32_t b0 = 0, p0 = 0; bool wheel = false;
        mouse_diag(&b0, &p0, &wheel);
        term_set_color(VGA_YELLOW, VGA_BLACK);
        term_printf("HID self-test: MOVE THE MOUSE for ~4 seconds...\n");
        term_set_color(VGA_LTGRAY, VGA_BLACK);
        uint32_t t0 = pit_ms();
        while (pit_ms() - t0 < 4000u) {
            pnp_tick();          /* polls usbhid -> xhci_intr_in -> mouse_inject */
            pit_sleep(4);
        }
        uint32_t b1 = 0, p1 = 0;
        mouse_diag(&b1, &p1, &wheel);
        uint32_t got = b1 - b0;
        term_printf("HID self-test: %u report(s) in 4s.\n", got);
        debug_printf("[usbnative] HID self-test: %u report(s)\n", got);
        if (got == 0) {
            term_set_color(VGA_YELLOW, VGA_BLACK);
            term_printf("  -> interrupt-IN delivers nothing; brutal dump:\n");
            term_set_color(VGA_LTGRAY, VGA_BLACK);
            static char bd[2048];
            xhci_brutal_dump(bd, sizeof(bd));
            term_printf("%s", bd);
            debug_printf("%s", bd);   /* mirror to COM1 */
        } else {
            term_printf("  -> reports ARE flowing; the cursor should track now.\n");
        }
    }
}

/* ---------- Command: drivers (drvmgr hardware/driver inventory) ---------- */
static void cmd_drivers(void) {
    static char rep[3072];
    drvmgr_format(rep, sizeof(rep));
    term_printf("%s", rep);
}

/* ---------- Command: mouse-reset (full PS/2 handshake retry) ------------- *
 * The boot mouse init is deliberately gentle so it doesn't kill an SMM-
 * emulated USB mouse.  A real PS/2 mouse that powered up disabled may need
 * the full reset+enable handshake; this runs it on demand. */
static void cmd_mousereset(void) {
    term_printf("mouse-reset: running full PS/2 handshake...\n");
    bool ok = mouse_full_reset();
    term_printf("mouse-reset: %s. Move the mouse, then run `inf`.\n",
                ok ? "device acknowledged (BAT OK)"
                   : "no BAT ack (SMM-emulated or no PS/2 mouse)");
}

/* ---------- Command: time / date ---------------------------------------- */
static void cmd_time(void) {
    rtc_time_t t;
    rtc_now(&t);
    char buf[32];
    rtc_format_time(&t, buf, sizeof(buf));
    term_printf("%s\n", buf);
}

static void cmd_date(void) {
    rtc_time_t t;
    rtc_now(&t);
    char buf[32];
    rtc_format_datetime(&t, buf, sizeof(buf));
    term_printf("%s\n", buf);
}

/* ---------- Command: tasktimer ------------------------------------------ *
 * Sub-commands:
 *      tasktimer at HH:MM:SS <payload...>     - run at absolute today/tomorrow
 *      tasktimer in <N> <unit> <payload...>   - unit ∈ {s, m, h}
 *      tasktimer list                         - show all armed tasks
 *      tasktimer cancel <id>                  - cancel a single task
 *      tasktimer clear                        - cancel everything
 *
 * The payload is everything from argv[start] to end-of-line, re-joined
 * with single spaces.  We deliberately don't try to honour quoting
 * since the shell's tokeniser doesn't either; NXScript syntax doesn't
 * need it for the kinds of one-liners a scheduler typically arms.
 * ---------------------------------------------------------------------- */
static void join_payload(int argc, char **argv, int from, char *out, size_t out_sz) {
    out[0] = 0;
    size_t pos = 0;
    for (int i = from; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (pos + n + 2 >= out_sz) break;
        if (i > from) out[pos++] = ' ';
        memcpy(out + pos, argv[i], n);
        pos += n;
        out[pos] = 0;
    }
}

static bool parse_uint_strict(const char *s, uint32_t *out) {
    if (!s || !*s) return false;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    if (*s) return false;
    *out = v;
    return true;
}

/* Parse "HH:MM:SS" or "HH:MM" into the three components.  Returns true on
 * success and populates the three out-pointers.  Single missing seconds
 * default to 0 so a user can type the more natural "14:30". */
static bool parse_hms(const char *s, uint8_t *h, uint8_t *m, uint8_t *sec) {
    if (!s) return false;
    uint32_t a = 0, b = 0, c = 0;
    int field = 0; bool any = false;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            uint32_t *t = (field == 0) ? &a : (field == 1) ? &b : &c;
            *t = (*t) * 10u + (uint32_t)(*s - '0');
            any = true;
        } else if (*s == ':') {
            if (!any) return false;
            field++;
            any = false;
            if (field > 2) return false;
        } else {
            return false;
        }
        s++;
    }
    if (field < 1) return false;             /* must have at least one colon */
    if (a > 23 || b > 59 || c > 59) return false;
    *h   = (uint8_t)a;
    *m   = (uint8_t)b;
    *sec = (uint8_t)c;
    return true;
}

typedef struct { int printed; } tt_list_ctx_t;
static bool tt_list_cb(int id, uint32_t target_seconds,
                       const char *payload, void *user) {
    tt_list_ctx_t *ctx = (tt_list_ctx_t *)user;
    uint32_t now_secs = rtc_seconds();
    int32_t  delta    = (int32_t)(target_seconds - now_secs);
    char     when[32];
    if (delta < 0) {
        ksnprintf(when, sizeof(when), "(due)");
    } else if (delta < 60) {
        ksnprintf(when, sizeof(when), "in %us", (uint32_t)delta);
    } else if (delta < 3600) {
        ksnprintf(when, sizeof(when), "in %um%02us",
                  (uint32_t)delta / 60u, (uint32_t)delta % 60u);
    } else {
        ksnprintf(when, sizeof(when), "in %uh%02um",
                  (uint32_t)delta / 3600u, ((uint32_t)delta / 60u) % 60u);
    }
    term_printf("  #%-3d  %-12s  %s\n", id, when, payload);
    ctx->printed++;
    return true;
}

static void cmd_tasktimer(int argc, char **argv) {
    if (argc < 2) {
        term_printf("usage: tasktimer at HH:MM:SS <payload>\n");
        term_printf("       tasktimer in  <N> <s|m|h>  <payload>\n");
        term_printf("       tasktimer list\n");
        term_printf("       tasktimer cancel <id>\n");
        term_printf("       tasktimer clear\n");
        term_printf("\nNote: <payload> is NXScript and is run when the time fires.\n");
        term_printf("      e.g.   tasktimer in 30 s print(\"ping\");\n");
        return;
    }

    const char *sub = argv[1];

    if (strcmp(sub, "list") == 0) {
        int n = tasktimer_count();
        if (n == 0) { term_printf("tasktimer: no scheduled tasks.\n"); return; }
        term_printf("tasktimer: %d task%s armed\n", n, n == 1 ? "" : "s");
        tt_list_ctx_t ctx = { 0 };
        tasktimer_iterate(tt_list_cb, &ctx);
        return;
    }

    if (strcmp(sub, "clear") == 0) {
        int n = tasktimer_clear();
        term_printf("tasktimer: cancelled %d task%s.\n", n, n == 1 ? "" : "s");
        return;
    }

    if (strcmp(sub, "cancel") == 0) {
        if (argc < 3) { term_printf("usage: tasktimer cancel <id>\n"); return; }
        uint32_t id = 0;
        if (!parse_uint_strict(argv[2], &id)) {
            term_printf("tasktimer cancel: '%s' is not a task id.\n", argv[2]);
            return;
        }
        int r = tasktimer_cancel((int)id);
        if (r == TT_OK)               term_printf("tasktimer: task #%u cancelled.\n", id);
        else if (r == TT_ERR_NOTFOUND) term_printf("tasktimer: no task #%u.\n", id);
        else                          term_printf("tasktimer cancel: error %d.\n", r);
        return;
    }

    if (strcmp(sub, "at") == 0) {
        if (argc < 4) {
            term_printf("usage: tasktimer at HH:MM:SS <payload>\n");
            return;
        }
        uint8_t h = 0, m = 0, s = 0;
        if (!parse_hms(argv[2], &h, &m, &s)) {
            term_printf("tasktimer at: '%s' is not HH:MM[:SS].\n", argv[2]);
            return;
        }
        char payload[TASKTIMER_PAYLOAD_MAX];
        join_payload(argc, argv, 3, payload, sizeof(payload));
        int r = tasktimer_schedule_at(h, m, s, payload);
        if (r > 0) {
            rtc_time_t now;
            rtc_now(&now);
            const char *rolled = (h * 3600 + m * 60 + s <=
                                  now.hour * 3600 + now.minute * 60 + now.second)
                                  ? " (rolls to tomorrow)" : "";
            term_printf("tasktimer: task #%d armed for %02u:%02u:%02u%s\n",
                        r, h, m, s, rolled);
        } else if (r == TT_ERR_FULL) {
            term_printf("tasktimer: no free slots (max %d).\n", TASKTIMER_MAX_TASKS);
        } else if (r == TT_ERR_PAYLOAD) {
            term_printf("tasktimer: empty or oversize payload.\n");
        } else {
            term_printf("tasktimer at: error %d.\n", r);
        }
        return;
    }

    if (strcmp(sub, "in") == 0) {
        if (argc < 5) {
            term_printf("usage: tasktimer in <N> <s|m|h> <payload>\n");
            return;
        }
        uint32_t n = 0;
        if (!parse_uint_strict(argv[2], &n) || n == 0) {
            term_printf("tasktimer in: '%s' must be a positive integer.\n", argv[2]);
            return;
        }
        const char *unit = argv[3];
        uint32_t mult;
        if      (strcmp(unit, "s") == 0) mult = 1u;
        else if (strcmp(unit, "m") == 0) mult = 60u;
        else if (strcmp(unit, "h") == 0) mult = 3600u;
        else { term_printf("tasktimer in: unit must be s, m, or h.\n"); return; }
        uint32_t offset = n * mult;

        char payload[TASKTIMER_PAYLOAD_MAX];
        join_payload(argc, argv, 4, payload, sizeof(payload));
        int r = tasktimer_schedule_in(offset, payload);
        if (r > 0)                   term_printf("tasktimer: task #%d armed for +%u seconds.\n", r, offset);
        else if (r == TT_ERR_FULL)   term_printf("tasktimer: no free slots (max %d).\n", TASKTIMER_MAX_TASKS);
        else if (r == TT_ERR_PAYLOAD) term_printf("tasktimer: empty or oversize payload.\n");
        else                         term_printf("tasktimer in: error %d.\n", r);
        return;
    }

    term_printf("tasktimer: unknown sub-command '%s' (try 'tasktimer').\n", sub);
}

/* ---------- Command: explorer ------------------------------------------
 * Launch (or refocus) the NexxoN Explorer window.  No arguments - the
 * window inherits its starting directory from the shell's current
 * working directory, and navigation in the explorer updates the same
 * cwd so the shell prompt stays in sync. */
static void cmd_explorer(int argc, char **argv) {
    /* Optional path argument: navigate there before opening so a desktop
     * folder double-click lands inside that folder (explorer inherits the
     * shell cwd on open / refocus). */
    if (argc >= 2 && argv[1] && argv[1][0]) {
        uint32_t ino;
        int r = nxfs_resolve_path(argv[1], &ino);
        if (r != NXFS_OK) r = nxfs_resolve(nxfs_cwd(), argv[1], &ino);
        if (r == NXFS_OK) nxfs_set_cwd(ino);
    }
    if (apps_launch(explorer_open, "explorer")) {
        term_printf("explorer: window opened (cwd in sync with shell)\n");
    } else {
        term_printf("explorer: failed to create window "
                    "(out of slots / framebuffer pool?)\n");
    }
}

/* ---------- Command: addicon -------------------------------------------
 * Register a desktop icon.  Usage:
 *
 *      addicon <label>  <command...>
 *
 * The command is everything from argv[2] onwards, re-joined with single
 * spaces - same convention as `tasktimer`.  Double-clicking the icon
 * later will hand the command to shell_exec() exactly as if the user
 * had typed it themselves. */
static void cmd_addicon(int argc, char **argv) {
    if (argc < 3) {
        term_printf("usage: addicon <label> <command...>\n");
        term_printf("       e.g.  addicon Editor efile readme.txt\n");
        return;
    }
    char cmd_buf[DESKTOP_CMD_MAX];
    join_payload(argc, argv, 2, cmd_buf, sizeof(cmd_buf));
    int r = desktop_add_icon(argv[1], cmd_buf);
    if (r < 0) {
        term_printf("addicon: failed (icon table full, or label / command "
                    "too long)\n");
    } else {
        term_printf("addicon: slot %d  \"%s\" -> %s\n", r, argv[1], cmd_buf);
    }
}

/* ---------- Command: help ------------------------------------------------ */
static void cmd_help(int argc, char **argv) {
    int page = 0;
    if (argc >= 2) {
        const char *p = argv[1];
        if (p[0] == '-') p++;
        page = (int)parse_uint(p);
    }
    if (page == 0) {
        term_printf("NexxoN help (paginated).  Run 'help -<N>' for a page:\n");
        term_printf("  -1  Filesystem    -2  System       -3  Network\n");
        term_printf("  -4  GUI apps      -5  NXScript      -6  Shortcuts\n");
        return;
    }
    if (page == 1) {
    term_printf("Filesystem commands:\n");
    term_printf("  lf                  list current directory\n");
    term_printf("  cd <name>           change directory (.. = parent)\n");
    term_printf("  pwd                 print working directory\n");
    term_printf("  tree                recursive directory tree\n");
    term_printf("  cfile <name>        create file\n");
    term_printf("  dfile <name>        delete file\n");
    term_printf("  efile <name>        launch full-screen editor\n");
    term_printf("  rfile <name>        print file contents\n");
    term_printf("  cat <file>          print file contents\n");
    term_printf("  cdir <name>         create directory\n");
    term_printf("  ddir <name>         delete directory (recursive)\n");
    term_printf("  edir <old> <new>    rename directory\n");
    term_printf("  copy <file>         copy a file to the clipboard\n");
    term_printf("  paste               paste the clipboard into this directory\n");
    term_printf("  grep <pat> [file]   search for a pattern (pipe-aware)\n");
    term_printf("  wc [file]           count lines/words/bytes (pipe-aware)\n");
    term_printf("  tee <file>          copy stdin to a file (pipe-aware)\n");
    term_printf("  trash               open the Recycle Bin\n");
    term_printf("  trash-in <file>     move a file to the Recycle Bin\n");
    term_printf("  trash-empty         empty the Recycle Bin\n");
    return;
    }
    if (page == 2) {
    term_printf("System commands:\n");
    term_printf("  inf                 system / hardware info\n");
    term_printf("  sysrep              deep diagnostic report (CPU/MTRR/USB/FS)\n");
    term_printf("  drivers             driver <-> hardware inventory (drvmgr)\n");
    term_printf("  mounts              list mounted filesystems\n");
    term_printf("  lsusb (ls-usb)      list USB devices\n");
    term_printf("  usbnative           take USB over from BIOS SMM (native mouse)\n");
    term_printf("  mouse-reset         re-run the PS/2 mouse handshake\n");
    term_printf("  time                show wall-clock time (HH:MM:SS)\n");
    term_printf("  date                show full date + time from the CMOS RTC\n");
    term_printf("  kbd [en|hu]         show or switch keyboard layout\n");
    term_printf("  lang [en|hu]        show or switch UI language (persisted)\n");
    term_printf("  theme [name]        show or switch the desktop theme\n");
    term_printf("  whoami              print the current user\n");
    term_printf("  echo <text...>      print arguments\n");
    term_printf("  beep [hz] [ms]      beep the PC speaker\n");
    term_printf("  clear               clear the screen\n");
    term_printf("  cpufreq [0-100]     show or set the CPU performance target\n");
    term_printf("  suspend             enter ACPI S3 sleep\n");
    if (nxfs_mode() == NXFS_MODE_LIVE) {
        term_printf("  installsys          install RAM image to SATA disk (LIVE only)\n");
        term_printf("  reinstallsys        wipe & re-install over an existing SATA install (LIVE only)\n");
    }
    term_printf("  reboot              reboot via 8042 controller\n");
    term_printf("  shutdown            power off (ACPI + VM fallbacks)\n");
    term_printf("  run <file>          execute an NXScript file\n");
    term_printf("  script              enter NXScript REPL\n");
    return;
    }
    if (page == 3) {
    term_printf("Network commands:\n");
    term_printf("  ping <ip>           ICMP ping target IP (4 packets)\n");
    term_printf("  ifconfig [-v]       show network config (-v: NIC register dump)\n");
    term_printf("  dhcp                request a fresh DHCPv4 lease\n");
    term_printf("  ntp [ip]            query an NTP server (port 123)\n");
    term_printf("  browser             open NexxoN Browser\n");
    term_printf("  download <url>      stream URL to disk via sys_download\n");
    term_printf("  wifi                open the Wi-Fi manager\n");
    term_printf("  bluetooth (bt)      open the Bluetooth manager\n");
    return;
    }
    if (page == 4) {
    term_printf("GUI applications:\n");
    term_printf("  taskmgr             open Task Manager\n");
    term_printf("  devmgr              open Device Manager (PCI inventory)\n");
    term_printf("  pong                launch NexxoN Pong\n");
    term_printf("  settings (gephaz / ossettings) open the OS Settings hub\n");
    term_printf("  usermgr             open the User Manager\n");
    term_printf("  explorer            open NexxoN Explorer\n");
    term_printf("  nexsheet [file]     open NexSheet (Excel-style; .xlsx / .csv)\n");
    term_printf("  music (audioplayer) [f] open the audio player\n");
    term_printf("  imgview [file]      open the image viewer\n");
    term_printf("  nexstore (store)    open the app store\n");
    term_printf("  installer           open the system installer\n");
    term_printf("  displays (multimon) open the multi-monitor settings\n");
    term_printf("  usermode            run the Ring-3 demo task (int 0x80 syscalls)\n");
    return;
    }
    if (page == 5) {
    term_printf("  tasktimer ...       schedule a script to run at a wall-clock time\n");
    term_printf("                      try 'tasktimer' (no args) for sub-command help\n");
    term_printf("  addicon <l> <c...>  add a desktop icon  (label, then command)\n");
    term_printf("  explorer            open the NexxoN Explorer file-manager window\n");
    term_printf("  help                this list\n");
    term_printf("NXScript: task num txt numdec bit if loop step pick out print()\n");
    term_printf("  GUI:    CreateWindow(x,y,w,h,\"title\")  WindowColor(color)  TextColor(color)\n");
    term_printf("  Colors: RED GREEN BLUE YELLOW CYAN WHITE BLACK NAVY\n");
    return;
    }
    if (page == 6) {
    term_printf("Shortcuts: Up/Down arrows recall previous commands.  TAB completes.\n");
    term_printf("           Win+Del opens the Task Manager from any context.\n");
    term_printf("           Right-click the desktop or any icon for context menus.\n");
    return;
    }
    term_printf("help: unknown page %d (try -1..-6)\n", page);
}

/* Forward declaration — the pipe helpers call dispatch() before it's defined. */
static void dispatch(const char *original_line, int argc, char **argv);

/* ---------- Pipe / redirect infrastructure --------------------------------
 * g_pipe_in holds piped input for the right-hand side of a pipe chain.
 * Commands that support reading from a pipe check shell_pipe_read().       */
#define SHELL_PIPE_BUF  4096
static char g_pipe_in[SHELL_PIPE_BUF];
static bool g_pipe_in_valid = false;

const char *shell_pipe_read(void) {
    return g_pipe_in_valid ? g_pipe_in : NULL;
}

/* grep: filter lines from piped input (or argv[1]) matching a pattern. */
static void cmd_grep(int argc, char **argv) {
    if (argc < 2) { term_printf("usage: grep <pattern> [text]\n"); return; }
    const char *pat = argv[1];
    const char *src = (argc >= 3) ? argv[2] : shell_pipe_read();
    if (!src) { term_printf("grep: no input (use a pipe)\n"); return; }
    int matches = 0;
    const char *p = src;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (*p == '\n') p++;
        /* Simple substring match. */
        bool found = false;
        for (int i = 0; i <= line_len - (int)strlen(pat); i++) {
            bool ok = true;
            for (int j = 0; pat[j]; j++) {
                if (line_start[i + j] != pat[j]) { ok = false; break; }
            }
            if (ok) { found = true; break; }
        }
        if (found) {
            for (int i = 0; i < line_len; i++) term_putc(line_start[i]);
            term_putc('\n');
            matches++;
        }
    }
    if (matches == 0) term_printf("(no matches)\n");
}

/* wc: count lines/words/chars from piped input. */
static void cmd_wc(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *src = shell_pipe_read();
    if (!src) { term_printf("wc: no piped input\n"); return; }
    int lines = 0, words = 0, chars = 0;
    bool in_word = false;
    for (const char *p = src; *p; p++) {
        chars++;
        if (*p == '\n') { lines++; in_word = false; }
        else if (*p == ' ' || *p == '\t') { in_word = false; }
        else { if (!in_word) { words++; in_word = true; } }
    }
    term_printf("lines=%d  words=%d  chars=%d\n", lines, words, chars);
}

/* cat: print file contents (or piped input if no args). */
static void cmd_cat_file(int argc, char **argv) {
    if (argc < 2) {
        const char *src = shell_pipe_read();
        if (src) { term_printf("%s", src); }
        else     { term_printf("cat: no input\n"); }
        return;
    }
    uint32_t ino;
    if (nxfs_resolve(nxfs_cwd(), argv[1], &ino) != NXFS_OK) {
        term_printf("cat: not found: %s\n", argv[1]);
        return;
    }
    nxfs_inode_t node;
    if (nxfs_read_inode(ino, &node) != NXFS_OK) { term_printf("cat: read error\n"); return; }
    if (node.type != NXFS_TYPE_FILE) { term_printf("cat: not a file\n"); return; }
    static char fbuf[8192];
    uint32_t got = 0;
    if (nxfs_read_file(ino, fbuf, sizeof(fbuf) - 1, &got) == NXFS_OK) {
        fbuf[got] = 0;
        term_printf("%s", fbuf);
        if (got > 0 && fbuf[got-1] != '\n') term_putc('\n');
    } else {
        term_printf("cat: read error\n");
    }
}

/* tee: print input AND write to file. */
static void cmd_tee(int argc, char **argv) {
    const char *src = shell_pipe_read();
    if (!src) { term_printf("tee: no piped input\n"); return; }
    term_printf("%s", src);
    if (argc < 2) return;
    uint32_t ino;
    bool created = false;
    if (nxfs_resolve(nxfs_cwd(), argv[1], &ino) != NXFS_OK) {
        if (nxfs_create_file(nxfs_cwd(), argv[1], &ino) != NXFS_OK) {
            term_printf("tee: cannot create %s\n", argv[1]);
            return;
        }
        created = true;
    }
    (void)created;
    if (nxfs_write_file(ino, src, (uint32_t)strlen(src)) != NXFS_OK)
        term_printf("tee: write error\n");
}

/* ---------- Pipe / redirect pre-processor --------------------------------
 * Scans the command line for | > >> < operators (outside quotes).
 * Returns true if the line was handled as a pipe/redirect chain.
 *
 * Supported forms:
 *   cmd > file         redirect stdout to file (create/overwrite)
 *   cmd >> file        redirect stdout to file (append)
 *   cmd1 | cmd2        pipe: feed cmd1 stdout into cmd2 stdin
 *   cmd1 | cmd2 > file pipe + redirect
 */
static char g_pipe_cap[SHELL_PIPE_BUF];

static bool run_redirected(const char *cmdline, bool append, const char *dest_file) {
    /* Execute cmdline with output captured, then write to dest_file. */
    char mut[SHELL_LINE_MAX];
    char orig[SHELL_LINE_MAX];
    char *av[16];
    strncpy(mut,  cmdline, sizeof(mut)  - 1); mut [sizeof(mut)  - 1] = 0;
    strncpy(orig, cmdline, sizeof(orig) - 1); orig[sizeof(orig) - 1] = 0;
    term_capture_start(g_pipe_cap, sizeof(g_pipe_cap));
    int ac = tokenize(mut, av, 16);
    dispatch(orig, ac, av);
    uint32_t got = term_capture_stop();
    if (got == 0) return true;

    uint32_t ino;
    bool need_create = (nxfs_resolve(nxfs_cwd(), dest_file, &ino) != NXFS_OK);
    if (need_create) {
        if (nxfs_create_file(nxfs_cwd(), dest_file, &ino) != NXFS_OK) {
            term_printf("%s\n", L(STR_SHELL_REDIR_ERR));
            return true;
        }
    }
    if (append) {
        /* Read existing content and append. */
        static char abuf[SHELL_PIPE_BUF];
        uint32_t existing = 0;
        if (!need_create) nxfs_read_file(ino, abuf, sizeof(abuf) - 1, &existing);
        abuf[existing] = 0;
        if (existing + got < sizeof(abuf)) {
            for (uint32_t i = 0; i < got; i++) abuf[existing + i] = g_pipe_cap[i];
            nxfs_write_file(ino, abuf, existing + got);
        } else {
            nxfs_write_file(ino, g_pipe_cap, got);
        }
    } else {
        nxfs_write_file(ino, g_pipe_cap, got);
    }
    return true;
}

static bool try_pipe_redirect(const char *line) {
    int len = (int)strlen(line);
    if (len == 0) return false;

    /* Background job: "cmd &" — launch a GUI app and return immediately. */
    if (line[len - 1] == '&') {
        char bg_line[SHELL_LINE_MAX];
        int bl = 0;
        for (int i = 0; i < len - 1; i++) {
            if (line[i] == ' ' && i == len - 2) break;
            bg_line[bl++] = line[i];
        }
        bg_line[bl] = 0;
        /* Trim trailing spaces. */
        while (bl > 0 && bg_line[bl-1] == ' ') bg_line[--bl] = 0;
        if (bl == 0) return false;
        term_set_color(VGA_CYAN, VGA_BLACK);
        term_printf("[bg] %s\n", bg_line);
        term_set_color(SHELL_TEXT_FG, VGA_BLACK);
        char mut[SHELL_LINE_MAX], orig[SHELL_LINE_MAX];
        char *av[16];
        strncpy(mut,  bg_line, sizeof(mut)  - 1); mut [sizeof(mut)  - 1] = 0;
        strncpy(orig, bg_line, sizeof(orig) - 1); orig[sizeof(orig) - 1] = 0;
        int ac = tokenize(mut, av, 16);
        dispatch(orig, ac, av);
        return true;
    }

    /* Find operators left-to-right (simple, no quoting support). */
    int pipe_pos  = -1;
    int redir_pos = -1;
    bool append   = false;
    for (int i = 0; i < len; i++) {
        if (line[i] == '|' && pipe_pos < 0) { pipe_pos = i; }
        if (line[i] == '>' && redir_pos < 0) {
            redir_pos = i;
            append = (i + 1 < len && line[i + 1] == '>');
        }
    }
    if (pipe_pos < 0 && redir_pos < 0) return false;

    /* Handle redirection without pipe: "cmd > file" */
    if (pipe_pos < 0 && redir_pos >= 0) {
        char left[SHELL_LINE_MAX] = "";
        int out_skip = append ? 2 : 1;
        for (int i = 0; i < redir_pos && i < SHELL_LINE_MAX - 1; i++)
            left[i] = line[i];
        /* Trim trailing spaces from left. */
        int ll = (int)strlen(left);
        while (ll > 0 && left[ll-1] == ' ') left[--ll] = 0;

        /* Extract filename from right of ">". */
        const char *rp = line + redir_pos + out_skip;
        while (*rp == ' ') rp++;
        char fname[64] = "";
        int fi = 0;
        while (*rp && *rp != ' ' && fi < 63) fname[fi++] = *rp++;
        fname[fi] = 0;
        if (!fname[0]) { term_printf("%s\n", L(STR_SHELL_REDIR_ERR)); return true; }
        return run_redirected(left, append, fname);
    }

    /* Handle pipe: "cmd1 | cmd2 [> file]" */
    char left[SHELL_LINE_MAX] = "";
    char right[SHELL_LINE_MAX] = "";
    for (int i = 0; i < pipe_pos && i < SHELL_LINE_MAX - 1; i++) left[i] = line[i];
    int ll = (int)strlen(left);
    while (ll > 0 && left[ll-1] == ' ') left[--ll] = 0;
    const char *rp = line + pipe_pos + 1;
    while (*rp == ' ') rp++;
    int ri = 0;
    while (*rp && ri < SHELL_LINE_MAX - 1) right[ri++] = *rp++;
    right[ri] = 0;
    ll = ri;
    while (ll > 0 && right[ll-1] == ' ') right[--ll] = 0;

    /* Run left side with capture. */
    char lmut[SHELL_LINE_MAX], lorig[SHELL_LINE_MAX];
    char *lav[16];
    strncpy(lmut,  left, sizeof(lmut)  - 1); lmut [sizeof(lmut)  - 1] = 0;
    strncpy(lorig, left, sizeof(lorig) - 1); lorig[sizeof(lorig) - 1] = 0;
    term_capture_start(g_pipe_cap, sizeof(g_pipe_cap));
    int lac = tokenize(lmut, lav, 16);
    dispatch(lorig, lac, lav);
    term_capture_stop();
    strncpy(g_pipe_in, g_pipe_cap, sizeof(g_pipe_in) - 1);
    g_pipe_in[sizeof(g_pipe_in) - 1] = 0;
    g_pipe_in_valid = true;

    /* Check if right side also has a redirect. */
    int right_redir = -1;
    bool right_append = false;
    int rlen = (int)strlen(right);
    for (int i = 0; i < rlen; i++) {
        if (right[i] == '>' && right_redir < 0) {
            right_redir = i;
            right_append = (i + 1 < rlen && right[i+1] == '>');
        }
    }

    if (right_redir >= 0) {
        /* "cmd1 | cmd2 > file" */
        char r2cmd[SHELL_LINE_MAX] = "";
        for (int i = 0; i < right_redir && i < SHELL_LINE_MAX - 1; i++) r2cmd[i] = right[i];
        int r2l = (int)strlen(r2cmd);
        while (r2l > 0 && r2cmd[r2l-1] == ' ') r2cmd[--r2l] = 0;
        const char *fp = right + right_redir + (right_append ? 2 : 1);
        while (*fp == ' ') fp++;
        char fname[64] = "";
        int fi = 0;
        while (*fp && *fp != ' ' && fi < 63) fname[fi++] = *fp++;
        fname[fi] = 0;
        if (fname[0]) run_redirected(r2cmd, right_append, fname);
    } else {
        /* Run right side normally (it sees g_pipe_in). */
        char rmut[SHELL_LINE_MAX], rorig[SHELL_LINE_MAX];
        char *rav[16];
        strncpy(rmut,  right, sizeof(rmut)  - 1); rmut [sizeof(rmut)  - 1] = 0;
        strncpy(rorig, right, sizeof(rorig) - 1); rorig[sizeof(rorig) - 1] = 0;
        int rac = tokenize(rmut, rav, 16);
        dispatch(rorig, rac, rav);
    }

    g_pipe_in_valid = false;
    g_pipe_in[0] = 0;
    return true;
}

/* ---------- Dispatch ----------------------------------------------------- */
static void dispatch(const char *original_line, int argc, char **argv) {
    if (argc == 0) return;
    const char *cmd = argv[0];

    if      (strcmp(cmd, "lf")     == 0) cmd_lf();
    else if (strcmp(cmd, "tree")   == 0) cmd_tree();
    else if (strcmp(cmd, "cfile")  == 0) cmd_cfile(argc, argv);
    else if (strcmp(cmd, "dfile")  == 0) cmd_dfile(argc, argv);
    else if (strcmp(cmd, "efile")  == 0) cmd_efile(argc, argv);
    else if (strcmp(cmd, "rfile")  == 0) cmd_rfile(argc, argv);
    else if (strcmp(cmd, "cdir")   == 0) cmd_cdir (argc, argv);
    else if (strcmp(cmd, "ddir")   == 0) cmd_ddir (argc, argv);
    else if (strcmp(cmd, "edir")   == 0) cmd_edir (argc, argv);
    else if (strcmp(cmd, "cd")     == 0) cmd_cd   (argc, argv);
    else if (strcmp(cmd, "pwd")    == 0) cmd_pwd  ();
    else if (strcmp(cmd, "inf")    == 0) cmd_inf  ();
    else if (strcmp(cmd, "sysrep") == 0) cmd_sysrep();
    else if (strcmp(cmd, "usbnative") == 0) cmd_usbnative();
    else if (strcmp(cmd, "drivers")  == 0) cmd_drivers();
    else if (strcmp(cmd, "mouse-reset") == 0) cmd_mousereset();
    else if (strcmp(cmd, "kbd")    == 0) cmd_kbd  (argc, argv);
    else if (strcmp(cmd, "echo")       == 0) cmd_echo (argc, argv);
    else if (strcmp(cmd, "beep")       == 0) cmd_beep (argc, argv);
    else if (strcmp(cmd, "reboot")     == 0) cmd_reboot();
    else if (strcmp(cmd, "shutdown")   == 0) cmd_shutdown();
    else if (strcmp(cmd, "suspend")    == 0) {
        if (!dialog_yes_no(L(STR_PM_SUSPEND), L(STR_PM_SUSPEND_CONFIRM), NULL))
            return;
        if (!acpi_suspend_s3())
            term_printf("%s\n", L(STR_MSG_FAILED));
    }
    else if (strcmp(cmd, "cpufreq")    == 0) {
        if (argc >= 2) {
            int pct = 0;
            const char *a = argv[1];
            while (*a >= '0' && *a <= '9') pct = pct * 10 + (*a++ - '0');
            cpu_perf_set(pct);
            char desc[32];
            cpu_perf_desc(desc, sizeof(desc));
            term_printf("%s: %d%%  %s\n", L(STR_PM_CPUFREQ), pct, desc);
        } else {
            char desc[32];
            cpu_perf_desc(desc, sizeof(desc));
            int pct = cpu_perf_get();
            if (pct < 0) term_printf("%s: %s\n", L(STR_PM_CPUFREQ), L(STR_MSG_NO_DEVICE));
            else term_printf("%s: %d%%  %s  (usage: cpufreq <0-100>)\n",
                             L(STR_PM_CPUFREQ), pct, desc);
        }
    }
    else if (strcmp(cmd, "run")        == 0) cmd_run(argc, argv);
    else if (strcmp(cmd, "installsys")   == 0) cmd_installsys();
    else if (strcmp(cmd, "reinstallsys") == 0) cmd_reinstallsys();
    else if (strcmp(cmd, "script")       == 0) cmd_script (argc, argv);
    else if (strcmp(cmd, "taskmgr")    == 0) cmd_taskmgr();
    else if (strcmp(cmd, "devmgr")     == 0) cmd_devmgr();
    else if (strcmp(cmd, "pong")       == 0) cmd_pong();
    else if (strcmp(cmd, "ping")       == 0) cmd_ping(argc, argv);
    else if (strcmp(cmd, "ifconfig")   == 0) cmd_ifconfig(argc, argv);
    else if (strcmp(cmd, "lsusb")      == 0) cmd_lsusb();
    else if (strcmp(cmd, "mounts")     == 0) cmd_mounts();
    else if (strcmp(cmd, "ls-usb")     == 0) cmd_ls_usb(argc, argv);
    else if (strcmp(cmd, "utest-write") == 0) cmd_utest_write(argc, argv);
    else if (strcmp(cmd, "dhcp")       == 0) cmd_dhcp();
    else if (strcmp(cmd, "ntp")        == 0) cmd_ntp(argc, argv);
    else if (strcmp(cmd, "copy")       == 0) cmd_copy(argc, argv);
    else if (strcmp(cmd, "fsx")        == 0) cmd_fsx(argc, argv);
    else if (strcmp(cmd, "nxfsck")     == 0) cmd_nxfsck(argc, argv);
    else if (strcmp(cmd, "nxmount")    == 0) {
        /* Diagnostic: 'nxmount sata' force-mounts the installed disk
         * from a LIVE boot (rescue + QEMU test entry for the SATA path). */
        if (argc >= 2 && strcmp(argv[1], "sata") == 0) {
            int rr = nxfs_remount_sata();
            term_printf("nxmount: sata -> %d (%s)\n", rr, nxfs_backend_name());
        } else {
            term_printf("usage: nxmount sata\n");
        }
    }
    else if (strcmp(cmd, "paste")      == 0) cmd_paste();
    else if (strcmp(cmd, "theme")      == 0) cmd_theme(argc, argv);
    else if (strcmp(cmd, "browser")    == 0) (void)apps_launch(browser_open, "browser");
    else if (strcmp(cmd, "nexsheet")   == 0) {
        if (argc >= 2) nexsheet_open_file(argv[1]);
        else           (void)apps_launch(nexsheet_open, "nexsheet");
    }
    else if (strcmp(cmd, "sheet")      == 0) {
        if (argc >= 2) nexsheet_open_file(argv[1]);
        else           (void)apps_launch(nexsheet_open, "nexsheet");
    }
    else if (strcmp(cmd, "settings")   == 0) (void)apps_launch(gephaz_open,  "settings");
    else if (strcmp(cmd, "gephaz")     == 0) (void)apps_launch(gephaz_open,  "gephaz");
    else if (strcmp(cmd, "usermode")   == 0) usermode_demo();
    else if (strcmp(cmd, "ossettings") == 0) (void)apps_launch(gephaz_open,  "ossettings");
    else if (strcmp(cmd, "usermgr")    == 0) (void)apps_launch(usermgr_open, "usermgr");
    else if (strcmp(cmd, "trash")      == 0) (void)apps_launch(trash_open,   "trash");
    else if (strcmp(cmd, "nexstore")   == 0) (void)apps_launch(nexstore_open, "nexstore");
    else if (strcmp(cmd, "store")      == 0) (void)apps_launch(nexstore_open, "store");
    else if (strcmp(cmd, "music")      == 0) {
        if (argc >= 2) audioplayer_open_file(argv[1]);
        else           (void)apps_launch(audioplayer_open, "music");
    }
    else if (strcmp(cmd, "audioplayer") == 0) {
        if (argc >= 2) audioplayer_open_file(argv[1]);
        else           (void)apps_launch(audioplayer_open, "audioplayer");
    }
    else if (strcmp(cmd, "imgview")     == 0) {
        if (argc >= 2) imgview_open_file(argv[1]);
        else           (void)apps_launch(imgview_open, "imgview");
    }
    else if (strcmp(cmd, "installer")   == 0) (void)apps_launch(installer_open, "installer");
    else if (strcmp(cmd, "displays")    == 0) (void)apps_launch(multimon_open, "displays");
    else if (strcmp(cmd, "multimon")    == 0) (void)apps_launch(multimon_open, "multimon");
    else if (strcmp(cmd, "wifi")        == 0) (void)apps_launch(wifi_manager_open,  "wifi");
    else if (strcmp(cmd, "bluetooth")   == 0) (void)apps_launch(bt_manager_open,    "bluetooth");
    else if (strcmp(cmd, "bt")          == 0) (void)apps_launch(bt_manager_open,    "bt");
    else if (strcmp(cmd, "doom")        == 0) (void)apps_launch(doomapp_open,       "doom");
    else if (strcmp(cmd, "lang")       == 0) {
        if (argc >= 2) {
            if (strcmp(argv[1], "hu") == 0) i18n_set_language(LANG_HU);
            else if (strcmp(argv[1], "en") == 0) i18n_set_language(LANG_EN);
            /* Persist to /sys/gephaz.cfg so the language preference
             * survives reboot. */
            gephaz_save_settings();
            term_printf("lang: now %s (saved to /sys/gephaz.cfg)\n",
                        i18n_get_language() == LANG_HU ? "HU" : "EN");
        } else {
            term_printf("lang: current = %s   (usage: lang en|hu)\n",
                        i18n_get_language() == LANG_HU ? "HU" : "EN");
        }
    }
    else if (strcmp(cmd, "whoami")     == 0) {
        term_printf("%s\n", auth_current_user());
    }
    else if (strcmp(cmd, "trash-in")   == 0) {
        if (argc < 2) { term_printf("usage: trash-in <file>\n"); }
        else if (trash_move_in(argv[1]) != 0)
            term_printf("trash-in: failed\n");
        else
            term_printf("trash-in: moved '%s' to Recycle Bin\n", argv[1]);
    }
    else if (strcmp(cmd, "trash-empty") == 0) {
        int n = trash_empty();
        term_printf("trash: emptied %d item(s)\n", n);
    }
    else if (strcmp(cmd, "linux")      == 0) cmd_linux(argc, argv);
    else if (strcmp(cmd, "programs")   == 0) (void)apps_launch(programs_open, "programs");
    else if (strcmp(cmd, "time")       == 0) cmd_time();
    else if (strcmp(cmd, "date")       == 0) cmd_date();
    else if (strcmp(cmd, "tasktimer")  == 0) cmd_tasktimer(argc, argv);
    else if (strcmp(cmd, "addicon")    == 0) cmd_addicon(argc, argv);
    else if (strcmp(cmd, "explorer")   == 0) cmd_explorer(argc, argv);
    else if (strcmp(cmd, "help")       == 0) cmd_help (argc, argv);
    else if (strcmp(cmd, "clear")  == 0) term_clear();
    else if (strcmp(cmd, "grep")   == 0) cmd_grep(argc, argv);
    else if (strcmp(cmd, "wc")     == 0) cmd_wc(argc, argv);
    else if (strcmp(cmd, "cat")    == 0) cmd_cat_file(argc, argv);
    else if (strcmp(cmd, "tee")    == 0) cmd_tee(argc, argv);
    else {
        /* NXScript fallback: try interpreting the whole original line as
         * a script before declaring it unknown.  This catches function
         * calls like  hi();  and  print(2+2);  that didn't match the
         * keyword-prefix heuristic but are perfectly valid NXScript. */
        int r = nxscript_eval(original_line);
        if (r != NX_OK) {
            term_printf("unknown command: '%s'  (try 'help')\n", cmd);
            const char *e = nxscript_last_error();
            if (e && e[0]) {
                vga_color_t fg, bg;
                term_get_color(&fg, &bg);
                term_set_color(VGA_GRAY, bg);
                term_printf("(NXScript: %s)\n", e);
                term_set_color(fg, bg);
            }
        }
    }
}

/* ---------- shell_exec: external command entry point -------------------
 * Wraps the same parse-then-dispatch path the interactive loop runs, but
 * starting from a single string rather than a key-by-key read_line().
 * Used by the desktop environment when an icon is double-clicked. */
void shell_exec(const char *cmdline) {
    if (!cmdline || !cmdline[0]) return;

    /* Two buffers so the tokenizer's in-place NUL insertion doesn't
     * corrupt the original line we hand to the NXScript fallback. */
    static char buf_mut[SHELL_LINE_MAX];
    static char buf_orig[SHELL_LINE_MAX];
    static char *argv_local[16];

    strncpy(buf_mut,  cmdline, sizeof(buf_mut)  - 1);
    buf_mut [sizeof(buf_mut)  - 1] = 0;
    strncpy(buf_orig, cmdline, sizeof(buf_orig) - 1);
    buf_orig[sizeof(buf_orig) - 1] = 0;

    if (nxscript_looks_like(buf_orig)) {
        int r = nxscript_eval(buf_orig);
        if (r != NX_OK) {
            term_set_color(VGA_RED, VGA_BLACK);
            term_printf("NXScript error: %s\n", nxscript_last_error());
            term_set_color(SHELL_TEXT_FG, VGA_BLACK);
        }
        return;
    }

    /* Pipe / redirect pre-processor runs before tokenisation. */
    if (try_pipe_redirect(buf_orig)) return;

    int argc = tokenize(buf_mut, argv_local, 16);
    dispatch(buf_orig, argc, argv_local);
}

/* ---------- Shell main loop --------------------------------------------- */
/* Pumped by the net stack (net_set_idle_hook) during blocking network waits
 * so the desktop keeps rendering and the native USB mouse keeps polling while
 * a command like `ping`/`dhcp`/`ntp` is in flight.  Kept minimal (render +
 * pointer); re-entrancy is guarded inside the net layer. */
static void shell_net_idle_pump(void) {
    wm_tick();
    pnp_tick();
}

void shell_run(void) {
    char line[SHELL_LINE_MAX];
    char line_orig[SHELL_LINE_MAX];
    char *argv[16];

    /* Keep the GUI alive during blocking network I/O (see shell_net_idle_pump). */
    net_set_idle_hook(shell_net_idle_pump);

    /* Arm panic recovery FIRST so any subsystem we call below - including
     * the very first prompt() - can longjmp back to this point on a
     * recoverable fault.  setjmp() returns 0 on the initial entry; the
     * red-modal path returns 1 when the user dismisses the panic.       */
    if (PANIC_ARM_RECOVERY() != 0) {
        term_set_color(VGA_YELLOW, VGA_BLACK);
        term_printf("\n[recovered from a kernel exception - shell continuing]\n");
        term_set_color(SHELL_TEXT_FG, VGA_BLACK);
        /* Re-arm so a subsequent fault is also caught.  We deliberately
         * re-enter via the same setjmp slot - one recovery point is all
         * a single-threaded kernel needs. */
        (void)PANIC_ARM_RECOVERY();
    }

    term_set_color(VGA_GREEN, VGA_BLACK);
    term_printf("\n");
    term_printf(" NexxoN OS v3.0  -  interactive shell ready.\n");
    term_printf(" Type 'help' for the command list, 'inf' for system info.\n");
    term_printf(" Try AltGr+H for the Hungarian layout, AltGr+A for English.\n");
    term_set_color(SHELL_TEXT_FG, VGA_BLACK);

    if (nxfs_mode() == NXFS_MODE_LIVE) {
        term_set_color(VGA_YELLOW, VGA_BLACK);
        term_printf("\n");
        term_printf("================================================================\n");
        term_printf("   LIVE MODE   -   running from a %u-sector RAMFS image.\n",
                    nxfs_total_sectors());
        term_printf("   Files you create will be LOST on reboot unless you run\n");
        term_printf("   `installsys` to commit them to a SATA disk.\n");
        term_printf("================================================================\n");
        term_set_color(SHELL_TEXT_FG, VGA_BLACK);
    }
    term_printf("\n");

    for (;;) {
        /* Desktop-icon double-click queue.  Drain BEFORE we print the
         * prompt so the command output looks like the user typed it
         * themselves: echo the line in cyan, then dispatch.  We loop
         * because a single drain may produce another (e.g., if the
         * command was an NXScript that called another via 'run'). */
        for (;;) {
            const char *pending = desktop_drain_pending_command();
            if (!pending) break;
            /* Defensive copy: shell_exec may stomp the desktop's static
             * buffer if it calls back into the desktop subsystem. */
            char copy[DESKTOP_CMD_MAX];
            strncpy(copy, pending, sizeof(copy) - 1);
            copy[sizeof(copy) - 1] = 0;
            term_set_color(VGA_CYAN, VGA_BLACK);
            term_printf("[desktop] > %s\n", copy);
            term_set_color(SHELL_TEXT_FG, VGA_BLACK);
            wm_refocus_shell();   /* user wants to see the output */
            shell_exec(copy);
        }

        maybe_print_layout_change();
        maybe_update_taskmgr();
        tasktimer_tick();
        prompt();
        int n = read_line(line, sizeof(line));
        if (n <= 0) continue;

        /* Save the *original* line into history before tokenise() inserts NULs. */
        strncpy(line_orig, line, sizeof(line_orig) - 1);
        line_orig[sizeof(line_orig) - 1] = 0;
        hist_push(line_orig);

        /* NXScript fast path: if the line opens with an NXScript keyword
         * (e.g. "task ...", "num x = ...", "print(...)"), parse + execute
         * directly without going through the shell tokenizer. */
        if (nxscript_looks_like(line_orig)) {
            int r = nxscript_eval(line_orig);
            if (r != NX_OK) {
                term_set_color(VGA_RED, VGA_BLACK);
                term_printf("NXScript error: %s\n", nxscript_last_error());
                term_set_color(SHELL_TEXT_FG, VGA_BLACK);
            }
            continue;
        }

        /* Pipe / redirect pre-processor: handles |, >, >>, < before dispatch. */
        if (try_pipe_redirect(line_orig)) continue;

        int argc = tokenize(line, argv, 16);
        dispatch(line_orig, argc, argv);
    }
}
