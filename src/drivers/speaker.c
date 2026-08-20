/* ============================================================================
 * NexxoN OS - PC speaker driver
 * ----------------------------------------------------------------------------
 * Uses PIT channel 2 (port 0x42, command 0xB6 = ch2 / lobyte+hibyte / mode 3
 * = square wave / binary) plus port 0x61 bits 0..1 to gate the audio output
 * into the speaker amplifier.
 *
 * The frequency is a 16-bit divisor of the 1.193182 MHz PIT base clock:
 *      divisor = PIT_BASE_FREQ / requested_hz
 *
 * Below ~19 Hz the divisor overflows the 16 bits; above ~600 kHz it is too
 * small to produce a tone.  We clamp into the [37 Hz, 20 kHz] audio band.
 * ============================================================================ */
#include "speaker.h"
#include "io.h"
#include "pit.h"
#include "audio.h"

#define PIT_CH2_DATA  0x42
#define PIT_CMD       0x43
#define SPK_GATE      0x61
#define PIT_BASE_FREQ 1193182u

/* Global mixer state shared with Gephaz Audio tab + taskbar widget
 * (TASK 22).  The PC speaker is binary on/off so 'volume' really only
 * gates whether tones play at all (anything > 0 plays at full output),
 * but the abstraction is the right place to land when an AC'97 / HDA
 * mixer driver eventually lands. */
static int  g_master_volume = 70;     /* 0..100 */
static bool g_master_muted  = false;

int  audio_get_volume(void)        { return g_master_volume; }
void audio_set_volume(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_master_volume = v;
    audio_mixer_changed();
}
bool audio_is_muted (void)         { return g_master_muted; }
void audio_set_muted(bool m)       { g_master_muted = m; audio_mixer_changed(); }

void speaker_on(uint32_t hz) {
    if (hz < 37)    hz = 37;
    if (hz > 20000) hz = 20000;
    uint32_t div = PIT_BASE_FREQ / hz;
    if (div > 0xFFFF) div = 0xFFFF;

    outb(PIT_CMD, 0xB6);
    outb(PIT_CH2_DATA, (uint8_t)(div & 0xFF));
    outb(PIT_CH2_DATA, (uint8_t)((div >> 8) & 0xFF));

    /* Gate ON: set bits 0 and 1 of port 0x61 without disturbing the rest. */
    uint8_t g = inb(SPK_GATE);
    if ((g & 0x03) != 0x03) outb(SPK_GATE, g | 0x03);
}

void speaker_off(void) {
    uint8_t g = inb(SPK_GATE);
    outb(SPK_GATE, g & ~0x03);
}

/* Square-wave PCM synthesis for boards with no PC-speaker piezo (the
 * common case on modern ATX builds): when an HDA codec is online, system
 * tones render through it instead of PIT channel 2.  Mono submit; the
 * HDA layer up-mixes to stereo.  16384 frames cover the longest jingle. */
#define TONE_RATE  44100
#define TONE_AMP   6000
static int16_t g_tone_buf[16384];

static uint32_t tone_synth(int16_t *dst, uint32_t off, uint32_t cap,
                           uint32_t hz, uint32_t ms) {
    uint32_t frames = (TONE_RATE / 1000u) * ms;
    uint32_t half   = (hz > 0) ? (TONE_RATE / (2u * hz)) : 0;
    if (half == 0) half = 1;
    for (uint32_t i = 0; i < frames && off < cap; i++, off++)
        dst[off] = (hz == 0) ? 0
                 : (((i / half) & 1) ? (int16_t)-TONE_AMP : (int16_t)TONE_AMP);
    return off;
}

static bool hda_tone(uint32_t hz, uint32_t ms) {
    if (audio_device()->kind != AUDIO_KIND_HDA) return false;
    uint32_t n = tone_synth(g_tone_buf, 0, sizeof(g_tone_buf) / 2, hz, ms);
    audio_play_pcm(g_tone_buf, n, 1, TONE_RATE);
    return true;
}

void speaker_beep(uint32_t hz, uint32_t ms) {
    if (g_master_muted || g_master_volume == 0) return;
    if (hda_tone(hz, ms)) return;       /* non-blocking; DMA drains it */
    speaker_on(hz);
    pit_sleep(ms);
    speaker_off();
}

/* Short triadic chime: C5 -> E5 -> G5 -> C6.  Pauses 20 ms between notes
 * so the listener perceives four distinct tones rather than one slide. */
void speaker_boot_jingle(void) {
    static const uint32_t notes[4] = { 523, 659, 784, 1046 };
    if (audio_device()->kind == AUDIO_KIND_HDA) {
        uint32_t off = 0, cap = sizeof(g_tone_buf) / 2;
        for (int i = 0; i < 4; i++) {
            off = tone_synth(g_tone_buf, off, cap, notes[i], 70);
            off = tone_synth(g_tone_buf, off, cap, 0, 20);
        }
        audio_play_pcm(g_tone_buf, off, 1, TONE_RATE);
        return;
    }
    for (int i = 0; i < 4; i++) {
        speaker_on(notes[i]);
        pit_sleep(70);
        speaker_off();
        pit_sleep(20);
    }
}
