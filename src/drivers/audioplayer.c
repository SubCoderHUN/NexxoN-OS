/* ============================================================================
 * NexxoN OS - Audio Player  (NexxoN Music)
 * ----------------------------------------------------------------------------
 * GUI front-end for the audio subsystem.  Loads WAV files from NXFS and
 * streams them to the AC'97 / HDA driver.  Supports Play / Pause / Stop
 * with a progress bar showing elapsed time vs. total duration.
 *
 * Architecture:
 *   - On Play: read the whole WAV blob into a static 2 MiB buffer, hand it
 *     to audio_play_wav().  The AC'97 DMA streams it in the background.
 *   - Progress is estimated from pit_ms() elapsed since playback start and
 *     the sample-rate + PCM size extracted from the RIFF header.
 *   - Pause/Resume: mutes / unmutes the audio device; the DMA continues
 *     running but no sound is produced.  A proper seek would require
 *     reloading the DMA BDL, which is a follow-up improvement.
 * ============================================================================ */
#include "audioplayer.h"
#include "audio.h"
#include "window.h"
#include "icons.h"
#include "gfx.h"
#include "font.h"
#include "i18n.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "keyboard.h"
#include "pit.h"
#include "nxfs.h"
#include "vfs.h"
#include "dialogs.h"
#include "theme.h"

/* ---------- Layout ------------------------------------------------------- */
#define AP_WIN_W       400
#define AP_WIN_H       260
#define AP_WIN_X       160
#define AP_WIN_Y       100

#define AP_PAD          10
#define AP_HDR_H        40
#define AP_BTN_W        80
#define AP_BTN_H        28
#define AP_PROG_H       12
#define AP_VOL_H        12
#define AP_VOL_W       160

/* ---------- Colours ------------------------------------------------------ */
#define AP_BG          0xFFF4F4F8
#define AP_HDR_BG      0xFF181880
#define AP_HDR_FG      0xFFFFFFFF
#define AP_BTN_PLAY    0xFF1870D0
#define AP_BTN_STOP    0xFFC03030
#define AP_BTN_PAUSE   0xFF808000
#define AP_BTN_OPEN    0xFF408040
#define AP_BTN_FG      0xFFFFFFFF
#define AP_BTN_DIS     0xFFB0B8C0
#define AP_PROG_TRACK  0xFFCCCCCC
#define AP_PROG_FILL   0xFF2080E0
#define AP_PROG_BORDER 0xFF8090A8
#define AP_VOL_FILL    0xFF20C040
#define AP_TEXT_FG     0xFF202028
#define AP_TEXT_DIM    0xFF707888

/* ---------- WAV buffer --------------------------------------------------- */
#define AP_WAV_BUF_SIZE   (2 * 1024 * 1024)   /* 2 MiB max file */
static uint8_t g_wav_buf[AP_WAV_BUF_SIZE];

/* ---------- Player state ------------------------------------------------- */
typedef enum { AP_STOPPED, AP_PLAYING, AP_PAUSED } ap_state_t;

static struct {
    window_t *win;
    ap_state_t state;
    char      path[256];
    char      filename[64];
    uint32_t  wav_size;
    uint32_t  duration_ms;
    uint32_t  start_ms;
    uint32_t  paused_elapsed;
    bool      has_file;
    int       volume;
} g_ap;

static bool stale(void) {
    if (!g_ap.win) return true;
    if (!g_ap.win->in_use) { g_ap.win = NULL; return true; }
    return false;
}

/* ---------- RIFF/WAV duration estimate ----------------------------------- */
static uint32_t wav_duration_ms(const uint8_t *buf, uint32_t len) {
    if (len < 44) return 0;
    /* RIFF header: "RIFF" + size + "WAVE" + "fmt " + chunk_size + audio_format
     * + channels + sample_rate + byte_rate + block_align + bits_per_sample */
    if (buf[0] != 'R' || buf[1] != 'I' || buf[2] != 'F' || buf[3] != 'F') return 0;
    if (buf[8] != 'W' || buf[9] != 'A' || buf[10] != 'V' || buf[11] != 'E') return 0;

    uint32_t byte_rate = 0;
    uint32_t data_size = 0;
    uint32_t pos = 12;
    while (pos + 8 <= len) {
        uint32_t chunk_id   = *(uint32_t *)(buf + pos);
        uint32_t chunk_size = *(uint32_t *)(buf + pos + 4);
        pos += 8;
        if (chunk_id == 0x20746D66u) {  /* "fmt " */
            if (chunk_size >= 16 && pos + 12 <= len)
                byte_rate = *(uint32_t *)(buf + pos + 8);
        } else if (chunk_id == 0x61746164u) {  /* "data" */
            data_size = chunk_size;
            break;
        }
        pos += chunk_size;
        if (chunk_size & 1) pos++;
    }
    if (byte_rate == 0 || data_size == 0) return 0;
    /* Avoid 64-bit division: compute in seconds first, add remainder millis. */
    uint32_t secs = data_size / byte_rate;
    uint32_t rem  = data_size % byte_rate;
    return secs * 1000u + (rem * 1000u) / byte_rate;
}

/* ---------- Drawing helpers ---------------------------------------------- */
static void draw_button(draw_target_t *t, int x, int y, int w, int h,
                        const char *label, uint32_t bg, bool disabled) {
    uint32_t c = disabled ? AP_BTN_DIS : bg;
    gfx_fill_rect(t, x, y, w, h, c);
    gfx_draw_rect(t, x, y, w, h, 0xFF000000);
    gfx_fill_rect(t, x + 1, y + 1, w - 2, 1, 0x30FFFFFF);
    int tx = x + (w - (int)(strlen(label) * 8)) / 2;
    int ty = y + (h - 8) / 2;
    if (tx < x + 2) tx = x + 2;
    gfx_draw_string(t, tx, ty, label, AP_BTN_FG, c);
}

static void draw_progress(draw_target_t *t, int x, int y, int w, int h,
                          uint32_t elapsed_ms, uint32_t total_ms) {
    gfx_fill_rect(t, x, y, w, h, AP_PROG_TRACK);
    gfx_draw_rect(t, x, y, w, h, AP_PROG_BORDER);
    if (total_ms > 0 && elapsed_ms > 0) {
        uint32_t fill = elapsed_ms / (total_ms / (uint32_t)(w - 2) + 1);
        if (fill > (uint32_t)(w - 2)) fill = (uint32_t)(w - 2);
        if (fill > 0)
            gfx_fill_rect(t, x + 1, y + 1, (int)fill, h - 2, AP_PROG_FILL);
    }
}

static void format_time(char *buf, int sz, uint32_t ms) {
    uint32_t s = ms / 1000;
    uint32_t m = s / 60;
    s %= 60;
    ksnprintf(buf, (uint32_t)sz, "%u:%02u", m, s);
}

static void redraw(void) {
    if (stale()) return;
    draw_target_t *t = &g_ap.win->content;
    int cw = (int)t->width;
    int ch = (int)t->height;

    gfx_fill_rect(t, 0, 0, cw, ch, AP_BG);

    /* Header: glossy glass band. */
    gfx_fill_rect(t, 0, 0, cw, AP_HDR_H, AP_HDR_BG);
    gfx_blend_rect(t, 0, 0, cw, AP_HDR_H / 2, 0x22FFFFFFu);
    gfx_blend_rect(t, 0, AP_HDR_H - 1, cw, 1, GLASS_EDGE_DARK);
    gfx_draw_string_aa_clipped(t, AP_PAD, 11, cw - 2 * AP_PAD,
                               L(STR_APP_AUDIOPLAYER), AP_HDR_FG, AP_HDR_BG);

    int y = AP_HDR_H + AP_PAD;

    /* File name */
    const char *fname = g_ap.has_file ? g_ap.filename : L(STR_AP_NO_FILE);
    gfx_draw_string_clipped(t, AP_PAD, y, cw - 2 * AP_PAD,
                            fname, AP_TEXT_FG, AP_BG);
    y += 18;

    /* Status label */
    const char *status_str;
    switch (g_ap.state) {
        case AP_PLAYING: status_str = L(STR_AP_PLAYING); break;
        case AP_PAUSED:  status_str = L(STR_AP_PAUSED);  break;
        default:         status_str = L(STR_AP_STOPPED);
    }
    gfx_draw_string(t, AP_PAD, y, status_str, AP_TEXT_DIM, AP_BG);
    y += 18;

    /* Duration info */
    if (g_ap.has_file && g_ap.duration_ms > 0) {
        char dur_buf[16], ela_buf[16], info[48];
        format_time(dur_buf, sizeof(dur_buf), g_ap.duration_ms);

        uint32_t elapsed = 0;
        if (g_ap.state == AP_PLAYING)
            elapsed = pit_ms() - g_ap.start_ms;
        else if (g_ap.state == AP_PAUSED)
            elapsed = g_ap.paused_elapsed;
        if (elapsed > g_ap.duration_ms) elapsed = g_ap.duration_ms;

        format_time(ela_buf, sizeof(ela_buf), elapsed);
        ksnprintf(info, sizeof(info), "%s / %s", ela_buf, dur_buf);
        gfx_draw_string(t, AP_PAD, y, L(STR_AP_DURATION), AP_TEXT_DIM, AP_BG);
        gfx_draw_string(t, AP_PAD + 60, y, info, AP_TEXT_FG, AP_BG);
        y += 18;

        /* Progress bar */
        draw_progress(t, AP_PAD, y, cw - 2 * AP_PAD, AP_PROG_H, elapsed, g_ap.duration_ms);
        y += AP_PROG_H + AP_PAD;
    } else {
        y += 36;
    }

    /* Volume label + bar */
    gfx_draw_string(t, AP_PAD, y + 2, L(STR_AP_VOLUME), AP_TEXT_DIM, AP_BG);
    int vol_x = AP_PAD + 56;
    int vol_val = g_ap.volume;
    /* Shared Aero glass slider (matches the tray volume control). */
    gfx_draw_slider_aero(t, vol_x, y - 2, AP_VOL_W, AP_VOL_H + 4, vol_val * 10, AP_VOL_FILL);
    char vbuf[8];
    ksnprintf(vbuf, sizeof(vbuf), "%d%%", vol_val);
    gfx_draw_string(t, vol_x + AP_VOL_W + 6, y + 2, vbuf, AP_TEXT_FG, AP_BG);
    y += AP_VOL_H + AP_PAD + 4;

    /* Buttons row */
    bool can_play  = g_ap.has_file && g_ap.state != AP_PLAYING;
    bool can_pause = g_ap.state == AP_PLAYING;
    bool can_stop  = g_ap.state != AP_STOPPED;

    int bx = AP_PAD;
    draw_button(t, bx, y, AP_BTN_W, AP_BTN_H, L(STR_AP_PLAY),  AP_BTN_PLAY,  !can_play);
    bx += AP_BTN_W + 6;
    draw_button(t, bx, y, AP_BTN_W, AP_BTN_H, L(STR_AP_PAUSE), AP_BTN_PAUSE, !can_pause);
    bx += AP_BTN_W + 6;
    draw_button(t, bx, y, AP_BTN_W, AP_BTN_H, L(STR_AP_STOP),  AP_BTN_STOP,  !can_stop);
    bx += AP_BTN_W + 6;
    draw_button(t, bx, y, AP_BTN_W, AP_BTN_H, L(STR_AP_OPEN),  AP_BTN_OPEN,  false);

    wm_mark_dirty();
}

/* ---------- File loading ------------------------------------------------- */
static bool load_file(const char *path) {
    /* Mounted-volume paths ("/usb0/...") read through the VFS so a track on
     * a pendrive plays directly — no staging copy into NXFS. */
    uint32_t bytes_read = 0;
    {
        const char *q0 = path;
        while (*q0 == '/') q0++;
        char first[VFS_NAME_MAX];
        int fi = 0;
        while (q0[fi] && q0[fi] != '/' && fi < VFS_NAME_MAX - 1) {
            first[fi] = q0[fi]; fi++;
        }
        first[fi] = 0;
        if (vfs_find_mount(first)) {
            int got = vfs_read(path, g_wav_buf, AP_WAV_BUF_SIZE);
            if (got <= 0) return false;
            bytes_read = (uint32_t)got;
        }
    }

    if (bytes_read == 0) {
        /* Resolve the path through NXFS: walk each component. */
        uint32_t inode = 1;   /* root */
        char tmp[256];
        int n = 0;
        const char *p = path;
        while (*p == '/') p++;

        while (*p) {
            if (*p == '/' || *(p + 1) == '\0') {
                if (*(p + 1) == '\0' && *p != '/') tmp[n++] = *p;
                tmp[n] = 0;
                if (n > 0) {
                    uint32_t child = 0;
                    if (nxfs_resolve(inode, tmp, &child) != 0) return false;
                    inode = child;
                }
                n = 0;
            } else {
                if (n < (int)sizeof(tmp) - 1) tmp[n++] = *p;
            }
            p++;
        }

        if (nxfs_read_file(inode, g_wav_buf, AP_WAV_BUF_SIZE, &bytes_read) != 0)
            return false;
    }
    if (bytes_read < 44) return false;

    g_ap.wav_size    = bytes_read;
    g_ap.duration_ms = wav_duration_ms(g_wav_buf, bytes_read);

    /* Copy filename component. */
    const char *slash = path;
    const char *q = path;
    while (*q) { if (*q == '/') slash = q + 1; q++; }
    int fn = 0;
    while (*slash && fn < (int)sizeof(g_ap.filename) - 1)
        g_ap.filename[fn++] = *slash++;
    g_ap.filename[fn] = 0;

    int pn = 0;
    while (*path && pn < (int)sizeof(g_ap.path) - 1)
        g_ap.path[pn++] = *path++;
    g_ap.path[pn] = 0;

    g_ap.has_file = true;
    return true;
}

/* ---------- Transport controls ------------------------------------------- */
static void do_play(void) {
    if (!g_ap.has_file) return;
    if (g_ap.state == AP_PAUSED) {
        audio_set_muted(false);
        g_ap.start_ms = pit_ms() - g_ap.paused_elapsed;
        g_ap.state = AP_PLAYING;
        return;
    }
    g_ap.start_ms = pit_ms();
    g_ap.paused_elapsed = 0;
    audio_play_wav(g_wav_buf, g_ap.wav_size);
    g_ap.state = AP_PLAYING;
    debug_printf("[audioplayer] playing '%s' (%u bytes, %u ms)\n",
                 g_ap.filename, g_ap.wav_size, g_ap.duration_ms);
}

static void do_pause(void) {
    if (g_ap.state != AP_PLAYING) return;
    g_ap.paused_elapsed = pit_ms() - g_ap.start_ms;
    audio_set_muted(true);
    g_ap.state = AP_PAUSED;
}

static void do_stop(void) {
    audio_set_muted(false);
    audio_play_pcm(NULL, 0, 2, 44100);   /* flush with zero-length */
    g_ap.state = AP_STOPPED;
    g_ap.paused_elapsed = 0;
}

static void do_open(void) {
    char path[256] = "";
    if (!dialog_input(L(STR_AP_OPEN), "/music/song.wav", "", path, sizeof(path)))
        return;
    if (!load_file(path)) {
        const char *lines[] = { "Cannot load file.", path };
        dialog_info(L(STR_AP_OPEN), lines, 2);
        return;
    }
    do_stop();
}

/* ---------- Hit testing -------------------------------------------------- */
static bool btn_hit(int mx, int my, int bx, int y) {
    return mx >= bx && mx < bx + AP_BTN_W &&
           my >= y  && my < y  + AP_BTN_H;
}

static bool on_click(window_t *w, int mx, int my,
                     uint8_t pressed, uint8_t btn) {
    (void)w; (void)btn;
    if (!pressed) return false;

    /* Recalculate button Y matching redraw(). */
    int y = AP_HDR_H + AP_PAD + 18 + 18;
    if (g_ap.has_file && g_ap.duration_ms > 0)
        y += 18 + AP_PROG_H + AP_PAD;
    else
        y += 36;
    y += AP_VOL_H + AP_PAD + 4;

    int bx = AP_PAD;
    if (btn_hit(mx, my, bx, y) && g_ap.has_file && g_ap.state != AP_PLAYING) {
        do_play(); redraw(); return true;
    }
    bx += AP_BTN_W + 6;
    if (btn_hit(mx, my, bx, y) && g_ap.state == AP_PLAYING) {
        do_pause(); redraw(); return true;
    }
    bx += AP_BTN_W + 6;
    if (btn_hit(mx, my, bx, y) && g_ap.state != AP_STOPPED) {
        do_stop(); redraw(); return true;
    }
    bx += AP_BTN_W + 6;
    if (btn_hit(mx, my, bx, y)) {
        do_open(); redraw(); return true;
    }
    return false;
}

static void on_destroy(window_t *w) {
    (void)w;
    g_ap.win = NULL;
    do_stop();
}

static void on_lang_change(lang_t l) {
    (void)l;
    if (!stale()) wm_set_title(g_ap.win, L(STR_APP_AUDIOPLAYER));
    redraw();
}

/* Re-render on WM resize (TASK 7: content greyed out until clicked). */
static void on_resize(window_t *w) { (void)w; redraw(); }

/* ---------- Public API --------------------------------------------------- */
bool audioplayer_open(void) {
    if (!stale()) {
        wm_set_focus(g_ap.win);
        wm_mark_dirty();
        return true;
    }

    g_ap.volume = audio_get_volume();
    if (g_ap.volume == 0) g_ap.volume = 80;

    window_t *w = wm_create_window(AP_WIN_X, AP_WIN_Y, AP_WIN_W, AP_WIN_H,
                                   L(STR_APP_AUDIOPLAYER));
    if (!w) return false;
    g_ap.win = w;

    wm_set_content_click(w, on_click, NULL);
    wm_set_destroy_cb(w, on_destroy);
    wm_set_resize_cb(w, on_resize);
    wm_set_icon(w, ICON_MUSIC);
    static bool lang_cb_reg = false;
    if (!lang_cb_reg) { lang_register_cb(on_lang_change); lang_cb_reg = true; }

    redraw();
    wm_set_focus(w);
    return true;
}

bool audioplayer_active(void) {
    return !stale();
}

void audioplayer_tick(void) {
    if (stale()) return;
    if (g_ap.state != AP_PLAYING) return;

    /* Auto-stop when duration elapses. */
    if (g_ap.duration_ms > 0) {
        uint32_t elapsed = pit_ms() - g_ap.start_ms;
        if (elapsed >= g_ap.duration_ms + 500u) {
            g_ap.state = AP_STOPPED;
            redraw();
            return;
        }
    }
    /* Update progress bar ~4 Hz. */
    static uint32_t last_redraw = 0;
    uint32_t now = pit_ms();
    if (now - last_redraw >= 250u) {
        last_redraw = now;
        redraw();
    }
}

void audioplayer_open_file(const char *path) {
    if (!path) return;
    if (!audioplayer_active()) audioplayer_open();
    if (stale()) return;
    if (load_file(path)) {
        do_stop();
        redraw();
    }
}
