/* ============================================================================
 * NexxoN OS - NexxoN Browser  (v2.2 1024x768 Tiled Thin Client)
 * ============================================================================
 * The browser no longer parses HTML/CSS locally.  It acts as a graphical
 * terminal that connects to a remote Node.js/Puppeteer proxy over raw TCP.
 *
 * Protocol (ASCII commands -> proxy, binary frames <- proxy):
 *   Client sends:
 *     Maps <url>    - navigate to URL
 *     BACK          - go back in history
 *     REFRESH       - reload current page
 *     CLICK <x> <y> - mouse click at viewport coordinates
 *     SCROLL <delta>- mouse wheel scroll
 *     KEY <char>    - keyboard character
 *
 *   Server sends:
 *     IMG <size>\n<jpeg_bytes>              - full viewport screenshot
 *     TILE <x> <y> <w> <h> <size>\n<jpeg>  - tiled sub-region
 *     AUD <size>\n<mp3_bytes>               - audio chunk (MP3, 64kbps)
 *
 * v2.2 changes:
 *   - Remote viewport bumped from 800x600 to 1024x768 (matches the
 *     NexxoN screen resolution; user requirement).  The kernel-side
 *     g_br_frame_argb buffer grew from 1.9 MiB to 3 MiB to match.
 *   - Default window grown from 820x520 to 1004x712 so the new
 *     viewport renders nearly 1:1 within the WM chrome (title bar +
 *     borders + toolbar + status) on a 1024x768 screen.
 *   - Bulk-copy fast path in the TCP pump: when mid-payload we memcpy
 *     the prefix straight into the JPEG/MP3 buffer instead of dragging
 *     each byte through the parse-state switch — roughly 10x faster
 *     ingestion on full 1024x768 JPEGs.
 *   - Read chunks doubled to 8 KiB to amortise tcp_recv overhead.
 *   - Pairs with net.c's enlarged 256 KiB TCP receive ring (was 32 KiB)
 *     which was the root cause of the "browser dies on big sites" bug,
 *     plus the duplicate-ACK on out-of-order data so the proxy can
 *     fast-retransmit instead of waiting for the full RTO.
 *
 * v2.1 changes:
 *   - Tile reassembly: TILE frames are decoded into the correct position
 *     in the window framebuffer without needing a full-screen buffer.
 *   - Increased JPEG_CAP to 1 MiB for large enterprise pages.
 *   - Removed the incorrect R/B channel swap (jpeg.c already outputs
 *     correct ARGB on little-endian x86).
 *
 * The top navigation bar (address bar + Back/Refresh buttons) is rendered
 * locally.  Everything below it is the remote viewport.
 * ============================================================================ */
#include "apps.h"
#include "window.h"
#include "icons.h"
#include "gfx.h"
#include "theme.h"
#include "font.h"
#include "vga.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "keyboard.h"
#include "pit.h"
#include "net.h"
#include "e1000.h"
#include "dialogs.h"
#include "notify.h"
#include "i18n.h"
#include "jpeg.h"
#include "audio.h"
#include "nxfs.h"
#include "usermode.h"
#include "sched.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

/* ---- Layout constants ------------------------------------------------
 * Default window is sized to fit on a 1024x768 screen (the boot.asm
 * VBE request) once the 30 px desktop taskbar, 20 px WM title bar and
 * 3 px borders are subtracted: usable = 768 - 30 - 20 - 6 = 712.
 * The 1024x768 remote viewport scales down to the (BR_W - 6) x
 * (BR_H - 26 - toolbar - status) content area via the nearest-neighbour
 * blit, which is acceptable at ~10% vertical compression.  The WM lets
 * the user resize down to BR_MIN_W/H or up to fullscreen on bigger
 * VBE modes.  Window opens at (10, 10) so the right/bottom edges stay
 * on-screen at the default size. */
#define BR_W           1004
#define BR_H           712
#define BR_MIN_W       640
#define BR_MIN_H       400
#define BR_BG          0xFF1A1A2E
#define BR_HDR_BG      0xFF16213E
#define BR_HDR_FG      0xFFFFFFFF
#define BR_ADDR_BG     0xFF0F3460
#define BR_ADDR_FG     0xFFEAEAEA
#define BR_BTN_BG      0xFF533483
#define BR_BTN_BG_H    0xFF6C44A2
#define BR_BTN_FG      0xFFFFFFFF
#define BR_STATUS_BG   0xFF0F3460
#define BR_STATUS_FG   0xFFB0B0B0
#define BR_ADDR_H      28
#define BR_TOOLBAR_H   38
#define BR_STATUS_H    22
#define BR_BTN_W       70
#define BR_URL_MAX     256

/* ---- Proxy connection ------------------------------------------------ */
#define PROXY_PORT             9090
#define PROXY_TIMEOUT          5000
/* Watchdog: if the proxy goes COMPLETELY silent (no TCP payload at all),
 * drop the connection and reconnect.  Crucially this now keys off RX
 * ACTIVITY, not completed frames: the old frame-based check fired in the
 * middle of every big frame / slow page.goto, and each forced reconnect
 * re-sent the navigation — the proxy ended up renavigating in circles
 * ("page closed unexpectedly" churn in its log) and the user saw exactly
 * one page per proxy restart.  Bytes flowing = connection healthy.
 * A separate, much longer guard catches the "bytes flow but no frame
 * ever completes" wedge (half-frame parser stall, proxy capture stuck). */
#define BR_FRAME_WATCHDOG_MS   4000
#define BR_STALL_WATCHDOG_MS   20000
/* Heartbeat: every PING_MS the kernel sends a "PING\n" command line to
 * the proxy.  The proxy answers with a fresh capture; if no response
 * arrives the frame watchdog catches it and reconnects.  Cheap belt-
 * and-braces guard against the "TCP up, page detached" stall. */
#define BR_PING_MS             2000
#define PROXY_IP_DEFAULT "192.168.1.120"
#define BR_CFG_DIR     "sys"
#define BR_CFG_FILE    "browser.cfg"
/* Proxy auto-discovery: the proxy broadcasts "NEXXON-PROXY v1 <tcpport>"
 * on this UDP port every ~2 s; the freshest beacon sender wins over the
 * manual / saved IP at (re)connect time.  Manual config stays the
 * fallback whenever no beacon was heard within the freshness window. */
#define BR_DISCOVERY_PORT      9099
#define BR_DISCOVERY_FRESH_MS  15000

/* ---- Frame buffers ---------------------------------------------------
 * BR_FRAME_W/H must stay in lock-step with the proxy's VIEWPORT_W/H.
 * The 1024x768 viewport produces full-frame JPEGs of ~150-300 KiB and
 * 4x4 (256x192) tiles of ~25-50 KiB; both fit inside BR_JPEG_CAP
 * (1 MiB) and the 256 KiB TCP receive ring buffer (net.c). */
#define BR_JPEG_CAP    (1024 * 1024) /* max compressed JPEG size (1 MiB)   */
#define BR_FRAME_W     1024          /* proxy viewport width               */
#define BR_FRAME_H     768           /* proxy viewport height              */
#define BR_FRAME_PX    (BR_FRAME_W * BR_FRAME_H)
#define BR_MP3_CAP     4096          /* max MP3 chunk size                 */
#define BR_PCM_CAP     (1152 * 2)    /* max PCM samples per MP3 frame      */

/* ---- TCP protocol parser states -------------------------------------- */
typedef enum {
    BR_PARSE_IDLE,
    BR_PARSE_HEADER,
    BR_PARSE_IMG_DATA,
    BR_PARSE_AUD_DATA,
    BR_PARSE_TIL_HEADER,
    BR_PARSE_TIL_DATA,
    BR_PARSE_SYNC,          /* dropping bytes until next valid header magic */
    BR_PARSE_BLOCKED,       /* payload complete, waiting for the decode
                               worker's job slot to free up (pump gated)  */
} br_parse_state_t;

/* ---- Tile metadata (for TILE frames) --------------------------------- */
typedef struct {
    int x, y, w, h, size;
} br_tile_info_t;

/* ---- Pixel format conversion -----------------------------------------
 *
 * The JPEG decoder lays bytes down as B@0 G@1 R@2 A@3 — which on a
 * little-endian x86 reads as a uint32_t of value 0xAARRGGBB.  The
 * native VBE framebuffer layout is reported by multiboot via
 * vga_red_pos / vga_green_pos / vga_blue_pos.  When the framebuffer
 * happens to be XRGB8888 (the common case: R@16, G@8, B@0) the JPEG
 * bytes already match and br_convert_pixel is the identity.  When the
 * framebuffer is BGR-style (R@0, B@16, e.g. some VirtualBox 3D modes)
 * we cheaply swap R↔B at conversion time so the page colours look
 * right instead of dumping the JPEG bytes verbatim and shipping
 * obviously-wrong colours to the user.
 *
 * The conversion is decided once at browser_open(), cached in
 * g_br_needs_swap, and the per-pixel inner loop only pays for a
 * conditional branch + 4 bit-ops in the swap path. */
static bool g_br_needs_swap = false;

static void br_detect_pixfmt(void) {
    /* JPEG decoder produces R-at-16/G-at-8/B-at-0.  We need to swap
     * iff the framebuffer puts blue at 16 and red at 0 (or any other
     * config where the channel masks are flipped relative to the
     * JPEG output). */
    g_br_needs_swap = !vga_is_native_argb();
    debug_printf("[browser] pixel format: R@%u G@%u B@%u → swap=%d\n",
                 vga_red_pos(), vga_green_pos(), vga_blue_pos(),
                 (int)g_br_needs_swap);
}

static inline uint32_t br_convert_pixel(uint32_t argb) {
    if (!g_br_needs_swap) return argb;
    uint32_t a = argb & 0xFF000000u;
    uint32_t r = (argb >> 16) & 0xFFu;
    uint32_t g = (argb >>  8) & 0xFFu;
    uint32_t b = (argb)       & 0xFFu;
    /* Swap R↔B and shift each channel to its detected position. */
    return a |
           ((uint32_t)r << vga_red_pos())   |
           ((uint32_t)g << vga_green_pos()) |
           ((uint32_t)b << vga_blue_pos());
}

/* ---- Static state ---------------------------------------------------- */
static window_t *g_br_win = NULL;
static char      g_br_url[BR_URL_MAX] = "google.com";
static char      g_br_status[160] = "";   /* set via i18n on browser_open */
static int       g_br_addr_caret = 0;
static bool      g_br_addr_focus = true;

/* Navigation history. */
#define BR_HISTORY_MAX 16
static char      g_hist[BR_HISTORY_MAX][BR_URL_MAX];
static int       g_hist_count = 0;
static int       g_hist_pos   = -1;

/* TCP connection. */
static tcp_handle_t g_br_sock = TCP_INVALID;
static uint32_t     g_br_last_frame_ms = 0;
static uint32_t     g_br_last_rx_ms    = 0;   /* ANY payload byte received */
static uint32_t     g_br_last_ping_ms  = 0;
static bool         g_br_connected = false;
static uint32_t     g_br_connect_start_ms = 0;
/* True once a frame has been presented since connect/navigate — while
 * false, br_redraw paints the placeholder background into the viewport;
 * afterwards the viewport pixels belong to the presented frame and the
 * chrome repaint must NOT touch them. */
static bool         g_br_have_frame = false;

/* Protocol parser state. */
static br_parse_state_t g_br_parse_state = BR_PARSE_IDLE;
static char             g_br_hdr_buf[64];
static int              g_br_hdr_len = 0;
static int              g_br_payload_size = 0;
static int              g_br_payload_off = 0;

/* Tile state (for TIL frames). */
static br_tile_info_t   g_br_tile = {0};

/* Frame buffers.
 *
 * JPEG decode moved into a scheduler worker task ("brjpeg") so a full
 * 1024x768 decode no longer freezes the idle loop (mouse, typing) for
 * the duration of every frame.  Ownership is split:
 *
 *   - g_br_jpeg_bufs[2]: ping-pong compressed-payload buffers.  The TCP
 *     parser fills bufs[g_br_fill]; on completion the buffer index is
 *     handed to the worker and the parser flips to the other one.  When
 *     the single job slot is still busy the parser enters
 *     BR_PARSE_BLOCKED and the pump stops consuming TCP (the 256 KiB
 *     ring + advertised window provide backpressure).
 *   - g_br_front: the presented back-buffer (tick blits it to the window
 *     under br_fb_lock).  TILE jobs compose into it under the lock.
 *   - g_br_back: IMG decode target; on success the worker swaps
 *     front/back under the lock, so present never sees a half-decoded
 *     frame and the lock is only ever held for a pointer swap / row
 *     copies - never a decode. */
ALIGNED(16) static uint8_t  g_br_jpeg_bufs[2][BR_JPEG_CAP];
static int                  g_br_fill = 0;
ALIGNED(16) static uint8_t  g_br_frame_a[BR_FRAME_PX * 4];
ALIGNED(16) static uint8_t  g_br_frame_b[BR_FRAME_PX * 4];
static uint8_t             *g_br_front = g_br_frame_a;
static uint8_t             *g_br_back  = g_br_frame_b;
ALIGNED(16) static uint8_t  g_br_mp3_buf[BR_MP3_CAP];
ALIGNED(16) static int16_t  g_br_pcm_buf[BR_PCM_CAP];
static int                  g_br_tiles_in_frame = 0;
/* Set when at least one tile has been decoded since the last frame
 * was presented; FRAMEEND consults this to know whether to issue a
 * "copy backbuffer → window content" pass. */
static volatile bool        g_br_backbuf_dirty = false;

/* ---- Async decode worker ---------------------------------------------
 * One job slot, strictly in-order (the parser stalls while it's busy),
 * results dropped when g_br_gen moved on (navigate / reset / destroy).
 * The front-buffer lock is a yield-spin flag - NOT a kspin, which would
 * disable interrupts; both critical sections are short (pointer swap,
 * tile row copies, present blit). */
#define BR_JOB_IMG   0
#define BR_JOB_TILE  1

typedef struct {
    int             kind;
    int             size;
    int             buf;            /* index into g_br_jpeg_bufs        */
    br_tile_info_t  tile;
    uint32_t        gen;
} br_job_t;

#define BR_JOB_IDLE     0
#define BR_JOB_QUEUED   1
#define BR_JOB_RUNNING  2

static br_job_t          g_br_job;
static volatile int      g_br_job_state = BR_JOB_IDLE;
static volatile uint32_t g_br_gen = 0;
static volatile bool     g_br_present_pending  = false;
static volatile bool     g_br_frameend_pending = false;
static volatile int      g_br_decode_fail_size = 0;
static bool              g_br_worker_spawned   = false;

static volatile uint32_t g_br_fb_lock_flag = 0;
static void br_fb_lock(void) {
    while (__atomic_exchange_n(&g_br_fb_lock_flag, 1, __ATOMIC_ACQUIRE)) {
        if (sched_running()) sched_yield();
        else __asm__ volatile ("pause");
    }
}
static void br_fb_unlock(void) {
    __atomic_store_n(&g_br_fb_lock_flag, 0, __ATOMIC_RELEASE);
}

/* MP3 decoder state. */
static mp3dec_t             g_br_mp3_dec;

/* Proxy IP (runtime, loaded from disk). */
static char      g_proxy_ip_str[32] = PROXY_IP_DEFAULT;
static uint32_t  g_proxy_ip = 0;

/* Auto-discovery state. */
static int       g_br_disc_slot = -1;
static uint32_t  g_br_disc_ip   = 0;
static uint16_t  g_br_disc_port = PROXY_PORT;
static uint32_t  g_br_disc_ms   = 0;
/* What the CURRENT connection attempt targets (discovered or manual). */
static uint32_t  g_br_target_ip   = 0;
static uint16_t  g_br_target_port = PROXY_PORT;

/* ---- Settings persistence -------------------------------------------- */
static uint32_t br_ensure_cfg_dir(void) {
    uint32_t dir_ino = 0;
    if (nxfs_resolve(0, BR_CFG_DIR, &dir_ino) != NXFS_OK) {
        if (nxfs_create_dir(0, BR_CFG_DIR, &dir_ino) != NXFS_OK) return 0;
    }
    return dir_ino;
}

static void br_save_proxy_ip(void) {
    uint32_t dir_ino = br_ensure_cfg_dir();
    if (!dir_ino) return;
    uint32_t cfg_ino = 0;
    if (nxfs_resolve(dir_ino, BR_CFG_FILE, &cfg_ino) != NXFS_OK) {
        if (nxfs_create_file(dir_ino, BR_CFG_FILE, &cfg_ino) != NXFS_OK) return;
    }
    char buf[64];
    int n = ksnprintf(buf, sizeof(buf), "proxy_ip=%s\n", g_proxy_ip_str);
    nxfs_write_file(cfg_ino, buf, (uint32_t)n);
    debug_printf("[browser] saved proxy IP: %s\n", g_proxy_ip_str);
}

static void br_load_proxy_ip(void) {
    uint32_t dir_ino = 0;
    if (nxfs_resolve(0, BR_CFG_DIR, &dir_ino) != NXFS_OK) return;
    uint32_t cfg_ino = 0;
    if (nxfs_resolve(dir_ino, BR_CFG_FILE, &cfg_ino) != NXFS_OK) return;

    char buf[128];
    uint32_t got = 0;
    if (nxfs_read_file(cfg_ino, buf, sizeof(buf) - 1, &got) != NXFS_OK) return;
    buf[got] = 0;

    const char *eq = strchr(buf, '=');
    if (!eq) return;
    const char *ip = eq + 1;
    int len = (int)strlen(ip);
    while (len > 0 && (ip[len - 1] == '\n' || ip[len - 1] == '\r' || ip[len - 1] == ' '))
        len--;
    if (len <= 0 || len >= (int)sizeof(g_proxy_ip_str)) return;

    strncpy(g_proxy_ip_str, ip, (size_t)len);
    g_proxy_ip_str[len] = 0;
    debug_printf("[browser] loaded proxy IP: %s\n", g_proxy_ip_str);
}

static void br_resolve_proxy_ip(void) {
    g_proxy_ip = net_ip_aton(g_proxy_ip_str);
    if (g_proxy_ip == 0) {
        strncpy(g_proxy_ip_str, PROXY_IP_DEFAULT, sizeof(g_proxy_ip_str) - 1);
        g_proxy_ip_str[sizeof(g_proxy_ip_str) - 1] = 0;
        g_proxy_ip = net_ip_aton(g_proxy_ip_str);
    }
}

/* ---- URL helpers ----------------------------------------------------- */
static bool is_url_like(const char *s) {
    if (!s || !*s) return false;
    if (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0)
        return true;
    bool has_dot = false;
    for (const char *p = s; *p; p++) {
        if (*p == ' ') return false;
        if (*p == '.') has_dot = true;
    }
    return has_dot;
}

static void url_ensure_scheme(char *out, const char *in, int cap) {
    if (strncmp(in, "http://", 7) == 0 || strncmp(in, "https://", 8) == 0) {
        strncpy(out, in, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    ksnprintf(out, cap, "https://%s", in);
}

static void url_encode_query(const char *query, char *out, int cap) {
    ksnprintf(out, cap, "https://www.google.com/search?q=");
    int off = (int)strlen(out);
    for (int i = 0; query[i] && off < cap - 2; i++) {
        if (query[i] == ' ') { out[off++] = '+'; out[off] = 0; }
        else { out[off++] = query[i]; out[off] = 0; }
    }
}

/* ---- History --------------------------------------------------------- */
static void br_history_push(const char *url) {
    if (g_hist_pos >= 0 && g_hist_pos < g_hist_count - 1)
        g_hist_count = g_hist_pos + 1;
    if (g_hist_count > 0 && g_hist_pos >= 0 &&
        strcmp(g_hist[g_hist_pos], url) == 0) return;
    if (g_hist_count >= BR_HISTORY_MAX) {
        for (int i = 1; i < BR_HISTORY_MAX; i++) {
            strncpy(g_hist[i - 1], g_hist[i], BR_URL_MAX - 1);
            g_hist[i - 1][BR_URL_MAX - 1] = 0;
        }
        g_hist_count = BR_HISTORY_MAX - 1;
    }
    strncpy(g_hist[g_hist_count], url, BR_URL_MAX - 1);
    g_hist[g_hist_count][BR_URL_MAX - 1] = 0;
    g_hist_pos = g_hist_count;
    g_hist_count++;
}

/* ---- TCP send helper ------------------------------------------------- */
static void br_send(const char *cmd) {
    if (g_br_sock == TCP_INVALID || !g_br_connected) return;
    int len = (int)strlen(cmd);
    tcp_send(g_br_sock, cmd, (uint32_t)len);
    tcp_send(g_br_sock, "\n", 1);
}

/* ---- Protocol parser ------------------------------------------------- */
static void br_parse_reset(void) {
    g_br_parse_state = BR_PARSE_IDLE;
    g_br_hdr_len = 0;
    g_br_payload_size = 0;
    g_br_payload_off = 0;
    g_br_tiles_in_frame = 0;
}

/* Blit a decoded JPEG tile into the window content framebuffer at the
 * correct (x, y) offset.  Handles scaling and clipping. */
static void br_blit_tile_to_window(int tile_x, int tile_y,
                                    int tile_w, int tile_h,
                                    uint8_t *tile_argb,
                                    int src_w, int src_h) {
    if (!g_br_win || !g_br_win->in_use) return;
    draw_target_t *t = &g_br_win->content;

    int dst_w = (int)t->width;
    int dst_h = (int)t->height - BR_TOOLBAR_H - BR_STATUS_H;
    if (dst_w < 1 || dst_h < 1) return;
    if (src_w < 1) src_w = BR_FRAME_W;
    if (src_h < 1) src_h = BR_FRAME_H;

    /* Map tile position from viewport coords to window content coords.
     *
     * CRITICAL: compute each tile's END from the NEXT tile's start
     * boundary, not just `tile_w * dst_w / BR_FRAME_W`.  With integer
     * division the standalone-width formula leaves 1-pixel gaps at
     * certain tile boundaries (e.g. for dst_w=998 the four tiles end
     * at 249, 498, 748, 997 — missing column 498 entirely).  That's
     * the "black cross" the user reported.  Anchoring win_end_x/y to
     * the same floor formula the NEXT tile would use for its win_x/y
     * guarantees the right edge of tile N equals the left edge of
     * tile N+1, no gap, no overlap. */
    int content_area_h = dst_h;
    int win_x     = (tile_x * dst_w) / BR_FRAME_W;
    int win_end_x = ((tile_x + tile_w) * dst_w) / BR_FRAME_W;
    int win_y     = BR_TOOLBAR_H + (tile_y * content_area_h) / BR_FRAME_H;
    int win_end_y = BR_TOOLBAR_H +
                    ((tile_y + tile_h) * content_area_h) / BR_FRAME_H;
    int win_w = win_end_x - win_x;
    int win_h = win_end_y - win_y;

    /* Clamp to content bounds. */
    if (win_x >= dst_w || win_y >= (int)t->height) return;
    if (win_x + win_w < 0 || win_y + win_h < BR_TOOLBAR_H) return;
    if (win_x < 0) { win_w += win_x; win_x = 0; }
    if (win_y < BR_TOOLBAR_H) { win_h -= (BR_TOOLBAR_H - win_y); win_y = BR_TOOLBAR_H; }
    if (win_x + win_w > dst_w) win_w = dst_w - win_x;
    if (win_y + win_h > (int)t->height) win_h = (int)t->height - win_y;
    if (win_w < 1 || win_h < 1) return;

    /* Nearest-neighbour blit for this tile region.
     * Pixel format: JPEG decoder outputs BGRA bytes which on LE x86
     * read as 0xAARRGGBB (native ARGB).  br_convert_pixel handles
     * VBE format mismatch (R/B swap, 5:6:5, etc.). */
    for (int dy = 0; dy < win_h; dy++) {
        int y = win_y + dy;
        if (y >= (int)t->height) break;
        int viewport_y = ((y - BR_TOOLBAR_H) * BR_FRAME_H) / content_area_h;
        int sy = viewport_y - tile_y;
        if (sy < 0) sy = 0;
        if (sy >= src_h) sy = src_h - 1;
        uint32_t *src_row = (uint32_t *)(tile_argb + sy * src_w * 4);
        uint32_t *dst_row = (uint32_t *)(t->fb + (uint32_t)y * t->pitch + win_x * 4);

        for (int dx = 0; dx < win_w; dx++) {
            int viewport_x = ((win_x + dx) * BR_FRAME_W) / dst_w;
            int sx = viewport_x - tile_x;
            if (sx < 0) sx = 0;
            if (sx >= src_w) sx = src_w - 1;
            dst_row[dx] = br_convert_pixel(src_row[sx]);
        }
    }
}

/* Forward decl: br_present_backbuf is shared by both the IMG (full
 * frame) and the FRAMEEND (tile-mode) paths.  Defined further down. */
static void br_present_backbuf(void);

/* Copy a tile's decoded RGBA data into the correct region of the
 * 1024x768 off-screen back-buffer.  This is the asynchronous-buffer
 * step: nothing is written to the user-visible window framebuffer
 * here, so a partially-received frame never appears on screen as a
 * "top-to-bottom redraw" stripe. */
static void br_compose_tile_into_backbuf(int tile_x, int tile_y,
                                         int tile_w, int tile_h,
                                         const uint8_t *tile_rgba,
                                         int src_w, int src_h) {
    if (src_w < 1 || src_h < 1) return;
    if (tile_w < 1 || tile_h < 1) return;
    /* Clip the source region against the BR_FRAME_W/H back-buffer.  */
    int sw = tile_w > src_w ? src_w : tile_w;
    int sh = tile_h > src_h ? src_h : tile_h;
    if (tile_x + sw > BR_FRAME_W) sw = BR_FRAME_W - tile_x;
    if (tile_y + sh > BR_FRAME_H) sh = BR_FRAME_H - tile_y;
    if (sw < 1 || sh < 1) return;

    for (int y = 0; y < sh; y++) {
        const uint32_t *src = (const uint32_t *)(tile_rgba + (uint32_t)y * src_w * 4);
        uint32_t       *dst = (uint32_t *)(g_br_front +
                                           ((uint32_t)(tile_y + y) * BR_FRAME_W +
                                            (uint32_t)tile_x) * 4);
        memcpy(dst, src, (size_t)sw * 4);
    }
}

/* Blit the whole 1024x768 back-buffer onto the live window content
 * framebuffer, scaled down to the window's content area.  Called
 * either when a TILE-mode frame completes (FRAMEEND) or when an IMG
 * full-frame arrives.  Doing the scale + colour-format conversion in
 * one tight memory-bound loop is dramatically faster than the
 * per-tile path because the inner loop stays in L1 / L2 cache. */
static void br_present_backbuf(void) {
    if (!g_br_win || !g_br_win->in_use) return;
    draw_target_t *t = &g_br_win->content;
    int dst_w = (int)t->width;
    int dst_h = (int)t->height - BR_TOOLBAR_H - BR_STATUS_H;
    if (dst_w < 1 || dst_h < 1) return;

    int oy = BR_TOOLBAR_H;
    for (int dy = 0; dy < dst_h; dy++) {
        int y = oy + dy;
        if (y >= (int)t->height) break;
        if (y < 0) continue;
        int sy = (dy * BR_FRAME_H) / dst_h;
        if (sy < 0) sy = 0;
        if (sy >= BR_FRAME_H) sy = BR_FRAME_H - 1;
        const uint32_t *src_row = (const uint32_t *)(g_br_front +
                                                    sy * BR_FRAME_W * 4);
        uint32_t *dst_row = (uint32_t *)(t->fb + (uint32_t)y * t->pitch);

        for (int dx = 0; dx < dst_w; dx++) {
            int sx = (dx * BR_FRAME_W) / dst_w;
            if (sx < 0) sx = 0;
            if (sx >= BR_FRAME_W) sx = BR_FRAME_W - 1;
            dst_row[dx] = br_convert_pixel(src_row[sx]);
        }
    }
    /* NOTE: the frame-watchdog timestamp is set by the CALLERS that
     * present a genuinely NEW frame (IMG / FRAMEEND).  This function is
     * also used to re-blit the existing back-buffer after a resize, and
     * that must not look like fresh proxy traffic to the watchdog. */
    g_br_have_frame    = true;
    g_br_backbuf_dirty = false;
    wm_mark_dirty();
}

/* Tile decode scratch - owned exclusively by the decode worker (or the
 * synchronous fallback path when the scheduler isn't running). */
ALIGNED(16) static uint8_t g_br_tile_scratch[256 * 256 * 4];

/* Execute one decode job.  Runs on the worker task's stack; everything
 * here is plain memory work - no wm calls, no g_br_win access, no
 * status-string writes (the tick translates the result flags),
 * honouring the rule that only the idle loop touches the UI. */
static void br_decode_job(const br_job_t *job) {
    jpeg_info_t ji;
    memset(&ji, 0, sizeof(ji));
    const uint8_t *src = g_br_jpeg_bufs[job->buf];

    if (job->kind == BR_JOB_IMG) {
        if (!jpeg_decode(src, (uint32_t)job->size, g_br_back,
                         BR_FRAME_PX * 4, &ji)) {
            g_br_decode_fail_size = job->size;
            return;
        }
        if (job->gen != g_br_gen) return;    /* user navigated away      */
        br_fb_lock();
        uint8_t *t = g_br_front; g_br_front = g_br_back; g_br_back = t;
        g_br_backbuf_dirty = true;
        br_fb_unlock();
        g_br_present_pending = true;
    } else {
        if (!jpeg_decode(src, (uint32_t)job->size, g_br_tile_scratch,
                         sizeof(g_br_tile_scratch), &ji))
            return;
        if (job->gen != g_br_gen) return;
        br_fb_lock();
        br_compose_tile_into_backbuf(job->tile.x, job->tile.y,
                                     job->tile.w, job->tile.h,
                                     g_br_tile_scratch,
                                     (int)ji.width, (int)ji.height);
        g_br_backbuf_dirty = true;
        br_fb_unlock();
        g_br_tiles_in_frame++;
    }
}

/* Worker task body: poll the single job slot, yield otherwise.  The
 * 40 ms scheduler quantum preempts long decodes back to the idle loop,
 * which is exactly the point - the UI keeps running mid-decode. */
static void br_jpeg_worker(uint32_t arg) {
    (void)arg;
    for (;;) {
        if (g_br_job_state == BR_JOB_QUEUED) {
            g_br_job_state = BR_JOB_RUNNING;
            br_decode_job(&g_br_job);
            __atomic_store_n(&g_br_job_state, BR_JOB_IDLE, __ATOMIC_RELEASE);
        } else {
            sched_yield();
        }
    }
}

/* Hand the completed payload in g_br_jpeg_bufs[g_br_fill] to the worker.
 * Returns false when the job slot is busy (caller stalls the parser).
 * Falls back to an inline decode when the scheduler/worker isn't up -
 * the pre-worker behaviour, still correct, just blocks the tick. */
static bool br_job_submit(int kind) {
    if (!g_br_worker_spawned) {
        br_job_t j;
        j.kind = kind; j.size = g_br_payload_size; j.buf = g_br_fill;
        j.tile = g_br_tile; j.gen = g_br_gen;
        br_decode_job(&j);
        return true;
    }
    if (g_br_job_state != BR_JOB_IDLE) return false;
    g_br_job.kind = kind;
    g_br_job.size = g_br_payload_size;
    g_br_job.buf  = g_br_fill;
    g_br_job.tile = g_br_tile;
    g_br_job.gen  = g_br_gen;
    __atomic_store_n(&g_br_job_state, BR_JOB_QUEUED, __ATOMIC_RELEASE);
    g_br_fill ^= 1;                 /* parser refills the other buffer  */
    return true;
}

/* Payload completed: validate cheaply (a few header/footer bytes), then
 * submit.  On a busy job slot the parser parks in BR_PARSE_BLOCKED with
 * the payload intact; br_tcp_pump retries before consuming more TCP. */
static int g_br_blocked_kind = BR_JOB_IMG;

static void br_payload_complete(int kind) {
    const uint8_t *buf = g_br_jpeg_bufs[g_br_fill];
    if (kind == BR_JOB_IMG) {
        if (g_br_payload_size < 4) {
            ksnprintf(g_br_status, sizeof(g_br_status),
                      L(STR_BR_JPEG_TRUNC), g_br_payload_size);
            br_parse_reset(); return;
        }
        if (buf[0] != 0xFF || buf[1] != 0xD8) {
            ksnprintf(g_br_status, sizeof(g_br_status),
                      L(STR_BR_BAD_JPEG), buf[0], buf[1]);
            br_parse_reset(); return;
        }
        if (buf[g_br_payload_size - 2] != 0xFF ||
            buf[g_br_payload_size - 1] != 0xD9) {
            ksnprintf(g_br_status, sizeof(g_br_status),
                      "%s", L(STR_BR_INCOMPLETE_JPEG));
            br_parse_reset(); return;
        }
        /* Freshness over completeness: a full IMG frame is an independent
         * snapshot, so when the ring already holds a big chunk of the
         * NEXT frame, decoding THIS one only widens the lag.  Drop it (at
         * most every other one, so progress is guaranteed).  Tiles are
         * never skipped - they compose into the same back-buffer. */
        static bool s_skipped_last = false;
        if (!s_skipped_last && tcp_available(g_br_sock) > 8192) {
            s_skipped_last = true;
            br_parse_reset();
            return;
        }
        s_skipped_last = false;
    } else {
        if (g_br_payload_size < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {
            br_parse_reset(); return;
        }
    }
    if (br_job_submit(kind)) {
        br_parse_reset();
        return;
    }
    g_br_blocked_kind = kind;
    g_br_parse_state = BR_PARSE_BLOCKED;
}

/* Called when the proxy emits a FRAMEEND marker.  With the async worker
 * the present is deferred until the in-flight tile (if any) lands; the
 * tick's present stage picks g_br_frameend_pending up. */
static void br_process_frameend(void) {
    if (g_br_worker_spawned && g_br_job_state != BR_JOB_IDLE) {
        g_br_frameend_pending = true;
        return;
    }
    if (g_br_backbuf_dirty) g_br_present_pending = true;
    g_br_tiles_in_frame = 0;
}

static void br_process_aud_frame(void) {
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&g_br_mp3_dec,
                                       g_br_mp3_buf, g_br_payload_off,
                                       g_br_pcm_buf, &info);
    if (samples > 0 && info.channels > 0) {
        audio_play_pcm(g_br_pcm_buf, (uint32_t)samples,
                       info.channels, info.hz);
    }
}

/* Scan a decimal integer from a string, advance pointer. */
static int br_parse_int(const char **pp) {
    const char *p = *pp;
    while (*p == ' ') p++;
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *pp = p;
    return v;
}

/* Enter sync state: drop bytes until we see 'I', 'A', or 'T'.  This
 * recovers alignment if a frame boundary was lost mid-stream. */
static void br_enter_sync(void) {
    g_br_parse_state = BR_PARSE_SYNC;
    g_br_hdr_len = 0;
    g_br_payload_size = 0;
    g_br_payload_off = 0;
}

static void br_parse_byte(uint8_t b) {
    switch (g_br_parse_state) {
    case BR_PARSE_IDLE:
    case BR_PARSE_SYNC:
        /* F = FRAMEEND payload-less marker (tile-mode frame complete) */
        if (b == 'I' || b == 'A' || b == 'T' || b == 'F') {
            g_br_hdr_buf[0] = (char)b;
            g_br_hdr_len = 1;
            g_br_parse_state = BR_PARSE_HEADER;
        }
        break;

    case BR_PARSE_HEADER:
        if (b == '\n') {
            g_br_hdr_buf[g_br_hdr_len] = 0;

            char type = g_br_hdr_buf[0];

            if (type == 'F') {
                /* Protocol: FRAMEEND\n with no payload.  Triggers the
                 * accumulated TILE back-buffer to be presented as one
                 * atomic update to the window content framebuffer. */
                br_process_frameend();
                g_br_parse_state = BR_PARSE_IDLE;
                g_br_hdr_len = 0;
                break;
            } else if (type == 'T') {
                /* Protocol: TILE x y w h size\n
                 * x,y = pixel offset in viewport (0..799, 0..599)
                 * w,h = tile dimensions (typically 200x200, smaller at edges)
                 * size = JPEG byte count */
                const char *p = g_br_hdr_buf;
                /* Skip "TILE" magic.  Accept "TIL" (legacy) too. */
                if (g_br_hdr_len >= 4 &&
                    g_br_hdr_buf[1] == 'I' && g_br_hdr_buf[2] == 'L') {
                    p += (g_br_hdr_buf[3] == 'E') ? 5 : 4; /* TILE=5 TIL=4 */
                } else {
                    br_enter_sync();
                    return;
                }
                g_br_tile.x = br_parse_int(&p);
                g_br_tile.y = br_parse_int(&p);
                g_br_tile.w = br_parse_int(&p);
                g_br_tile.h = br_parse_int(&p);
                int sz = br_parse_int(&p);
                if (sz <= 0 || sz > BR_JPEG_CAP ||
                    g_br_tile.w < 1 || g_br_tile.h < 1 ||
                    g_br_tile.w > 256 || g_br_tile.h > 256) {
                    br_enter_sync();
                    return;
                }
                g_br_payload_size = sz;
                g_br_payload_off = 0;
                g_br_parse_state = BR_PARSE_TIL_DATA;
            } else if (type == 'I') {
                /* Parse "IMG size" */
                const char *num = g_br_hdr_buf + 4;
                int sz = br_parse_int(&num);
                if (sz <= 0 || sz > BR_JPEG_CAP) { br_enter_sync(); return; }
                g_br_payload_size = sz;
                g_br_payload_off = 0;
                g_br_parse_state = BR_PARSE_IMG_DATA;
            } else if (type == 'A') {
                /* Parse "AUD size" */
                const char *num = g_br_hdr_buf + 4;
                int sz = br_parse_int(&num);
                if (sz <= 0 || sz > BR_MP3_CAP) { br_enter_sync(); return; }
                g_br_payload_size = sz;
                g_br_payload_off = 0;
                g_br_parse_state = BR_PARSE_AUD_DATA;
            } else {
                br_enter_sync();
            }
        } else {
            if (g_br_hdr_len < (int)sizeof(g_br_hdr_buf) - 1)
                g_br_hdr_buf[g_br_hdr_len++] = (char)b;
            else
                br_enter_sync();
        }
        break;

    case BR_PARSE_IMG_DATA:
        if (g_br_payload_off < BR_JPEG_CAP)
            g_br_jpeg_bufs[g_br_fill][g_br_payload_off++] = b;
        if (g_br_payload_off >= g_br_payload_size)
            br_payload_complete(BR_JOB_IMG);   /* may park in BLOCKED */
        break;

    case BR_PARSE_TIL_DATA:
        if (g_br_payload_off < BR_JPEG_CAP)
            g_br_jpeg_bufs[g_br_fill][g_br_payload_off++] = b;
        if (g_br_payload_off >= g_br_payload_size)
            br_payload_complete(BR_JOB_TILE);  /* may park in BLOCKED */
        break;

    case BR_PARSE_AUD_DATA:
        if (g_br_payload_off < BR_MP3_CAP)
            g_br_mp3_buf[g_br_payload_off++] = b;
        if (g_br_payload_off >= g_br_payload_size) {
            br_process_aud_frame();
            br_parse_reset();
        }
        break;

    case BR_PARSE_BLOCKED:
        /* Unreachable: the pump never feeds bytes while blocked. */
        break;
    }
}

/* Fast bulk-copy of a contiguous TCP chunk into the active payload
 * buffer.  When the parser is in *_DATA state and we have a run of
 * bytes to consume, we can skip the byte-by-byte switch dispatch
 * entirely and memcpy the prefix.  Returns the number of bytes
 * consumed by the fast path (caller advances past them). */
static int br_bulk_consume(const uint8_t *src, int n) {
    if (n <= 0) return 0;
    int need = g_br_payload_size - g_br_payload_off;
    if (need <= 0) return 0;
    int take = (n < need) ? n : need;
    uint8_t *dst = NULL;
    int cap = 0;
    switch (g_br_parse_state) {
    case BR_PARSE_IMG_DATA:
    case BR_PARSE_TIL_DATA:
        dst = g_br_jpeg_bufs[g_br_fill];
        cap = BR_JPEG_CAP;
        break;
    case BR_PARSE_AUD_DATA:
        dst = g_br_mp3_buf;
        cap = BR_MP3_CAP;
        break;
    default:
        return 0;
    }
    int free_cap = cap - g_br_payload_off;
    int copy = (take < free_cap) ? take : free_cap;
    if (copy > 0) {
        memcpy(dst + g_br_payload_off, src, (size_t)copy);
    }
    g_br_payload_off += take;   /* advance even when capped so we sync */
    if (g_br_payload_off >= g_br_payload_size) {
        switch (g_br_parse_state) {
        case BR_PARSE_IMG_DATA:
            br_payload_complete(BR_JOB_IMG);   /* may park in BLOCKED */
            break;
        case BR_PARSE_TIL_DATA:
            br_payload_complete(BR_JOB_TILE);  /* may park in BLOCKED */
            break;
        case BR_PARSE_AUD_DATA:
            br_process_aud_frame();
            br_parse_reset();
            break;
        default: break;
        }
    }
    return take;
}

/* ---- TCP receive pump (optimised: larger chunks, fewer round-trips) -- */
/* Carry state: when the parser parks in BR_PARSE_BLOCKED (decode worker
 * busy) mid-chunk, the unconsumed remainder stays here until the next
 * pump - tcp_recv has already dequeued those bytes.  Cleared on stream
 * reset so a navigation never replays stale bytes. */
static uint8_t g_br_chunk[8192];
static int     g_br_chunk_len = 0;
static int     g_br_chunk_pos = 0;

static void br_tcp_pump(void) {
    if (g_br_sock == TCP_INVALID || !g_br_connected) return;

    /* Read in 8 KiB chunks — large enough to amortise syscall overhead
     * but small enough to fit on the kernel stack.  The outer loop
     * keeps draining until the TCP ring buffer is empty (tcp_recv
     * returns -1) so a single browser_tick can absorb the full
     * 256 KiB ring even when the proxy bursts a complete frame.
     *
     * Storage is static (not stack) so deep call chains from the WM
     * or panic recovery don't blow the 64 KiB kernel stack.  Single-
     * threaded kernel + cooperative scheduler => one shared buffer
     * is safe. */
    /* Blocked on the worker's job slot?  Retry the hand-off first; if
     * it is still busy, consume nothing this tick (the TCP ring + the
     * advertised window provide backpressure). */
    if (g_br_parse_state == BR_PARSE_BLOCKED) {
        if (!br_job_submit(g_br_blocked_kind)) return;
        br_parse_reset();
    }

    /* Budget the pump.  The proxy streams continuously (15 fps), so "drain
     * until empty" never terminates while frames keep landing faster than we
     * parse them — the shell idle loop (UI, clock, input dispatch) lived
     * inside this while-loop for the stream's whole lifetime and the desktop
     * froze hard the moment the first connect succeeded.  A few chunks +
     * a wall-clock cap per tick keeps ingest fast (24 KiB/pass at idle-loop
     * rate is megabytes/s) while the compositor stays alive; the 256 KiB TCP
     * ring + the advertised-window backpressure absorb the difference. */
    int      chunks_left = 3;
    uint32_t t0 = pit_ms();
    for (;;) {
        if (g_br_chunk_pos >= g_br_chunk_len) {
            if (chunks_left-- <= 0) break;
            int n = tcp_recv(g_br_sock, g_br_chunk, sizeof(g_br_chunk), 0);
            if (n <= 0) break;
            g_br_chunk_len = n;
            g_br_chunk_pos = 0;
            g_br_last_rx_ms = pit_ms();   /* connection demonstrably alive */
        }
        while (g_br_chunk_pos < g_br_chunk_len) {
            /* Bulk path: when we're mid-payload, memcpy the prefix
             * straight into the payload buffer instead of looping
             * through the parse switch byte-by-byte.  Cuts ~10x off
             * the JPEG ingestion cost on big tiles. */
            int taken = br_bulk_consume(g_br_chunk + g_br_chunk_pos,
                                        g_br_chunk_len - g_br_chunk_pos);
            if (taken > 0) {
                g_br_chunk_pos += taken;
            } else {
                br_parse_byte(g_br_chunk[g_br_chunk_pos++]);
            }
            if (g_br_parse_state == BR_PARSE_BLOCKED) return;
        }
        if (pit_ms() - t0 > 30) break;     /* ingest ran long: yield to UI */
    }
}

/* ---- Navigation ------------------------------------------------------
 *
 * Every URL change resets the in-progress packet parser AND sends an
 * out-of-band RESET line to the proxy so the Windows-side capture loop
 * abandons any half-rendered screenshot.  Without this, a slow first
 * page would keep its tiles arriving long after the second page's
 * MAPS command, which historically left the proxy in an "executing
 * old goto" state that needed a server restart.
 *
 * We do NOT tear down the TCP socket on every navigation — the
 * connection is recycled (cleanly closed only when the proxy IP
 * changes or the user types in a new server).  This keeps the
 * thin-client snappy on quick URL hops. */
static void br_local_reset_stream(void) {
    br_parse_reset();
    /* Invalidate any in-flight / queued decode: the worker checks the
     * generation after decoding and drops stale results, so a slow
     * frame from the PREVIOUS page can never present over the new one. */
    g_br_gen++;
    g_br_present_pending  = false;
    g_br_frameend_pending = false;
    g_br_chunk_len = g_br_chunk_pos = 0;   /* drop carried stream bytes */
    /* Reset the watchdog timers so the navigation gets a full
     * watchdog-window worth of grace before the kernel decides the
     * proxy has hung. */
    g_br_last_frame_ms = pit_ms();
    g_br_last_rx_ms    = pit_ms();
    g_br_last_ping_ms  = pit_ms();
}

static void br_navigate(const char *input) {
    char url[BR_URL_MAX];
    if (is_url_like(input)) {
        url_ensure_scheme(url, input, sizeof(url));
    } else {
        url_encode_query(input, url, sizeof(url));
    }

    strncpy(g_br_url, url, BR_URL_MAX - 1);
    g_br_url[BR_URL_MAX - 1] = 0;
    g_br_addr_caret = (int)strlen(g_br_url);

    /* Reset the local parser BEFORE we send the new command - any
     * still-in-flight TILE / IMG bytes from the previous page would
     * otherwise corrupt the new frame stream.  Then send an explicit
     * RESET to the proxy so its capture loop also abandons the prior
     * page's pending screenshot instead of streaming us a stale
     * frame that arrives AFTER the new Maps command. */
    br_local_reset_stream();
    br_send("RESET");

    char cmd[BR_URL_MAX + 8];
    ksnprintf(cmd, sizeof(cmd), "Maps %s", url);
    br_send(cmd);

    br_history_push(url);
    ksnprintf(g_br_status, sizeof(g_br_status), L(STR_BR_NAVIGATING), url);
}

static void br_back(void) {
    if (g_hist_pos <= 0) return;
    g_hist_pos--;
    strncpy(g_br_url, g_hist[g_hist_pos], BR_URL_MAX - 1);
    g_br_url[BR_URL_MAX - 1] = 0;
    g_br_addr_caret = (int)strlen(g_br_url);
    br_local_reset_stream();
    br_send("BACK");
    ksnprintf(g_br_status, sizeof(g_br_status), L(STR_BR_BACK_TO), g_br_url);
}

static void br_refresh(void) {
    br_local_reset_stream();
    br_send("REFRESH");
    ksnprintf(g_br_status, sizeof(g_br_status), L(STR_BR_REFRESHING), g_br_url);
}

static void br_send_remote_keycode(int c) {
    if (!g_br_connected) return;
    char cmd[32];
    switch (c) {
    case '\n':
    case '\r': br_send("KEY enter"); return;
    case '\b': br_send("KEY backspace"); return;
    case '\t': br_send("KEY tab"); return;
    case KEY_DEL: br_send("KEY delete"); return;
    case KEY_LEFT: br_send("KEY arrowleft"); return;
    case KEY_RIGHT: br_send("KEY arrowright"); return;
    case KEY_UP: br_send("KEY arrowup"); return;
    case KEY_DOWN: br_send("KEY arrowdown"); return;
    case KEY_HOME: br_send("KEY home"); return;
    case KEY_END: br_send("KEY end"); return;
    case KEY_PGUP: br_send("KEY pageup"); return;
    case KEY_PGDN: br_send("KEY pagedown"); return;
    default:
        if (c >= 0x20 && c < 0x7F) {
            ksnprintf(cmd, sizeof(cmd), "KEY %c", (char)c);
            br_send(cmd);
        }
        return;
    }
}

/* ---- Redraw ---------------------------------------------------------- */
/* Repaints the CHROME (toolbar + status bar) and, only while no frame has
 * been presented yet, the placeholder viewport.  This used to gfx_clear
 * the whole content every browser_tick — wiping the freshly presented
 * page within milliseconds, so the viewport showed the background colour
 * almost permanently ("rendering broken" on top of the connection bug). */
static void br_redraw(void) {
    if (!g_br_win || !g_br_win->in_use) { g_br_win = NULL; return; }
    draw_target_t *t = &g_br_win->content;
    if (!g_br_have_frame) {
        int vh = (int)t->height - BR_TOOLBAR_H - BR_STATUS_H;
        if (vh > 0)
            gfx_fill_rect(t, 0, BR_TOOLBAR_H, (int)t->width, vh, BR_BG);
    }

    gfx_fill_rect(t, 0, 0, (int)t->width, BR_TOOLBAR_H, BR_HDR_BG);
    gfx_blend_rect(t, 0, 0, (int)t->width, BR_TOOLBAR_H / 2, 0x16FFFFFFu);
    gfx_blend_rect(t, 0, BR_TOOLBAR_H - 1, (int)t->width, 1, GLASS_EDGE_DARK);

    int addr_y = (BR_TOOLBAR_H - BR_ADDR_H) / 2;
    int nav_btn_w = 30;

    int back_x = 8;
    bool back_enabled = (g_hist_pos > 0);
    gfx_draw_button_aero(t, back_x, addr_y, nav_btn_w, BR_ADDR_H, "<",
                         back_enabled ? BR_BTN_BG : 0xFF303050, false);

    int rf_x = back_x + nav_btn_w + 4;
    gfx_draw_button_aero(t, rf_x, addr_y, nav_btn_w, BR_ADDR_H, "R",
                         BR_BTN_BG, false);

    int addr_x = rf_x + nav_btn_w + 8;
    int set_w = 24;
    int addr_w = (int)t->width - addr_x - 8 - BR_BTN_W - 8 - set_w - 8;
    if (addr_w < 80) addr_w = 80;
    gfx_fill_rect(t, addr_x, addr_y, addr_w, BR_ADDR_H, BR_ADDR_BG);
    gfx_draw_rect(t, addr_x, addr_y, addr_w, BR_ADDR_H, 0xFF000000);

    int max_chars = (addr_w - 8) / FONT_GLYPH_W;
    int n = (int)strlen(g_br_url);
    int start = 0;
    if (n > max_chars) start = n - max_chars;
    gfx_draw_string(t, addr_x + 4,
                    addr_y + (BR_ADDR_H - FONT_GLYPH_H) / 2,
                    g_br_url + start, BR_ADDR_FG, BR_ADDR_BG);

    int caret_x = addr_x + 4 + (g_br_addr_caret - start) * FONT_GLYPH_W;
    if (caret_x >= addr_x + 4 && caret_x < addr_x + addr_w - 4) {
        gfx_fill_rect(t, caret_x, addr_y + 3, 1, BR_ADDR_H - 6, BR_ADDR_FG);
    }

    int go_x = addr_x + addr_w + 8;
    gfx_draw_button_aero(t, go_x, addr_y, BR_BTN_W, BR_ADDR_H, L(STR_BR_GO),
                         BR_BTN_BG, false);

    int set_x = go_x + BR_BTN_W + 4;
    gfx_draw_button_aero(t, set_x, addr_y, set_w, BR_ADDR_H, "S",
                         BR_BTN_BG, false);

    int pane_bot = (int)t->height - BR_STATUS_H;
    gfx_fill_rect(t, 0, pane_bot, (int)t->width, BR_STATUS_H, BR_STATUS_BG);

    char conn_status[80];
    if (g_br_connected) {
        uint32_t elapsed = pit_ms() - g_br_last_frame_ms;
        ksnprintf(conn_status, sizeof(conn_status), L(STR_BR_CONNECTED_SINCE), elapsed);
    } else {
        ksnprintf(conn_status, sizeof(conn_status), "%s", L(STR_BR_DISCONNECTED));
    }

    char full_status[200];
    ksnprintf(full_status, sizeof(full_status), "%s | %s", g_br_status, conn_status);
    int sx = ((int)t->width - 16) / FONT_GLYPH_W;
    if ((int)strlen(full_status) > sx) full_status[sx] = 0;
    gfx_draw_string(t, 8, pane_bot + (BR_STATUS_H - FONT_GLYPH_H) / 2,
                    full_status, BR_STATUS_FG, BR_STATUS_BG);

    wm_mark_dirty();
}

/* ---- Click handler --------------------------------------------------- */
static bool br_click(window_t *w, int cx, int cy, uint8_t pressed, uint8_t btn) {
    (void)btn;
    if (w != g_br_win) return false;
    if (!(pressed & MOUSE_BTN_LEFT)) return true;

    int addr_y = (BR_TOOLBAR_H - BR_ADDR_H) / 2;
    int nav_btn_w = 30;
    int back_x = 8;
    int rf_x = back_x + nav_btn_w + 4;
    int addr_x = rf_x + nav_btn_w + 8;
    int set_w = 24;
    int addr_w = (int)w->content.width - addr_x - 8 - BR_BTN_W - 8 - set_w - 8;
    int go_x = addr_x + addr_w + 8;
    int set_x = go_x + BR_BTN_W + 4;

    if (cy >= addr_y && cy < addr_y + BR_ADDR_H) {
        if (cx >= back_x && cx < back_x + nav_btn_w) {
            br_back(); br_redraw(); return true;
        }
        if (cx >= rf_x && cx < rf_x + nav_btn_w) {
            br_refresh(); br_redraw(); return true;
        }
        if (cx >= addr_x && cx < addr_x + addr_w) {
            g_br_addr_focus = true;
            wm_set_focus(g_br_win);
            br_redraw(); return true;
        }
        if (cx >= go_x && cx < go_x + BR_BTN_W) {
            br_navigate(g_br_url); br_redraw(); return true;
        }
        if (cx >= set_x && cx < set_x + set_w) {
            char new_ip[32];
            if (dialog_input(L(STR_BR_PROXY_SETTINGS), L(STR_BR_PROXY_IP_PROMPT),
                             g_proxy_ip_str, new_ip, sizeof(new_ip))) {
                bool valid = true;
                int dots = 0;
                for (const char *p = new_ip; *p && valid; p++) {
                    if (*p == '.') dots++;
                    else if (*p < '0' || *p > '9') valid = false;
                }
                if (valid && dots == 3) {
                    strncpy(g_proxy_ip_str, new_ip, sizeof(g_proxy_ip_str) - 1);
                    g_proxy_ip_str[sizeof(g_proxy_ip_str) - 1] = 0;
                    br_resolve_proxy_ip();
                    br_save_proxy_ip();
                    if (g_br_sock != TCP_INVALID) {
                        tcp_close(g_br_sock);
                        g_br_sock = TCP_INVALID;
                    }
                    g_br_connected = false;
                    ksnprintf(g_br_status, sizeof(g_br_status),
                              L(STR_BR_PROXY_CHANGED), g_proxy_ip_str);
                } else {
                    ksnprintf(g_br_status, sizeof(g_br_status), "%s",
                              L(STR_BR_INVALID_IP));
                }
            }
            br_redraw(); return true;
        }
        return true;
    }

    if (cy >= BR_TOOLBAR_H && g_br_connected) {
        int vy = cy - BR_TOOLBAR_H;
        int vh = (int)w->content.height - BR_TOOLBAR_H - BR_STATUS_H;
        int px = (cx * BR_FRAME_W) / (int)w->content.width;
        int py = (vy * BR_FRAME_H) / vh;
        if (px < 0) px = 0;
        if (px >= BR_FRAME_W) px = BR_FRAME_W - 1;
        if (py < 0) py = 0;
        if (py >= BR_FRAME_H) py = BR_FRAME_H - 1;

        char cmd[32];
        ksnprintf(cmd, sizeof(cmd), "CLICK %d %d", px, py);
        br_send(cmd);
        return true;
    }

    return true;
}

/* ---- Scroll handler -------------------------------------------------- */
static bool br_scroll(window_t *w, int dz) {
    (void)w;
    if (!g_br_connected) return true;
    char cmd[32];
    /* mouse.c notes: negative dz = wheel down. Puppeteer expects positive
     * deltaY for scroll down, so invert sign here. */
    ksnprintf(cmd, sizeof(cmd), "SCROLL %d", -dz * 100);
    br_send(cmd);
    return true;
}

/* ---- Key handler ----------------------------------------------------- */
static bool br_key_handler(window_t *w, int c) {
    if (w != g_br_win || !w->in_use) return false;
    if (c == 0) return true;

    if (!g_br_addr_focus) {
        br_send_remote_keycode(c);
        return true;
    }

    if (c == '\n' || c == '\r') {
        if (g_br_url[0]) br_navigate(g_br_url);
        g_br_addr_focus = false;
        br_redraw(); return true;
    }
    if (c == '\b') {
        int n = (int)strlen(g_br_url);
        if (g_br_addr_caret > 0 && n > 0) {
            memmove(g_br_url + g_br_addr_caret - 1,
                    g_br_url + g_br_addr_caret,
                    (size_t)(n - g_br_addr_caret + 1));
            g_br_addr_caret--;
        }
        br_redraw(); return true;
    }
    if (c == KEY_DEL) {
        int n = (int)strlen(g_br_url);
        if (g_br_addr_caret < n) {
            memmove(g_br_url + g_br_addr_caret,
                    g_br_url + g_br_addr_caret + 1,
                    (size_t)(n - g_br_addr_caret));
        }
        br_redraw(); return true;
    }
    if (c == KEY_LEFT)  { if (g_br_addr_caret > 0) g_br_addr_caret--; br_redraw(); return true; }
    if (c == KEY_RIGHT) { int n = (int)strlen(g_br_url); if (g_br_addr_caret < n) g_br_addr_caret++; br_redraw(); return true; }
    if (c == KEY_HOME)  { g_br_addr_caret = 0; br_redraw(); return true; }
    if (c == KEY_END)   { g_br_addr_caret = (int)strlen(g_br_url); br_redraw(); return true; }
    if (c >= 0x20 && c < 0x7F) {
        int n = (int)strlen(g_br_url);
        if (n + 1 < BR_URL_MAX) {
            memmove(g_br_url + g_br_addr_caret + 1,
                    g_br_url + g_br_addr_caret,
                    (size_t)(n - g_br_addr_caret + 1));
            g_br_url[g_br_addr_caret] = (char)c;
            g_br_addr_caret++;
        }
        br_redraw(); return true;
    }
    return true;
}

/* ---- Resize / destroy callbacks -------------------------------------- */
static void br_resize_cb(window_t *w) {
    (void)w;
    /* The content framebuffer was reallocated: re-blit the existing
     * back-buffer into the new geometry so the page doesn't vanish until
     * the next proxy frame. */
    if (g_br_have_frame) {
        br_fb_lock();
        br_present_backbuf();
        br_fb_unlock();
    }
    br_redraw();
}
static void br_destroy_cb(window_t *w) {
    if (w == g_br_win) {
        if (g_br_sock != TCP_INVALID) { tcp_close(g_br_sock); g_br_sock = TCP_INVALID; }
        g_br_connected = false;
        g_br_win = NULL;
        /* Drop any in-flight decode result; the worker task itself stays
         * parked (it just yields) for the next browser session. */
        g_br_gen++;
        g_br_present_pending  = false;
        g_br_frameend_pending = false;
    }
}

/* ---- Present stage (tick context only) --------------------------------
 * Translates the worker's result flags into UI work: status strings for
 * failed decodes, and the actual back-buffer -> window blit.  IMG frames
 * can present even while the NEXT decode runs (it targets g_br_back);
 * tile-mode presents wait for FRAMEEND + an idle job slot. */
static void br_present_stage(void) {
    if (g_br_decode_fail_size) {
        ksnprintf(g_br_status, sizeof(g_br_status),
                  L(STR_BR_JPEG_FAIL), g_br_decode_fail_size);
        g_br_decode_fail_size = 0;
    }
    if (g_br_frameend_pending && g_br_job_state == BR_JOB_IDLE) {
        g_br_frameend_pending = false;
        if (g_br_backbuf_dirty) g_br_present_pending = true;
        g_br_tiles_in_frame = 0;
    }
    if (g_br_present_pending) {
        g_br_present_pending = false;
        br_fb_lock();
        br_present_backbuf();
        br_fb_unlock();
        g_br_last_frame_ms = pit_ms();   /* a COMPLETE fresh frame landed */
    }
}

/* ---- Proxy auto-discovery (UDP beacon listener) ----------------------- */
static void br_discovery_poll(void) {
    if (g_br_disc_slot < 0) return;
    char buf[64];
    uint32_t sip = 0;
    uint16_t sport = 0;
    int n = net_udp_poll(g_br_disc_slot, buf, sizeof(buf) - 1, &sip, &sport);
    if (n <= 0 || sip == 0) return;
    buf[n] = 0;
    if (strncmp(buf, "NEXXON-PROXY ", 13) != 0) return;
    /* "NEXXON-PROXY <version> <tcpport>" - version token is skipped so
     * future proxies can extend the beacon without breaking us. */
    const char *p = buf + 13;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    int port = 0;
    while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0');
    if (port <= 0 || port > 65535) port = PROXY_PORT;

    bool is_new = (sip != g_br_disc_ip || (uint16_t)port != g_br_disc_port);
    g_br_disc_ip   = sip;
    g_br_disc_port = (uint16_t)port;
    g_br_disc_ms   = pit_ms();
    if (is_new) {
        char ip_str[20];
        net_ip_ntoa(sip, ip_str, sizeof(ip_str));
        debug_printf("[browser] proxy beacon: %s:%d\n", ip_str, port);
        char msg[64];
        ksnprintf(msg, sizeof(msg), L(STR_BR_PROXY_FOUND), ip_str);
        notify_post(NOTIFY_INFO, L(STR_BR_PROXY_TITLE), msg);
        /* Mid-retry against a dead manual IP: drop the pending connect
         * so the next tick dials the discovered proxy instead. */
        if (!g_br_connected && g_br_sock != TCP_INVALID) {
            tcp_close(g_br_sock);
            g_br_sock = TCP_INVALID;
        }
    }
}

/* ---- Connection management (non-blocking) ---------------------------- */
static void br_connect_start(void) {
    if (g_br_connected) return;
    if (!net_link_up()) return;

    /* Freshly-heard beacon wins; manual / saved IP is the fallback. */
    if (g_br_disc_ip && pit_ms() - g_br_disc_ms < BR_DISCOVERY_FRESH_MS) {
        g_br_target_ip   = g_br_disc_ip;
        g_br_target_port = g_br_disc_port;
    } else {
        br_resolve_proxy_ip();
        g_br_target_ip   = g_proxy_ip;
        g_br_target_port = PROXY_PORT;
    }

    char ip_str[20];
    net_ip_ntoa(g_br_target_ip, ip_str, sizeof(ip_str));
    debug_printf("[browser] connecting to proxy at %s:%d ...\n",
                 ip_str, g_br_target_port);
    g_br_sock = tcp_connect(g_br_target_ip, g_br_target_port, 0);
    if (g_br_sock == TCP_INVALID) {
        ksnprintf(g_br_status, sizeof(g_br_status),
                  L(STR_BR_CONN_FAILED), ip_str, g_br_target_port);
        return;
    }
    g_br_connect_start_ms = pit_ms();
    ksnprintf(g_br_status, sizeof(g_br_status), L(STR_BR_CONNECTING),
              ip_str, g_br_target_port);
}

static void br_connect_poll(void) {
    if (g_br_sock == TCP_INVALID) return;

    if (tcp_is_connected(g_br_sock)) {
        g_br_connected = true;
        br_parse_reset();
        mp3dec_init(&g_br_mp3_dec);
        /* Arm the silence watchdog from "now": if the proxy never sends a
         * single byte, tier-1 fires BR_FRAME_WATCHDOG_MS later. */
        g_br_last_rx_ms = pit_ms();
        ksnprintf(g_br_status, sizeof(g_br_status), "%s", L(STR_BR_CONNECTED));
        debug_printf("[browser] connected to proxy\n");

        char url[BR_URL_MAX];
        url_ensure_scheme(url, g_br_url, sizeof(url));
        char cmd[BR_URL_MAX + 8];
        ksnprintf(cmd, sizeof(cmd), "Maps %s", url);
        br_send(cmd);
        br_history_push(url);
        return;
    }

    if (pit_ms() - g_br_connect_start_ms >= PROXY_TIMEOUT) {
        char ip_str[20];
        net_ip_ntoa(g_br_target_ip, ip_str, sizeof(ip_str));
        tcp_close(g_br_sock);
        g_br_sock = TCP_INVALID;
        ksnprintf(g_br_status, sizeof(g_br_status),
                  L(STR_BR_CONN_TIMEOUT), ip_str, g_br_target_port);
        debug_printf("[browser] connection timed out\n");
    }
}

/* ---- Public API ------------------------------------------------------ */
/* The real browser_open body.  Runs on the kernel stack at CPL=0 (every
 * call goes through SYS_INVOKE so we can audit the ring transition).
 * Returns 1 on success, 0 on failure — matches the `int32_t (*)(uint32_t)`
 * signature that app_run_ring3 expects. */
static int32_t browser_open_impl(uint32_t unused) {
    (void)unused;
    if (g_br_win && g_br_win->in_use) {
        wm_set_focus(g_br_win);
        br_redraw();
        return 1;
    }
    if (!e1000_present()) {
        const char *info[] = {
            L(STR_BR_NEED_NIC1),
            L(STR_BR_NEED_NIC2),
        };
        dialog_info(L(STR_APP_BROWSER), info, 2);
        return 0;
    }

    /* Open near the top-left so the 1004x712 default window fits on a
     * 1024x768 screen without spilling under the taskbar. */
    g_br_win = wm_create_window(10, 10, BR_W, BR_H, L(STR_APP_BROWSER));
    if (!g_br_win) return 0;

    /* Install the destroy callback FIRST so that, if any subsequent
     * init step here faults, the recoverable-panic sweep that tears
     * down non-protected windows still calls br_destroy_cb() and
     * clears g_br_win.  Otherwise the next browser_open() would see
     * a dangling pointer and try to refocus a slot that may have
     * been reused by a completely different app. */
    wm_set_destroy_cb(g_br_win, br_destroy_cb);

    wm_set_content_click(g_br_win, br_click, NULL);
    wm_set_key_handler(g_br_win, br_key_handler);
    wm_set_scroll_handler(g_br_win, br_scroll);
    wm_set_resizable(g_br_win, true, BR_MIN_W, BR_MIN_H);
    wm_set_resize_cb(g_br_win, br_resize_cb);
    wm_set_icon(g_br_win, ICON_BROWSER);

    g_br_addr_caret = (int)strlen(g_br_url);
    ksnprintf(g_br_status, sizeof(g_br_status), "%s", L(STR_BR_READY));
    g_br_connected = false;
    g_br_sock = TCP_INVALID;
    g_br_last_frame_ms = 0;
    g_br_last_rx_ms    = 0;
    g_br_have_frame    = false;
    br_parse_reset();

    br_load_proxy_ip();
    br_resolve_proxy_ip();
    /* Register the discovery listener once; the slot survives window
     * close so beacons heard in between are not lost. */
    if (g_br_disc_slot < 0)
        g_br_disc_slot = net_udp_listen(BR_DISCOVERY_PORT);
    /* Detect the runtime pixel format ONCE so the per-pixel inner
     * loop doesn't have to consult vga_*_pos() for every pixel of
     * every frame. */
    br_detect_pixfmt();

    /* Spawn the JPEG decode worker once.  If the scheduler isn't up
     * (defensive - it starts long before apps), br_job_submit falls
     * back to the old inline decode. */
    if (!g_br_worker_spawned && sched_running()) {
        if (sched_spawn("brjpeg", br_jpeg_worker, 0, NULL)) {
            g_br_worker_spawned = true;
            debug_printf("[browser] JPEG decode worker spawned\n");
        }
    }

    br_redraw();
    return 1;
}

/* Public entry: drop the launcher to CPL=3 so even an "open browser"
 * click crosses the ring boundary at least once.  The ring-3 stub
 * immediately reflects back into the kernel via SYS_INVOKE for the
 * actual work — that's the documented escape hatch for kernel APIs
 * that don't have their own syscall yet (wm_*, gfx_*, font_*,
 * jpeg_*).  The brief CPL=3 trip catches any privileged instruction
 * the app might try to execute on the boot path, and exercises the
 * GDT/TSS/IDT plumbing on every launch so regressions show up
 * immediately instead of after months of dormant code. */
bool browser_open(void) {
    int32_t rv = app_run_ring3(browser_open_impl, 0, "browser");
    return rv != 0;
}

void browser_tick(void) {
    if (!g_br_win || !g_br_win->in_use) return;

    /* Beacon listener runs in every state - a proxy starting up after
     * the browser window opened must still be discovered. */
    br_discovery_poll();

    if (g_br_sock == TCP_INVALID && !g_br_connected) {
        br_connect_start();
        br_redraw();
        return;
    }

    if (!g_br_connected) {
        br_connect_poll();
        if (!g_br_connected) {
            br_redraw();
            return;
        }
    }

    if (!tcp_is_open(g_br_sock)) {
        g_br_connected = false;
        g_br_sock = TCP_INVALID;
        ksnprintf(g_br_status, sizeof(g_br_status), "%s", L(STR_BR_CONN_LOST));
        br_redraw();
        return;
    }

    /* Connection watchdog, two tiers (see the #define block up top):
     *   1. SILENCE: no TCP payload at all for BR_FRAME_WATCHDOG_MS.
     *      Bytes mid-frame / during a slow page.goto count as life signs,
     *      so a big JPEG or an 8 s navigation no longer triggers the
     *      reconnect-renavigate storm that wedged the proxy.
     *   2. STALL: bytes flow but no COMPLETE frame for
     *      BR_STALL_WATCHDOG_MS (half-frame parser desync, proxy capture
     *      lane stuck) — reconnect resets both ends' framing. */
    uint32_t now = pit_ms();
    uint32_t last_alive = g_br_last_rx_ms > g_br_last_frame_ms
                        ? g_br_last_rx_ms : g_br_last_frame_ms;
    const char *wd_kind = NULL;
    if (last_alive != 0 && now - last_alive > BR_FRAME_WATCHDOG_MS)
        wd_kind = "silence";
    else if (g_br_last_frame_ms != 0 &&
             now - g_br_last_frame_ms > BR_STALL_WATCHDOG_MS)
        wd_kind = "frame stall";
    if (wd_kind) {
        debug_printf("[browser] %s watchdog: rx idle %ums, frame idle %ums "
                     "- forcing reconnect\n", wd_kind,
                     now - g_br_last_rx_ms, now - g_br_last_frame_ms);
        tcp_close(g_br_sock);
        g_br_sock = TCP_INVALID;
        g_br_connected = false;
        g_br_last_frame_ms = 0;
        g_br_last_rx_ms    = 0;
        g_br_last_ping_ms  = 0;
        br_parse_reset();
        ksnprintf(g_br_status, sizeof(g_br_status), "%s", L(STR_BR_NO_FRAMES));
        br_redraw();
        return;
    }

    br_tcp_pump();
    br_present_stage();

    /* Heartbeat: send PING so the proxy captures a fresh screenshot
     * even when the remote page is static.  Without this, the frame
     * watchdog would fire on every page that stops animating, causing
     * an infinite reconnect cycle. */
    if (now - g_br_last_ping_ms >= BR_PING_MS) {
        br_send("PING");
        g_br_last_ping_ms = now;
    }

    br_redraw();
}
