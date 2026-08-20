/* ============================================================================
 * NexxoN OS - Audio subsystem (TASK 22)  v1.0
 * ----------------------------------------------------------------------------
 * Discovery + PCM mixer + AC'97 / Intel HDA driver shim.  Speaks to the
 * NABMBAR / BDL (Bus Master Descriptor List) registers on AC'97
 * controllers and lays the groundwork for Intel HDA (we probe the
 * device but the full HDA codec walk is in a follow-up patch).
 *
 *   audio_init()        - PCI scan + bring up the first capable codec.
 *   audio_get_volume()  - master volume 0..100 (clamped).
 *   audio_set_volume()  - applies to the codec mixer + tracks muted state.
 *   audio_play_wav()    - convenience: parse a 16-bit / 44.1 kHz PCM WAV
 *                         blob and stream it to the codec.
 *   audio_play_pcm()    - bulk push of 16-bit PCM samples.  Caller picks
 *                         channel count + rate from the supported set.
 *
 * The mixer maintains a single hardware output stream because that's the
 * dominant pattern for desktop OSes (system-wide volume + one focused
 * sink per app).  Per-app gain / panning is a future enhancement.
 * ============================================================================ */
#ifndef NEXXON_AUDIO_H
#define NEXXON_AUDIO_H

#include "types.h"

#define AUDIO_KIND_NONE     0
#define AUDIO_KIND_AC97     1
#define AUDIO_KIND_HDA      2
#define AUDIO_KIND_PCSPK    3       /* fallback: PIT speaker */

typedef struct {
    int      kind;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t io_base;
    uint16_t nabmbar;
    uint32_t mmio_base;
    bool     ready;
} audio_device_t;

bool audio_init       (void);
const audio_device_t *audio_device(void);

/* Idle-loop pump.  Keeps the HDA DMA ring fed from the software FIFO;
 * a no-op for AC'97 / PC-speaker outputs.  Must be called from every
 * loop that also pumps wm_tick()/pnp_tick() so playback survives modal
 * dialogs (stability lesson: modal loops must keep pumping). */
void audio_tick       (void);

/* Notifies the active codec driver that volume/mute changed (invoked by
 * the mixer setters in speaker.c). */
void audio_mixer_changed(void);

int  audio_get_volume (void);            /* declared by speaker.c too */
void audio_set_volume (int v);
bool audio_is_muted   (void);
void audio_set_muted  (bool m);

/* Bulk PCM submit.  `samples` are signed 16-bit interleaved.  `rate`
 * must be one of 8000 / 11025 / 16000 / 22050 / 44100 (others are
 * resampled by nearest-neighbour).  Returns the number of frames
 * actually queued. */
int  audio_play_pcm   (const int16_t *samples, uint32_t frames,
                       int channels, int rate);

/* Parse a RIFF/WAV blob and stream it.  Returns 0 on success. */
int  audio_play_wav   (const uint8_t *src, uint32_t len);

#endif /* NEXXON_AUDIO_H */
