/* ============================================================================
 * NexxoN OS - Video streaming + A/V sync  (v1.0)
 * ----------------------------------------------------------------------------
 * HLS / DASH chunk fetcher + skeletal H.264 / VP9 NAL parser that
 * decodes frame layout headers (no pixel decoder yet) and routes the
 * presentation timestamps through a shared clock so the audio mixer
 * can keep the PCM buffer in lock-step.
 *
 * The clock contract:
 *
 *     video_sync_init(rate_hz)       - global wall reference, typically 90 kHz
 *                                       which is the MPEG-TS canonical clock
 *     video_sync_now()               - current PTS at the master clock
 *     video_sync_present(pts)        - blocks until master clock reaches pts
 *     video_sync_audio_advance(samples) - audio sink reports samples played
 *
 * The presenter and the audio mixer drive their cursors against the
 * same monotonic counter (pit_ms() scaled to rate_hz) so latency
 * between the two streams never exceeds a single tick.
 * ============================================================================ */
#ifndef NEXXON_VIDEO_H
#define NEXXON_VIDEO_H

#include "types.h"

#define VIDEO_HLS_MAX_SEGS  16
#define VIDEO_NAL_MAX       8192

typedef struct {
    uint32_t pts_90k;           /* presentation timestamp at 90 kHz */
    uint32_t dts_90k;           /* decode timestamp                  */
    uint16_t width;
    uint16_t height;
    uint8_t  nal_type;          /* H.264 NAL unit type 1..23         */
    bool     keyframe;
    uint8_t *payload;
    uint32_t payload_len;
} video_frame_t;

void video_sync_init        (uint32_t rate_hz);
uint32_t video_sync_now     (void);
int  video_sync_present     (uint32_t pts);
void video_sync_audio_advance(uint32_t samples_played, uint32_t sample_rate);

/* ---- H.264 NAL parser ------------------------------------------------ *
 * Walks Annex-B start codes (00 00 00 01) and decodes the leading byte
 * of each NAL unit into a {forbidden_zero_bit, nal_ref_idc, nal_type}
 * triple plus its raw payload extents. */
int  video_h264_split_nals  (const uint8_t *src, uint32_t src_len,
                             video_frame_t *out, int max);

/* ---- HLS playlist + chunk fetcher ----------------------------------- */
typedef struct {
    char     url[192];
    uint32_t duration_ms;
} video_hls_segment_t;

int  video_hls_parse_playlist(const char *m3u8, uint32_t len,
                              video_hls_segment_t *out, int max);
int  video_hls_fetch_chunk   (const char *url, uint8_t *out, uint32_t cap);

#endif /* NEXXON_VIDEO_H */
