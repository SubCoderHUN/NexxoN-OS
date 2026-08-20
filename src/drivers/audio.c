/* ============================================================================
 * NexxoN OS - Audio subsystem (AC'97 + HDA discovery)
 * ----------------------------------------------------------------------------
 * Brings up the first available audio device:
 *
 *   * AC'97 (PCI class 0x04 sub 0x01) - we map NABMBAR + bring the
 *     PCM-out stream out of reset, set the variable sample rate
 *     register to 44100 Hz, install a 1-entry Bus Master Descriptor
 *     List pointing at a static 16 KiB DMA buffer, and arm the
 *     controller.  Callers fill the buffer via audio_play_pcm and the
 *     controller drains it.
 *
 *   * Intel HDA (PCI class 0x04 sub 0x03) - logged + reserved.  We do
 *     not yet walk the codec verbs, so the device is identified but
 *     the PIT speaker fallback is used for actual playback.
 *
 *   * Fallback - if nothing else is present, audio_play_* maps to the
 *     PIT speaker frequency derived from the loudest non-zero sample.
 *     Crude but lets a WAV alarm still produce sound on hardware with
 *     no real audio.
 *
 * The mixer state (volume + mute) is shared with speaker.c through
 * the audio_set_volume / audio_get_volume API already declared there.
 * ============================================================================ */
#include "audio.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include "debug.h"
#include "pit.h"
#include "speaker.h"
#include "hda.h"

static audio_device_t g_dev;

/* AC'97 register offsets (NABMBAR = bus master register window). */
#define AC97_GLOB_CNT     0x2C
#define AC97_GLOB_STA     0x30
#define AC97_PO_BDBAR     0x10
#define AC97_PO_CIV       0x14
#define AC97_PO_LVI       0x15
#define AC97_PO_SR        0x16
#define AC97_PO_CR        0x1B

/* NAMBAR (mixer) register offsets. */
#define AC97_MIX_RESET      0x00
#define AC97_MIX_MASTER     0x02
#define AC97_MIX_PCM        0x18
#define AC97_MIX_RATE_FRONT 0x2C

#define AC97_BDL_LEN  32
typedef struct PACKED {
    uint32_t buffer;
    uint16_t length;     /* samples */
    uint16_t flags;
} ac97_bdl_t;

static ac97_bdl_t g_bdl[AC97_BDL_LEN] ALIGNED(16);
static int16_t    g_audio_buf[16384]  ALIGNED(16);

static void ac97_setup(audio_device_t *d) {
    /* Cold reset. */
    outl(d->nabmbar + AC97_GLOB_CNT, 2);
    pit_sleep(2);
    outl(d->nabmbar + AC97_GLOB_CNT, 0);
    pit_sleep(2);
    /* Volume registers - 0 = loudest. */
    outw(d->io_base + AC97_MIX_MASTER, 0x0202);
    outw(d->io_base + AC97_MIX_PCM,    0x0202);
    /* Sample rate. */
    outw(d->io_base + AC97_MIX_RATE_FRONT, 44100);
    /* Build a single BDL entry pointing at our buffer. */
    memset(g_bdl, 0, sizeof(g_bdl));
    g_bdl[0].buffer = (uint32_t)(uintptr_t)g_audio_buf;
    g_bdl[0].length = sizeof(g_audio_buf) / 2;
    g_bdl[0].flags  = 0;
    /* Reset PCM-out engine. */
    outb(d->nabmbar + AC97_PO_CR, 2);
    pit_sleep(2);
    outl(d->nabmbar + AC97_PO_BDBAR, (uint32_t)(uintptr_t)g_bdl);
    outb(d->nabmbar + AC97_PO_LVI, 0);
    d->ready = true;
}

bool audio_init(void) {
    memset(&g_dev, 0, sizeof(g_dev));
    /* AC'97 PCI scan. */
    pci_device_t devs[64];
    int n = pci_enumerate(devs, 64);
    for (int i = 0; i < n; i++) {
        if (devs[i].class_code != 0x04) continue;
        if (devs[i].subclass == 0x01) {
            g_dev.kind      = AUDIO_KIND_AC97;
            g_dev.vendor_id = devs[i].vendor_id;
            g_dev.device_id = devs[i].device_id;
            g_dev.io_base   = (uint16_t)(devs[i].bar[0] & 0xFFFCu);
            g_dev.nabmbar   = (uint16_t)(devs[i].bar[1] & 0xFFFCu);
            pci_enable_busmaster(&devs[i]);
            ac97_setup(&g_dev);
            debug_printf("[audio] AC'97 %04x:%04x mixer=0x%04x bm=0x%04x\n",
                         g_dev.vendor_id, g_dev.device_id,
                         g_dev.io_base, g_dev.nabmbar);
            return true;
        }
        if (devs[i].subclass == 0x03) {
            g_dev.kind      = AUDIO_KIND_HDA;
            g_dev.vendor_id = devs[i].vendor_id;
            g_dev.device_id = devs[i].device_id;
            g_dev.mmio_base = devs[i].bar[0] & 0xFFFFFFF0u;
            pci_enable_busmaster(&devs[i]);
            /* Intel PCHs: route HDA bus-master traffic to TC0 (TCSEL,
             * config offset 0x44).  Firmware sometimes leaves a non-zero
             * traffic class behind which starves the DMA engine. */
            if (g_dev.vendor_id == 0x8086) {
                uint32_t tcsel = pci_read32(devs[i].bus, devs[i].device,
                                            devs[i].function, 0x44);
                pci_write32(devs[i].bus, devs[i].device, devs[i].function,
                            0x44, tcsel & ~0x07u);
            }
            if (hda_init(g_dev.mmio_base)) {
                g_dev.ready = true;
                hda_apply_volume(audio_get_volume(), audio_is_muted());
                debug_printf("[audio] Intel HDA %04x:%04x MMIO 0x%08x: "
                             "native PCM output online\n",
                             g_dev.vendor_id, g_dev.device_id,
                             g_dev.mmio_base);
                return true;
            }
            debug_printf("[audio] Intel HDA %04x:%04x MMIO 0x%08x: codec "
                         "bring-up failed - falling back to PC speaker\n",
                         g_dev.vendor_id, g_dev.device_id, g_dev.mmio_base);
            g_dev.kind = AUDIO_KIND_PCSPK;
            g_dev.ready = true;
            return true;
        }
    }
    g_dev.kind = AUDIO_KIND_PCSPK;
    g_dev.ready = true;
    return false;
}

const audio_device_t *audio_device(void) { return &g_dev; }

/* Idle-loop pump: only the HDA path needs continuous service (its DMA
 * ring is topped up from a software FIFO); AC'97 loops its BDL buffer
 * autonomously and the PC speaker is fire-and-forget. */
void audio_tick(void) {
    if (g_dev.kind == AUDIO_KIND_HDA && g_dev.ready) hda_tick();
}

/* Called by speaker.c whenever the master volume / mute state changes so
 * codec-level amplifiers track the mixer (the PC-speaker fallback gates
 * tones in software and needs no hardware update). */
void audio_mixer_changed(void) {
    if (g_dev.kind == AUDIO_KIND_HDA && g_dev.ready)
        hda_apply_volume(audio_get_volume(), audio_is_muted());
}

/* Push a chunk of PCM into the BDL buffer.  We loop the buffer so a
 * caller's stream can be > 16k samples; subsequent submits wrap. */
static uint32_t g_bdl_off = 0;

int audio_play_pcm(const int16_t *samples, uint32_t frames,
                   int channels, int rate) {
    if (!samples || frames == 0) return 0;
    if (channels < 1) channels = 1;
    /* Nearest-neighbour rate convert if needed. */
    int stride = (rate * 1024) / 44100;
    if (stride == 0) stride = 1024;
    uint32_t copied = 0;
    if (g_dev.kind == AUDIO_KIND_HDA && g_dev.ready) {
        int taken = hda_play_pcm(samples, frames, channels, rate);
        /* Pump once immediately so short sounds start without waiting
         * for the next idle-loop audio_tick. */
        hda_tick();
        return taken;
    }
    if (g_dev.kind == AUDIO_KIND_AC97 && g_dev.ready) {
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t src_idx = (i * (uint32_t)stride) >> 10;
            if (src_idx >= frames) break;
            int16_t s = samples[src_idx * channels];   /* take left */
            uint32_t off = g_bdl_off % (sizeof(g_audio_buf) / 2);
            g_audio_buf[off] = s;
            g_bdl_off++;
            copied++;
        }
        /* Arm the controller (LVI = number of completed buffers - 1).  We
         * keep this at 0 because we use a single BDL entry; the engine
         * loops the buffer continuously. */
        outb(g_dev.nabmbar + AC97_PO_LVI, 0);
        outb(g_dev.nabmbar + AC97_PO_CR, 0x11);   /* run + ioc */
        return (int)copied;
    }
    if (g_dev.kind == AUDIO_KIND_PCSPK) {
        /* Sample the peak frequency of a 100ms window and beep it. */
        int peak = 0;
        for (uint32_t i = 0; i < frames && i < 4410; i++) {
            int v = samples[i * channels];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        uint32_t freq = 220 + (uint32_t)(peak / 64);
        if (freq > 4000) freq = 4000;
        if (peak > 200) speaker_beep(freq, 20);
        return (int)frames;
    }
    return 0;
}

typedef struct PACKED {
    char     riff[4];
    uint32_t size;
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_len;
    uint16_t format;
    uint16_t channels;
    uint32_t rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_hdr_t;

int audio_play_wav(const uint8_t *src, uint32_t len) {
    if (!src || len < sizeof(wav_hdr_t)) return -1;
    const wav_hdr_t *h = (const wav_hdr_t *)src;
    if (memcmp(h->riff, "RIFF", 4) != 0 || memcmp(h->wave, "WAVE", 4) != 0)
        return -1;
    if (h->format != 1 || h->bits_per_sample != 16) return -1;
    /* Locate the "data" chunk. */
    uint32_t off = sizeof(wav_hdr_t);
    while (off + 8 <= len) {
        const char *id = (const char *)(src + off);
        uint32_t cl = *(const uint32_t *)(src + off + 4);
        if (memcmp(id, "data", 4) == 0) {
            const int16_t *pcm = (const int16_t *)(src + off + 8);
            uint32_t frames = cl / (h->channels * 2);
            return audio_play_pcm(pcm, frames, h->channels, h->rate);
        }
        off += 8 + cl;
    }
    return -1;
}
