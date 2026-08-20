/* ============================================================================
 * NexxoN OS - Windows 7-style Task Manager  (v2.0 — TASK 15)
 * ----------------------------------------------------------------------------
 * Six-tab layout mirroring Win 7's design:
 *   Alkalmazasok   running graphical apps (one row per non-protected WM
 *                  window) with state + End Task button
 *   Folyamatok     process matrix: image name, user, CPU%, memory,
 *                  description, Kill Process button
 *   Szolgaltatasok kernel-resident services (taskmgr, scheduler, NXFS,
 *                  WM, audio mixer, ...) with status badges
 *   Teljesitmeny   live line graphs for CPU + physical memory plus
 *                  numerical readouts (Total / Cached / Available / Free)
 *   Halozat        per-NIC bandwidth meter sampled from netif counters
 *   Felhasznalok   active sessions (only one in this build until
 *                  multi-session lands)
 *
 * Above the tab strip sits the menu bar [Fajl | Beallitasok | Nezet |
 * Sugo].  Status bar at the bottom shows "Folyamatok: N | CPU: X% |
 * Fizikai memoria: Y%".  All labels resolve through i18n() so the EN/HU
 * toggle in Gephaz repaints them immediately.
 * ============================================================================ */
#include "apps.h"
#include "window.h"
#include "icons.h"
#include "gfx.h"
#include "theme.h"   /* glass tokens for the app-level Aero sheen */
#include "font.h"
#include "vga.h"
#include "rtc.h"
#include "pit.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "vmm.h"
#include "nxfs.h"
#include "tasktimer.h"
#include "i18n.h"
#include "auth.h"
#include "netif.h"
#include "speaker.h"
#include "boot_info.h"

/* #12: every user-visible string must follow the system language.  TR()
 * returns the Hungarian variant when LANG_HU is active, the English one
 * otherwise -- used for the strings that were previously hard-coded (mostly
 * Hungarian, so they showed Hungarian even in English mode). */
static const char *TR(const char *en, const char *hu) {
    return (i18n_get_language() == LANG_HU) ? hu : en;
}

/* Compute a stable system-memory snapshot from real physical RAM
 * (multiboot mem_lower + mem_upper) plus the static kernel image
 * footprint and the fully-reserved WM framebuffer pool.  This is the
 * value the "Fizikai memoria" graph + status bar quote, in place of
 * the old wm_pool_stats-only number that fluctuated every time the
 * user resized a window (since pool usage IS proportional to window
 * size, but the SYSTEM's view of "RAM used" should not jitter for a
 * single allocator's internal accounting).
 *
 *   total_kib   = boot_info_mem_total_kib (constant from boot)
 *   used_kib    = kernel image + WM pool TOTAL (constant) +
 *                 currently-allocated portion of the pool
 *
 * Reserving the pool's full capacity means "used" only moves when
 * the pool grows past its 6 MiB ceiling (which it cannot today) —
 * so resize-window-resize cycles no longer flicker the readout. */
static void tm_sys_mem_stats(uint32_t *total_kib_out,
                             uint32_t *used_kib_out) {
    uint32_t total_kib = boot_info_mem_total_kib();
    if (total_kib == 0) total_kib = 65536;   /* 64 MiB fallback */

    uint32_t kernel_kib = boot_info_kernel_image_kib();

    uint32_t pool_used = 0, pool_total = 0;
    wm_pool_stats(&pool_used, &pool_total);
    uint32_t vmm_used = 0, vmm_total = 0;
    vmm_frame_stats(&vmm_used, &vmm_total);

    /* The two largest dynamic pools (the WM framebuffer pool, the VMM
     * physical-frame pool) live in the kernel BSS, so they are ALREADY inside
     * kernel_kib but are mostly reserved-yet-unused capacity.  Report the real
     * working set without double-counting: take the kernel image minus that
     * reserved pool capacity (= text+data+small BSS, the truly-resident base),
     * then add back only the part of each pool actually in use.  This now MOVES
     * sensibly as windows open (pool grows) and memory is mapped (vmm grows). */
    uint32_t pool_total_kib = pool_total / 1024u;
    uint32_t vmm_total_kib  = vmm_total  / 1024u;
    uint32_t pool_used_kib  = pool_used  / 1024u;
    uint32_t vmm_used_kib   = vmm_used   / 1024u;
    uint32_t base_kib = kernel_kib;
    if (base_kib > pool_total_kib + vmm_total_kib)
        base_kib -= (pool_total_kib + vmm_total_kib);
    uint32_t used_kib = base_kib + pool_used_kib + vmm_used_kib;
    if (used_kib > total_kib) used_kib = total_kib;

    if (total_kib_out) *total_kib_out = total_kib;
    if (used_kib_out)  *used_kib_out  = used_kib;
}

#define TM_W           600
#define TM_H           440
#define TM_MIN_W       520
#define TM_MIN_H       360

/* Palette — Win 7 Aero-ish but readable on a 32-bit framebuffer. */
#define TM_BG          0xFFF0F0F4
#define TM_HDR_BG      0xFFE8E8EE
#define TM_HDR_FG      0xFF1A1A1A
#define TM_TAB_BG      0xFFD8D8E0
#define TM_TAB_HOT     0xFFFFFFFF
#define TM_TAB_BORDER  0xFF7080A0
#define TM_FG          0xFF101010
#define TM_FG_DIM      0xFF606070
#define TM_FG_HOT      0xFFFFFFFF
#define TM_SEL_BG      0xFF1850C8
#define TM_ROW_H       18
#define TM_STATUS_BG   0xFFE0E0E8
#define TM_GRAPH_BG    0xFF101030
#define TM_GRAPH_GRID  0xFF303060
#define TM_GRAPH_CPU   0xFF40FF80
#define TM_GRAPH_MEM   0xFFFFE040
#define TM_GRAPH_NET   0xFF80C0FF
#define TM_BTN_BG      0xFF1850C8
#define TM_BTN_BG_H    0xFF306FD8
#define TM_BTN_FG      0xFFFFFFFF

#define TM_MENU_H      18
#define TM_TAB_H       24
#define TM_TAB_W       96
#define TM_STATUS_H    20
#define TM_NUM_TABS    6

#define TM_GRAPH_N     96

typedef enum {
    TM_T_APPS = 0,
    TM_T_PROCS,
    TM_T_SVCS,
    TM_T_PERF,
    TM_T_NET,
    TM_T_USERS,
} tm_tab_t;

static window_t *g_tm_win    = NULL;
static tm_tab_t  g_tm_tab    = TM_T_APPS;
static uint32_t  g_tm_last_ms = 0;
static int       g_tm_sel    = -1;
static int       g_tm_scroll = 0;

/* History rings for the Performance tab. */
static uint8_t   g_cpu_hist[TM_GRAPH_N];
static uint8_t   g_mem_hist[TM_GRAPH_N];
static uint8_t   g_net_hist[TM_GRAPH_N];
static uint32_t  g_last_sample_ms = 0;

/* Network: real Kbps derived from netif byte-count deltas. */
static uint32_t  g_net_rx_kbps  = 0;
static uint32_t  g_net_tx_kbps  = 0;
static uint32_t  g_net_prev_rx  = 0;
static uint32_t  g_net_prev_tx  = 0;

static void tm_redraw(void);

/* ---------- Geometry helpers ----------------------------------------- */
static int tm_menu_y   (void) { return 0; }
static int tm_tabs_y   (void) { return TM_MENU_H + 2; }
static int tm_content_y(void) { return TM_MENU_H + 2 + TM_TAB_H + 2; }
static int tm_status_y (draw_target_t *t) { return (int)t->height - TM_STATUS_H; }
/* Tabs sized to the window width so they fit + reflow on resize (no clipping
 * past the edge when the window is shrunk). */
static int tm_tab_w(int content_w) {
    int w = (content_w - 12) / TM_NUM_TABS;
    if (w < 44)  w = 44;
    if (w > 140) w = 140;
    return w;
}
static int tm_tab_x(int content_w, int idx) { return 6 + idx * tm_tab_w(content_w); }

/* ---------- Menu bar -------------------------------------------------- */
static void draw_menu_bar(draw_target_t *t) {
    gfx_fill_rect(t, 0, 0, (int)t->width, TM_MENU_H, TM_HDR_BG);
    /* Glass sheen: brighter top half + a soft depth line at the bottom. */
    gfx_blend_rect(t, 0, 0, (int)t->width, TM_MENU_H / 2, GLASS_GLOSS);
    gfx_blend_rect(t, 0, TM_MENU_H - 1, (int)t->width, 1, GLASS_EDGE_DARK);
    const char *items[4] = {
        i18n_or("taskmgr.menu.file",    "Fajl"),
        i18n_or("taskmgr.menu.options", "Beallitasok"),
        i18n_or("taskmgr.menu.view",    "Nezet"),
        i18n_or("taskmgr.menu.help",    "Sugo"),
    };
    int x = 8;
    for (int i = 0; i < 4; i++) {
        int w = (int)strlen(items[i]) * FONT_GLYPH_W + 12;
        gfx_draw_string(t, x + 6,
                        (TM_MENU_H - FONT_GLYPH_H) / 2,
                        items[i], GLASS_TEXT_DARK, 0x00000000u);
        x += w;
    }
}

/* ---------- Tab strip ------------------------------------------------- */
static void draw_tab_strip(draw_target_t *t) {
    const char *labels[TM_NUM_TABS] = {
        i18n_or("taskmgr.tab.apps",  "Alkalmazasok"),
        i18n_or("taskmgr.tab.procs", "Folyamatok"),
        i18n_or("taskmgr.tab.svc",   "Szolgaltatasok"),
        i18n_or("taskmgr.tab.perf",  "Teljesitmeny"),
        i18n_or("taskmgr.tab.net",   "Halozat"),
        i18n_or("taskmgr.tab.users", "Felhasznalok"),
    };
    int y = tm_tabs_y();
    int tw = tm_tab_w((int)t->width);
    for (int i = 0; i < TM_NUM_TABS; i++) {
        int x = tm_tab_x((int)t->width, i);
        gfx_draw_tab_aero(t, x, y, tw, TM_TAB_H, labels[i], GLASS_ACCENT,
                          i == (int)g_tm_tab, false);
    }
    gfx_blend_rect(t, 0, y + TM_TAB_H, (int)t->width, 1, GLASS_EDGE_DARK);
}

/* ---------- Status bar ------------------------------------------------ */
static void draw_status_bar(draw_target_t *t) {
    int sy = tm_status_y(t);
    gfx_fill_rect(t, 0, sy, (int)t->width, TM_STATUS_H, TM_STATUS_BG);
    gfx_blend_rect(t, 0, sy, (int)t->width, 1, GLASS_EDGE_LIGHT);   /* top gloss */

    int proc_count = 0;
    window_t *wins[WM_MAX_WINDOWS];
    int n = wm_get_windows(wins, WM_MAX_WINDOWS);
    for (int i = 0; i < n; i++) {
        if (wins[i]->in_use) proc_count++;
    }
    uint32_t total_kib = 0, used_kib = 0;
    tm_sys_mem_stats(&total_kib, &used_kib);
    uint32_t mem_pct = total_kib ? (used_kib * 100u / total_kib) : 0;
    uint32_t cpu_pct = g_cpu_hist[TM_GRAPH_N - 1];

    char line[160];
    ksnprintf(line, sizeof(line),
              "%s: %d  |  %s: %u%%  |  %s: %u%%",
              TR("Processes", "Folyamatok"), proc_count,
              TR("CPU usage", "CPU-hasznalat"), cpu_pct,
              TR("Physical memory", "Fizikai memoria"), mem_pct);
    /* ISSUE 2: clip to the window width so resizing the window very
     * narrow truncates the status string instead of bleeding past the
     * right edge of the chrome. */
    gfx_draw_string_clipped(t, 8, sy + (TM_STATUS_H - FONT_GLYPH_H) / 2,
                            (int)t->width - 16, line, TM_HDR_FG,
                            TM_STATUS_BG);
}

/* ---------- Sampling (once per second) -------------------------------- */
static void sample_tick(void) {
    uint32_t now = pit_ms();
    if (g_last_sample_ms == 0) {
        g_last_sample_ms = now;
        netif_byte_counts(&g_net_prev_rx, &g_net_prev_tx);
        return;
    }
    if (now - g_last_sample_ms < 1000u) return;
    uint32_t window_ms = now - g_last_sample_ms;
    if (window_ms == 0) window_ms = 1;
    g_last_sample_ms = now;

    /* Real CPU utilisation: idle-vs-busy timer ticks (pit.c). */
    uint32_t pct = pit_cpu_usage_pct();
    if (pct > 100) pct = 100;

    /* RAM working set. */
    uint32_t total_kib = 0, used_kib = 0;
    tm_sys_mem_stats(&total_kib, &used_kib);
    uint32_t mpct = total_kib ? (used_kib * 100u / total_kib) : 0;
    if (mpct > 100) mpct = 100;

    /* Network: real RX/TX byte deltas -> Kbps (Kbps = bytes*8 / window_ms). */
    uint32_t rxb = 0, txb = 0;
    netif_byte_counts(&rxb, &txb);
    uint32_t drx = rxb - g_net_prev_rx;        /* u32 subtraction wraps safely */
    uint32_t dtx = txb - g_net_prev_tx;
    g_net_prev_rx = rxb;
    g_net_prev_tx = txb;
    g_net_rx_kbps = (drx / window_ms) * 8u + ((drx % window_ms) * 8u) / window_ms;
    g_net_tx_kbps = (dtx / window_ms) * 8u + ((dtx % window_ms) * 8u) / window_ms;
    /* Graph scale: combined Kbps, 0..100 with 2000 Kbps ~ full scale. */
    uint32_t net_pct = (g_net_rx_kbps + g_net_tx_kbps) / 20u;
    if (net_pct > 100) net_pct = 100;

    for (int i = 1; i < TM_GRAPH_N; i++) {
        g_cpu_hist[i - 1] = g_cpu_hist[i];
        g_mem_hist[i - 1] = g_mem_hist[i];
        g_net_hist[i - 1] = g_net_hist[i];
    }
    g_cpu_hist[TM_GRAPH_N - 1] = (uint8_t)pct;
    g_mem_hist[TM_GRAPH_N - 1] = (uint8_t)mpct;
    g_net_hist[TM_GRAPH_N - 1] = (uint8_t)net_pct;
}

/* ---------- Tab 1: Alkalmazasok -------------------------------------- */
static void tab_apps(draw_target_t *t) {
    int top = tm_content_y() + 6;
    {
        char hdr[64];
        ksnprintf(hdr, sizeof(hdr), "%-24s %s",
                  TR("Status", "Allapot"), TR("Application", "Programkod"));
        gfx_draw_string(t, 8, top, hdr, TM_FG_DIM, TM_BG);
    }
    top += FONT_GLYPH_H + 4;

    window_t *wins[WM_MAX_WINDOWS];
    int n = wm_get_windows(wins, WM_MAX_WINDOWS);
    int visible = (tm_status_y(t) - top - 30) / TM_ROW_H;
    if (visible < 1) visible = 1;
    if (g_tm_scroll < 0) g_tm_scroll = 0;
    if (g_tm_scroll > n - visible && n > visible) g_tm_scroll = n - visible;

    int row_top = top;
    for (int i = 0; i < visible && g_tm_scroll + i < n; i++) {
        int idx = g_tm_scroll + i;
        window_t *w = wins[idx];
        if (!w->in_use) continue;
        int ry = row_top + i * TM_ROW_H;
        bool sel = (idx == g_tm_sel);
        if (sel) gfx_fill_rect(t, 0, ry, (int)t->width, TM_ROW_H, TM_SEL_BG);

        const char *state = w->minimized
                            ? TR("[Minimized]", "[Minimalizalt]")
                            : TR("Running", "Fut");
        char line[160];
        ksnprintf(line, sizeof(line), "%-22s %s",
                  state, w->title);
        gfx_draw_string(t, 8, ry + (TM_ROW_H - FONT_GLYPH_H) / 2,
                        line, sel ? TM_FG_HOT : TM_FG,
                        sel ? TM_SEL_BG : TM_BG);
    }

    /* End Task button bottom-right. */
    const char *btn = i18n_or("btn.kill_process", "Feladat befejezese");
    int bw = (int)strlen(btn) * FONT_GLYPH_W + 24;
    int bx = (int)t->width - bw - 12;
    int by = tm_status_y(t) - 30 - 6;
    int mx = mouse_x(), my = mouse_y();
    int abs_bx = g_tm_win->x + WM_BORDER + bx;
    int abs_by = g_tm_win->y + WM_BORDER + WM_TITLE_H + 2 + by;
    bool hot = (mx >= abs_bx && mx < abs_bx + bw &&
                my >= abs_by && my < abs_by + 28);
    uint32_t bg = hot ? TM_BTN_BG_H : TM_BTN_BG;
    gfx_fill_rect(t, bx, by, bw, 28, bg);
    gfx_draw_rect(t, bx, by, bw, 28, 0xFF000000);
    gfx_draw_string(t, bx + 12, by + (28 - FONT_GLYPH_H) / 2,
                    btn, TM_BTN_FG, bg);
}

/* ---------- Tab 2: Folyamatok ---------------------------------------- */
static void tab_procs(draw_target_t *t) {
    int top = tm_content_y() + 6;
    /* Column header. */
    char header[160];
    ksnprintf(header, sizeof(header),
              "%-22s %-10s %5s %10s   %s",
              i18n_or("taskmgr.col.name", "Programkod neve"),
              i18n_or("taskmgr.col.user", "Felhaszn."),
              i18n_or("taskmgr.col.cpu",  "CPU%%"),
              i18n_or("taskmgr.col.mem",  "Memoria"),
              i18n_or("taskmgr.col.desc", "Leiras"));
    gfx_draw_string(t, 8, top, header, TM_FG_DIM, TM_BG);
    top += FONT_GLYPH_H + 4;

    window_t *wins[WM_MAX_WINDOWS];
    int n = wm_get_windows(wins, WM_MAX_WINDOWS);
    const char *user = auth_current_user();
    if (!user || !user[0]) user = "system";

    int visible = (tm_status_y(t) - top - 30) / TM_ROW_H;
    for (int i = 0; i < visible && g_tm_scroll + i < n; i++) {
        int idx = g_tm_scroll + i;
        window_t *w = wins[idx];
        if (!w->in_use) continue;
        int ry = top + i * TM_ROW_H;
        bool sel = (idx == g_tm_sel);
        if (sel) gfx_fill_rect(t, 0, ry, (int)t->width, TM_ROW_H, TM_SEL_BG);

        /* Private Working Set ≈ content framebuffer bytes / KiB. */
        uint32_t mem_kib = w->content_bytes / 1024u;
        /* Per-window CPU% not measured; show "--" so the column lines up. */
        char line[160];
        ksnprintf(line, sizeof(line),
                  "%-22.22s %-10.10s %5s %7u KiB  %s",
                  w->title, user, "--", mem_kib,
                  w->protected ? TR("Shell (protected)", "Shell (vedett)") :
                  w->minimized ? TR("Background", "Hatterben")            :
                                 TR("Running", "Fut, valaszol"));
        gfx_draw_string(t, 8, ry + (TM_ROW_H - FONT_GLYPH_H) / 2,
                        line, sel ? TM_FG_HOT : TM_FG,
                        sel ? TM_SEL_BG : TM_BG);
    }

    /* Folyamat leallitasa button. */
    const char *btn = i18n_or("btn.kill_process", "Folyamat leallitasa");
    int bw = (int)strlen(btn) * FONT_GLYPH_W + 24;
    int bx = (int)t->width - bw - 12;
    int by = tm_status_y(t) - 30 - 6;
    int mx = mouse_x(), my = mouse_y();
    int abs_bx = g_tm_win->x + WM_BORDER + bx;
    int abs_by = g_tm_win->y + WM_BORDER + WM_TITLE_H + 2 + by;
    bool hot = (mx >= abs_bx && mx < abs_bx + bw &&
                my >= abs_by && my < abs_by + 28);
    uint32_t bg = hot ? TM_BTN_BG_H : TM_BTN_BG;
    gfx_fill_rect(t, bx, by, bw, 28, bg);
    gfx_draw_rect(t, bx, by, bw, 28, 0xFF000000);
    gfx_draw_string(t, bx + 12, by + (28 - FONT_GLYPH_H) / 2,
                    btn, TM_BTN_FG, bg);
}

/* ---------- Tab 3: Szolgaltatasok ----------------------------------- */
static void tab_svcs(draw_target_t *t) {
    int y = tm_content_y() + 10;
    {
        char hdr[64];
        ksnprintf(hdr, sizeof(hdr), "%-24s%s",
                  TR("Service", "Szolgaltatas"), TR("State", "Allapot"));
        gfx_draw_string(t, 8, y, hdr, TM_FG_DIM, TM_BG);
    }
    y += FONT_GLYPH_H + 4;

    /* A small known-services table.  In a real init system this would
     * be enumerated; for now we just lay out the kernel-resident pieces
     * we ship and tag each one as running. */
    struct { const char *name; const char *state; } svcs[] = {
        { "wm           Window Manager + compositor",   "Fut" },
        { "term         Console terminal driver",        "Fut" },
        { "nxfs         NexxoN filesystem",              "Fut" },
        { "netif        NIC dispatcher",                 netif_present() ? "Fut" : "Leallt" },
        { "tasktimer    Wall-clock scheduler",           "Fut" },
        { "rtc          CMOS RTC driver",                "Fut" },
        { "speaker      PC speaker / audio mixer",       "Fut" },
        { "ctxmenu      Context menu service",           "Fut" },
        { "ssave        Screensaver daemon",             "Fut" },
        { "auth         User authentication",            "Fut" },
        { "ipc          Inter-process clipboard",        "Allj" },
    };
    int n = (int)(sizeof(svcs) / sizeof(svcs[0]));
    for (int i = 0; i < n; i++) {
        char line[160];
        bool running = (svcs[i].state[0] == 'F');
        ksnprintf(line, sizeof(line), "%-44s %s", svcs[i].name,
                  running ? TR("Running", "Fut") : TR("Stopped", svcs[i].state));
        uint32_t fg = running ? TM_FG : 0xFF905030;
        gfx_draw_string(t, 8, y + i * TM_ROW_H, line, fg, TM_BG);
    }
}

/* ---------- Tab 4: Teljesitmeny -------------------------------------- */
static void draw_graph(draw_target_t *t, int x, int y, int w, int h,
                       const uint8_t *hist, uint32_t color,
                       const char *title) {
    gfx_fill_rect(t, x, y, w, h, TM_GRAPH_BG);
    gfx_draw_rect(t, x, y, w, h, 0xFF000000);
    for (int pct = 25; pct < 100; pct += 25) {
        int gy = y + h - (h * pct) / 100;
        for (int gx = x + 4; gx < x + w; gx += 6) {
            gfx_putpixel(t, gx, gy, TM_GRAPH_GRID);
        }
    }
    int prev_x = -1, prev_y = 0;
    for (int i = 0; i < TM_GRAPH_N; i++) {
        int gx = x + (i * (w - 8)) / TM_GRAPH_N + 4;
        int gy = y + h - 2 - ((int)hist[i] * (h - 8)) / 100;
        if (prev_x >= 0) {
            int dx = gx - prev_x, dy = gy - prev_y;
            int adx = dx > 0 ? dx : -dx;
            int ady = dy > 0 ? dy : -dy;
            int steps = adx > ady ? adx : ady;
            if (steps == 0) steps = 1;
            for (int s = 0; s <= steps; s++) {
                int px = prev_x + (dx * s) / steps;
                int py = prev_y + (dy * s) / steps;
                gfx_putpixel(t, px, py, color);
                if (py + 1 < y + h) gfx_putpixel(t, px, py + 1, color);
            }
        }
        prev_x = gx;
        prev_y = gy;
    }
    char hdr[40];
    ksnprintf(hdr, sizeof(hdr), "%s   %d%%", title, hist[TM_GRAPH_N - 1]);
    gfx_draw_string(t, x + 6, y + 4, hdr, color, TM_GRAPH_BG);
}

static void tab_perf(draw_target_t *t) {
    int top = tm_content_y() + 6;
    int gw = (int)t->width - 24;
    int gh = 110;

    draw_graph(t, 12, top, gw, gh, g_cpu_hist, TM_GRAPH_CPU,
               TR("CPU usage", "CPU-hasznalat"));
    draw_graph(t, 12, top + gh + 12, gw, gh, g_mem_hist, TM_GRAPH_MEM,
               TR("Physical memory", "Fizikai memoria"));

    uint32_t total_kib = 0, used_kib = 0;
    tm_sys_mem_stats(&total_kib, &used_kib);
    uint32_t avail_kib = (total_kib > used_kib) ? total_kib - used_kib : 0;
    int ly = top + 2 * gh + 30;
    char line[160];
    ksnprintf(line, sizeof(line),
              "Total: %u KiB    Used: %u KiB    Available: %u KiB    "
              "Kernel: %u KiB",
              total_kib, used_kib, avail_kib,
              boot_info_kernel_image_kib());
    gfx_draw_string(t, 12, ly, line, TM_FG, TM_BG);
    ksnprintf(line, sizeof(line),
              "Uptime: %u ms    PIT ticks: %u    Sched tasks: %d",
              pit_ms(), pit_ticks(), tasktimer_count());
    gfx_draw_string(t, 12, ly + 14, line, TM_FG, TM_BG);
}

/* ---------- Tab 5: Halozat ------------------------------------------- */
static void tab_net(draw_target_t *t) {
    int y = tm_content_y() + 10;
    if (!netif_present()) {
        gfx_draw_string(t, 12, y,
                        "Nincs aktiv halozati interfesz.", TM_FG_DIM, TM_BG);
        return;
    }
    uint8_t mac[6]; netif_mac(mac);
    char line[160];
    ksnprintf(line, sizeof(line),
              "Interface : %s    MAC: %02x:%02x:%02x:%02x:%02x:%02x",
              netif_name(),
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    gfx_draw_string(t, 12, y, line, TM_FG, TM_BG); y += 16;

    ksnprintf(line, sizeof(line),
              "Aktualis savszelesseg:   Rx %u Kbps    Tx %u Kbps",
              g_net_rx_kbps, g_net_tx_kbps);
    gfx_draw_string(t, 12, y, line, TM_FG, TM_BG); y += 16;

    /* Cumulative totals (real counters). */
    uint32_t rxb = 0, txb = 0;
    netif_byte_counts(&rxb, &txb);
    ksnprintf(line, sizeof(line),
              "Osszesen:   Rx %u KiB    Tx %u KiB",
              rxb / 1024u, txb / 1024u);
    gfx_draw_string(t, 12, y, line, TM_FG_DIM, TM_BG); y += 16;

    /* Real throughput graph from the netif byte-count deltas. */
    int gw = (int)t->width - 24;
    int gh = 110;
    draw_graph(t, 12, y + 8, gw, gh, g_net_hist, TM_GRAPH_NET, "Throughput");
}

/* ---------- Tab 6: Felhasznalok -------------------------------------- */
static void tab_users(draw_target_t *t) {
    int y = tm_content_y() + 10;
    const char *user = auth_current_user();
    auth_role_t role = auth_current_role();
    char line[120];
    {
        char hdr[64];
        ksnprintf(hdr, sizeof(hdr), "%-26s%s",
                  TR("User", "Felhasznalo"), TR("Role", "Szerepkor"));
        gfx_draw_string(t, 12, y, hdr, TM_FG_DIM, TM_BG);
    }
    y += FONT_GLYPH_H + 6;
    ksnprintf(line, sizeof(line), "%-24s %s",
              user && user[0] ? user : "(none)",
              role == ROLE_ADMIN ? TR("admin (privileged)", "admin (privilegizalt)")
                                 : TR("user", "user"));
    gfx_draw_string(t, 12, y, line, TM_FG, TM_BG); y += 16;

    ksnprintf(line, sizeof(line), "%s: %d",
              TR("Total registered users", "Osszes regisztralt felhasznalo"),
              auth_user_count());
    gfx_draw_string(t, 12, y, line, TM_FG_DIM, TM_BG);
}

/* ---------- Click dispatch ------------------------------------------- */
static void kill_selected_window(void) {
    window_t *wins[WM_MAX_WINDOWS];
    int n = wm_get_windows(wins, WM_MAX_WINDOWS);
    if (g_tm_sel < 0 || g_tm_sel >= n) return;
    window_t *target = wins[g_tm_sel];
    if (!target || target->protected || target == g_tm_win) {
        debug_printf("[taskmgr] refused to kill protected/self window\n");
        return;
    }
    debug_printf("[taskmgr] killing window %d \"%s\"\n",
                 target->id, target->title);
    wm_destroy_window(target);
    g_tm_sel = -1;
}

static bool tm_click(window_t *w, int cx, int cy, uint8_t pressed, uint8_t btn) {
    (void)btn;
    if (w != g_tm_win) return false;
    if (!(pressed & MOUSE_BTN_LEFT)) return true;

    /* Tab strip hit-test takes priority over content. */
    int ty = tm_tabs_y();
    int cw = g_tm_win ? (int)g_tm_win->content.width : TM_W;
    int tw = tm_tab_w(cw);
    if (cy >= ty && cy < ty + TM_TAB_H) {
        for (int i = 0; i < TM_NUM_TABS; i++) {
            int tx = tm_tab_x(cw, i);
            if (cx >= tx && cx < tx + tw) {
                g_tm_tab = (tm_tab_t)i;
                g_tm_sel = -1;
                g_tm_scroll = 0;
                tm_redraw();
                return true;
            }
        }
        return true;
    }

    /* Apps + Procs tabs: list row click + End Task button. */
    if (g_tm_tab == TM_T_APPS || g_tm_tab == TM_T_PROCS) {
        int list_top = tm_content_y() + 6 + FONT_GLYPH_H + 4;
        int rel = cy - list_top;
        if (rel >= 0 && cy < tm_status_y(&g_tm_win->content) - 36) {
            int row = rel / TM_ROW_H;
            int idx = g_tm_scroll + row;
            window_t *wins[WM_MAX_WINDOWS];
            int n = wm_get_windows(wins, WM_MAX_WINDOWS);
            if (idx >= 0 && idx < n) g_tm_sel = idx;
            else                     g_tm_sel = -1;
            tm_redraw();
            return true;
        }
        /* End-task button hit-test. */
        const char *btn_lbl = i18n_or("btn.kill_process", "Folyamat leallitasa");
        int bw = (int)strlen(btn_lbl) * FONT_GLYPH_W + 24;
        int bx = (int)w->content.width - bw - 12;
        int by = tm_status_y(&w->content) - 30 - 6;
        if (cx >= bx && cx < bx + bw &&
            cy >= by && cy < by + 28) {
            kill_selected_window();
            tm_redraw();
            return true;
        }
    }
    return true;
}

/* ---------- Composition ---------------------------------------------- */
static void tm_redraw(void) {
    if (!g_tm_win || !g_tm_win->in_use) { g_tm_win = NULL; return; }
    draw_target_t *t = &g_tm_win->content;
    gfx_clear(t, TM_BG);
    draw_menu_bar(t);
    draw_tab_strip(t);
    switch (g_tm_tab) {
        case TM_T_APPS:  tab_apps (t); break;
        case TM_T_PROCS: tab_procs(t); break;
        case TM_T_SVCS:  tab_svcs (t); break;
        case TM_T_PERF:  tab_perf (t); break;
        case TM_T_NET:   tab_net  (t); break;
        case TM_T_USERS: tab_users(t); break;
    }
    draw_status_bar(t);
    wm_mark_dirty();
}

static void tm_resize_cb(window_t *w) { (void)w; tm_redraw(); }
static void tm_destroy_cb(window_t *w) { if (w == g_tm_win) g_tm_win = NULL; }

bool taskmgr_open(void) {
    if (g_tm_win && g_tm_win->in_use) {
        wm_set_focus(g_tm_win);
        tm_redraw();
        return true;
    }
    g_tm_win = wm_create_window((int)vga_width() - TM_W - 10, 10,
                                TM_W, TM_H, L(STR_APP_TASKMGR));
    if (!g_tm_win) return false;
    wm_set_content_click(g_tm_win, tm_click, NULL);
    wm_set_resizable(g_tm_win, true, TM_MIN_W, TM_MIN_H);
    wm_set_resize_cb(g_tm_win, tm_resize_cb);
    wm_set_destroy_cb(g_tm_win, tm_destroy_cb);
    wm_set_icon(g_tm_win, ICON_TASKMGR);
    g_tm_last_ms = 0;
    g_tm_sel = -1;
    g_tm_scroll = 0;
    g_tm_tab = TM_T_APPS;
    tm_redraw();
    return true;
}

void taskmgr_tick(void) {
    /* Always sample so the graphs animate even when the window is
     * closed — opening it then shows real history rather than a flat
     * line. */
    sample_tick();
    if (!g_tm_win) return;
    if (!g_tm_win->in_use) { g_tm_win = NULL; return; }
    uint32_t now = pit_ms();
    if (now - g_tm_last_ms >= 1000u) {
        g_tm_last_ms = now;
        tm_redraw();
    }
}
