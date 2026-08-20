/* ============================================================================
 * NexxoN OS - Video streaming + A/V sync
 * ----------------------------------------------------------------------------
 * Provides a unified monotonic clock that the video presenter and the
 * audio mixer both consume.  Audio sinks call video_sync_audio_advance
 * to declare "I just played N samples"; the master clock biases the
 * subsequent video presentation timestamps so the picture stays in
 * sync with the sound.
 *
 * The H.264 splitter accepts Annex-B encoded streams (typical MPEG-TS
 * / HLS payload), enumerates the NAL units, decodes the first byte of
 * each, and surfaces them through video_frame_t structs.  Full pixel
 * decoding is in a follow-up patch - this layer is enough to drive
 * frame-accurate timing.
 * ============================================================================ */
#include "video.h"
#include "string.h"
#include "debug.h"
#include "pit.h"
#include "download.h"

static uint32_t g_rate_hz       = 90000u;
static uint32_t g_origin_ms     = 0;
static uint32_t g_audio_pts_90k = 0;
static uint32_t g_audio_sample_rate = 44100u;

void video_sync_init(uint32_t rate_hz) {
    g_rate_hz       = rate_hz ? rate_hz : 90000u;
    g_origin_ms     = pit_ms();
    g_audio_pts_90k = 0;
    debug_printf("[video] sync clock initialised @ %u Hz\n", g_rate_hz);
}

uint32_t video_sync_now(void) {
    uint32_t ms = pit_ms() - g_origin_ms;
    /* Convert milliseconds to clock units (rate_hz / 1000). */
    return ms * (g_rate_hz / 1000u);
}

int video_sync_present(uint32_t pts) {
    while (video_sync_now() < pts) {
        __asm__ volatile ("sti; hlt");
        if (pit_ms() - g_origin_ms > 60000u) break;   /* sanity 60s cap */
    }
    return 0;
}

void video_sync_audio_advance(uint32_t samples, uint32_t sample_rate) {
    if (!sample_rate) sample_rate = g_audio_sample_rate;
    g_audio_sample_rate = sample_rate;
    /* Update the audio cursor in the master clock domain.  Folded as
     * 32-bit math to avoid pulling in libgcc's __udivdi3 (freestanding
     * kernel).  rate_hz/sample_rate * samples; with rate_hz=90000 and
     * sample_rate=44100 the multiplier is approx 2.04 which fits in
     * 32 bits for samples up to ~2 billion (~13 hours of audio). */
    uint32_t scale = g_rate_hz / sample_rate;
    uint32_t rem   = g_rate_hz - scale * sample_rate;
    g_audio_pts_90k += samples * scale + (samples * rem) / sample_rate;
}

/* ---------- H.264 NAL splitter --------------------------------------- */
int video_h264_split_nals(const uint8_t *src, uint32_t src_len,
                          video_frame_t *out, int max) {
    if (!src || !out || max <= 0) return 0;
    int n = 0;
    uint32_t i = 0;
    while (i + 4 <= src_len && n < max) {
        /* Annex-B start code: 00 00 00 01 or 00 00 01. */
        bool sc4 = (src[i] == 0 && src[i+1] == 0 && src[i+2] == 0 && src[i+3] == 1);
        bool sc3 = (src[i] == 0 && src[i+1] == 0 && src[i+2] == 1);
        if (!sc4 && !sc3) { i++; continue; }
        uint32_t hdr = sc4 ? 4 : 3;
        uint32_t start = i + hdr;
        if (start >= src_len) break;
        uint8_t nal_byte = src[start];
        uint32_t end = start + 1;
        while (end + 3 <= src_len) {
            if (src[end] == 0 && src[end+1] == 0 &&
                (src[end+2] == 1 || (src[end+2] == 0 && src[end+3] == 1))) {
                break;
            }
            end++;
        }
        video_frame_t *f = &out[n++];
        memset(f, 0, sizeof(*f));
        f->nal_type    = nal_byte & 0x1F;
        f->payload     = (uint8_t *)(src + start);
        f->payload_len = end - start;
        f->keyframe    = (f->nal_type == 5);          /* IDR slice */
        f->pts_90k     = video_sync_now();
        i = end;
    }
    return n;
}

/* ---------- HLS playlist + chunk fetcher ----------------------------- */
int video_hls_parse_playlist(const char *m3u8, uint32_t len,
                             video_hls_segment_t *out, int max) {
    if (!m3u8 || !out || max <= 0) return 0;
    int n = 0;
    uint32_t i = 0;
    uint32_t dur = 0;
    while (i < len && n < max) {
        /* Each line is either "#EXTINF:<seconds>," or a URL. */
        const char *line = m3u8 + i;
        uint32_t e = i;
        while (e < len && m3u8[e] != '\n') e++;
        if (line[0] == '#') {
            if (e - i >= 8 && line[1] == 'E' && line[2] == 'X' && line[3] == 'T' &&
                line[4] == 'I' && line[5] == 'N' && line[6] == 'F') {
                uint32_t p = i + 8;
                double seconds = 0;
                while (p < e && m3u8[p] >= '0' && m3u8[p] <= '9') {
                    seconds = seconds * 10 + (m3u8[p++] - '0');
                }
                if (p < e && m3u8[p] == '.') {
                    p++;
                    double frac = 0.1;
                    while (p < e && m3u8[p] >= '0' && m3u8[p] <= '9') {
                        seconds += (m3u8[p++] - '0') * frac;
                        frac *= 0.1;
                    }
                }
                dur = (uint32_t)(seconds * 1000);
            }
        } else if (line[0] != '\r' && line[0] != '\n' && line[0] != 0 && e > i) {
            uint32_t cap = (e - i < sizeof(out[n].url) - 1)
                            ? (e - i) : sizeof(out[n].url) - 1;
            memcpy(out[n].url, line, cap);
            /* Trim trailing CR. */
            if (cap > 0 && out[n].url[cap - 1] == '\r') cap--;
            out[n].url[cap] = 0;
            out[n].duration_ms = dur;
            n++;
            dur = 0;
        }
        i = e + 1;
    }
    return n;
}

int video_hls_fetch_chunk(const char *url, uint8_t *out, uint32_t cap) {
    return download_simple(url, out, cap);
}
