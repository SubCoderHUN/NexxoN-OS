/* ============================================================================
 * NexxoN OS - Intel High Definition Audio driver
 * ----------------------------------------------------------------------------
 * Brings one HDA controller + one output codec path online:
 *
 *   1. Controller: full CRST reset cycle, codec presence from STATESTS,
 *      CORB/RIRB DMA rings for the verb interface (immediate-command
 *      registers are kept as a fallback for firmware that wedges the
 *      rings - some real PCHs need it after a warm reboot).
 *   2. Codec: walk the root node -> audio function group -> widgets.
 *      Pick the best output pin by default-configuration device type
 *      (line-out > speaker > headphone), then resolve its connection
 *      list to a DAC, tolerating one mixer/selector hop in between
 *      (Realtek codecs route line-out through a mixer; QEMU's
 *      hda-duplex pin connects straight to the DAC).
 *   3. Stream: the first OUTPUT stream descriptor runs a small cyclic
 *      buffer (BDL with HDA_BDL_ENTRIES entries over one contiguous
 *      ring).  The ring is topped up from a software FIFO by hda_tick()
 *      using the LPIB read-back as the consumer position; underruns
 *      pad with silence so stale samples never loop audibly.
 *
 * Everything is polled - no IRQ line is claimed.  All DMA targets live
 * in BSS; the kernel runs identity-mapped so virtual == physical.
 * ============================================================================ */
#include "hda.h"
#include "pit.h"
#include "string.h"
#include "debug.h"

/* ---- Controller register offsets (from MMIO BAR0) --------------------- */
#define HDA_GCAP        0x00    /* 16: global capabilities                 */
#define HDA_GCTL        0x08    /* 32: bit0 = CRST                         */
#define HDA_STATESTS    0x0E    /* 16: codec presence (write-1-clear)      */
#define HDA_CORBLBASE   0x40
#define HDA_CORBUBASE   0x44
#define HDA_CORBWP      0x48    /* 16 */
#define HDA_CORBRP      0x4A    /* 16: bit15 = read-pointer reset          */
#define HDA_CORBCTL     0x4C    /*  8: bit1 = DMA run                      */
#define HDA_CORBSIZE    0x4E    /*  8 */
#define HDA_RIRBLBASE   0x50
#define HDA_RIRBUBASE   0x54
#define HDA_RIRBWP      0x58    /* 16: bit15 = write-pointer reset         */
#define HDA_RINTCNT     0x5A    /* 16 */
#define HDA_RIRBCTL     0x5C    /*  8: bit1 = DMA run                      */
#define HDA_RIRBSTS     0x5D    /*  8: bit0 = RINTFL (write-1-clear)       */
#define HDA_RIRBSIZE    0x5E    /*  8 */
#define HDA_ICOI        0x60    /* 32: immediate command output            */
#define HDA_ICII        0x64    /* 32: immediate response input            */
#define HDA_ICIS        0x68    /* 16: bit0 = busy, bit1 = result valid    */
#define HDA_DPLBASE     0x70

/* Stream descriptor register offsets (from SD base). */
#define SD_CTL0         0x00    /*  8: bit0 SRST, bit1 RUN                 */
#define SD_CTL2         0x02    /*  8: bits 7:4 = stream tag               */
#define SD_STS          0x03    /*  8: write-1-clear status                */
#define SD_LPIB         0x04    /* 32: link position in cyclic buffer      */
#define SD_CBL          0x08    /* 32: cyclic buffer length                */
#define SD_LVI          0x0C    /* 16: last valid BDL index                */
#define SD_FMT          0x12    /* 16: stream format                      */
#define SD_BDPL         0x18
#define SD_BDPU         0x1C

/* ---- Codec verbs ------------------------------------------------------- */
#define VERB_GET_PARAM      0xF00
#define VERB_GET_CONN_LIST  0xF02
#define VERB_GET_CONFIG_DEF 0xF1C
#define VERB_SET_CONN_SEL   0x701
#define VERB_SET_POWER      0x705
#define VERB_SET_STREAM_CH  0x706
#define VERB_SET_PIN_CTL    0x707
#define VERB_SET_EAPD       0x70C
#define VERB_SET_AMP        0x003   /* 4-bit verb, 16-bit payload          */
#define VERB_SET_FORMAT     0x002   /* 4-bit verb, 16-bit payload          */

#define PARAM_NODE_COUNT    0x04
#define PARAM_FG_TYPE       0x05
#define PARAM_AUDIO_CAPS    0x09
#define PARAM_PCM_RATES     0x0A
#define PARAM_PIN_CAPS      0x0C
#define PARAM_CONN_LEN      0x0E
#define PARAM_OUT_AMP_CAPS  0x12

#define WIDGET_DAC          0x0
#define WIDGET_MIXER        0x2
#define WIDGET_SELECTOR     0x3
#define WIDGET_PIN          0x4

/* ---- Stream geometry ----------------------------------------------------
 * 16384 stereo frames = 64 KiB = ~372 ms at 44.1 kHz.  The BDL slices the
 * ring into 4 equal entries (the spec minimum is 2).  SILENCE_MARGIN is
 * how far ahead of the DMA reader hda_tick keeps the ring padded when the
 * FIFO runs dry - small enough that a fresh sound starts within ~50 ms. */
#define HDA_RING_FRAMES     16384
#define HDA_RING_BYTES      (HDA_RING_FRAMES * 4)
#define HDA_BDL_ENTRIES     4
#define HDA_SILENCE_MARGIN  2048
#define HDA_FRESH_LEAD      1024

/* Software FIFO: sized so the audio player's whole 2 MiB WAV cap fits
 * (audioplayer.c submits the full track in one audio_play_wav call and
 * expects the DMA to drain it in the background). */
#define HDA_FIFO_FRAMES     (576 * 1024)

typedef struct PACKED {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t length;
    uint32_t flags;      /* bit0 = IOC (unused - we poll LPIB) */
} hda_bdl_entry_t;

/* ---- DMA + FIFO storage -------------------------------------------------
 * CORB/RIRB/BDL need 128-byte alignment per spec. */
ALIGNED(128) static uint32_t        g_corb[256];
ALIGNED(128) static uint64_t        g_rirb[256];
ALIGNED(128) static hda_bdl_entry_t g_bdl[HDA_BDL_ENTRIES];
ALIGNED(128) static int16_t         g_ring[HDA_RING_FRAMES * 2];
static int16_t                      g_fifo[HDA_FIFO_FRAMES * 2];

/* ---- Driver state ------------------------------------------------------ */
static uint32_t g_mmio       = 0;
static bool     g_ready      = false;
static bool     g_use_immediate = false;  /* CORB/RIRB wedged -> ICOI path  */
static int      g_corb_entries = 256;
static int      g_rirb_entries = 256;
static uint16_t g_rirb_rp    = 0;

static int      g_cad        = -1;       /* codec address                  */
static int      g_afg        = -1;       /* audio function group nid       */
static int      g_dac        = -1;
static int      g_pin        = -1;
static int      g_amp_nid    = -1;       /* widget carrying the output amp */
static uint32_t g_amp_caps   = 0;
static int      g_rate       = 44100;    /* negotiated stream rate         */
static uint32_t g_sd         = 0;        /* output stream descriptor base  */

/* FIFO/ring bookkeeping (frame units). */
static uint32_t g_fifo_rd = 0, g_fifo_wr = 0, g_fifo_count = 0;
static uint32_t g_ring_wr = 0;            /* our producer position         */
static uint32_t g_silence_run = 0;        /* frames of silence since data  */

/* Software gain fallback when the codec path has no usable output amp. */
static bool g_sw_gain   = false;
static int  g_sw_volume = 100;
static bool g_sw_muted  = false;

/* ---- MMIO accessors ----------------------------------------------------- */
static inline void     w8 (uint32_t off, uint8_t  v) { *(volatile uint8_t  *)(uintptr_t)(g_mmio + off) = v; }
static inline void     w16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(uintptr_t)(g_mmio + off) = v; }
static inline void     w32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(uintptr_t)(g_mmio + off) = v; }
static inline uint8_t  r8 (uint32_t off) { return *(volatile uint8_t  *)(uintptr_t)(g_mmio + off); }
static inline uint16_t r16(uint32_t off) { return *(volatile uint16_t *)(uintptr_t)(g_mmio + off); }
static inline uint32_t r32(uint32_t off) { return *(volatile uint32_t *)(uintptr_t)(g_mmio + off); }

/* ---- Verb interface ------------------------------------------------------
 * Encoding: [31:28]=codec  [27:20]=nid  [19:8]=verb  [7:0]=payload for the
 * 12-bit verbs; the 4-bit verbs (SET_AMP, SET_FORMAT) take [19:16]=verb,
 * [15:0]=payload. */
static uint32_t verb_make(int nid, uint32_t verb, uint32_t payload) {
    if (verb <= 0xF)
        return ((uint32_t)g_cad << 28) | ((uint32_t)nid << 20) |
               (verb << 16) | (payload & 0xFFFF);
    return ((uint32_t)g_cad << 28) | ((uint32_t)nid << 20) |
           (verb << 8) | (payload & 0xFF);
}

static bool hda_cmd_immediate(uint32_t cmd, uint32_t *resp) {
    /* Clear a stale result-valid flag, then issue. */
    w16(HDA_ICIS, 0x02);
    uint32_t t0 = pit_ms();
    while (r16(HDA_ICIS) & 0x01) {
        if (pit_ms() - t0 > 50) return false;
    }
    w32(HDA_ICOI, cmd);
    w16(HDA_ICIS, 0x01);
    t0 = pit_ms();
    while (!(r16(HDA_ICIS) & 0x02)) {
        if (pit_ms() - t0 > 50) return false;
    }
    if (resp) *resp = r32(HDA_ICII);
    w16(HDA_ICIS, 0x02);
    return true;
}

static bool hda_cmd(int nid, uint32_t verb, uint32_t payload, uint32_t *resp) {
    uint32_t cmd = verb_make(nid, verb, payload);
    if (g_use_immediate) return hda_cmd_immediate(cmd, resp);

    uint16_t wp = (uint16_t)((r16(HDA_CORBWP) + 1) % g_corb_entries);
    g_corb[wp] = cmd;
    w16(HDA_CORBWP, wp);

    uint32_t t0 = pit_ms();
    for (;;) {
        uint16_t rirb_wp = (uint16_t)(r16(HDA_RIRBWP) % g_rirb_entries);
        while (g_rirb_rp != rirb_wp) {
            g_rirb_rp = (uint16_t)((g_rirb_rp + 1) % g_rirb_entries);
            uint64_t e = g_rirb[g_rirb_rp];
            uint32_t ex = (uint32_t)(e >> 32);
            /* Ack RINTFL (+ overrun) - QEMU's intel-hda stops draining
             * the CORB once RINTCNT responses pile up un-acked, and real
             * PCHs latch the overrun bit the same way. */
            w8(HDA_RIRBSTS, 0x05);
            if (ex & 0x10) continue;           /* unsolicited - skip      */
            if (resp) *resp = (uint32_t)e;
            return true;
        }
        if (pit_ms() - t0 > 50) {
            debug_printf("[hda] CORB/RIRB timeout (verb %03x nid %02x) - "
                         "switching to immediate commands\n", verb, nid);
            g_use_immediate = true;
            return hda_cmd_immediate(cmd, resp);
        }
    }
}

static uint32_t hda_param(int nid, uint32_t param) {
    uint32_t v = 0;
    if (!hda_cmd(nid, VERB_GET_PARAM, param, &v)) return 0;
    return v;
}

/* ---- Controller bring-up ------------------------------------------------ */
static bool hda_reset_controller(void) {
    /* Enter reset (CRST=0), wait for the link to settle, leave reset. */
    w32(HDA_GCTL, 0);
    uint32_t t0 = pit_ms();
    while (r32(HDA_GCTL) & 1) {
        if (pit_ms() - t0 > 100) {
            debug_printf("[hda] CRST never deasserted\n");
            return false;
        }
    }
    pit_sleep(2);
    w32(HDA_GCTL, 1);
    t0 = pit_ms();
    while (!(r32(HDA_GCTL) & 1)) {
        if (pit_ms() - t0 > 100) {
            debug_printf("[hda] CRST never asserted\n");
            return false;
        }
    }
    /* Codecs need >= 521 us after CRST to request enumeration. */
    pit_sleep(2);
    return true;
}

static void hda_setup_corb_rirb(void) {
    /* Stop both DMA engines before touching the ring registers. */
    w8(HDA_CORBCTL, 0);
    w8(HDA_RIRBCTL, 0);
    uint32_t t0 = pit_ms();
    while ((r8(HDA_CORBCTL) & 0x02) || (r8(HDA_RIRBCTL) & 0x02)) {
        if (pit_ms() - t0 > 50) break;
    }

    /* CORB: largest supported size (QEMU + every PCH support 256). */
    uint8_t sizecap = r8(HDA_CORBSIZE);
    if (sizecap & 0x40)      { w8(HDA_CORBSIZE, (uint8_t)((sizecap & 0xFC) | 2)); g_corb_entries = 256; }
    else if (sizecap & 0x20) { w8(HDA_CORBSIZE, (uint8_t)((sizecap & 0xFC) | 1)); g_corb_entries = 16;  }
    else                     { w8(HDA_CORBSIZE, (uint8_t)(sizecap & 0xFC));       g_corb_entries = 2;   }
    memset(g_corb, 0, sizeof(g_corb));
    w32(HDA_CORBLBASE, (uint32_t)(uintptr_t)g_corb);
    w32(HDA_CORBUBASE, 0);
    /* Read-pointer reset handshake (best-effort: QEMU latches the bit
     * differently from real PCHs, so don't hard-fail on read-back). */
    w16(HDA_CORBRP, 0x8000);
    pit_sleep(1);
    w16(HDA_CORBRP, 0);
    w16(HDA_CORBWP, 0);

    /* RIRB. */
    sizecap = r8(HDA_RIRBSIZE);
    if (sizecap & 0x40)      { w8(HDA_RIRBSIZE, (uint8_t)((sizecap & 0xFC) | 2)); g_rirb_entries = 256; }
    else if (sizecap & 0x20) { w8(HDA_RIRBSIZE, (uint8_t)((sizecap & 0xFC) | 1)); g_rirb_entries = 16;  }
    else                     { w8(HDA_RIRBSIZE, (uint8_t)(sizecap & 0xFC));       g_rirb_entries = 2;   }
    memset(g_rirb, 0, sizeof(g_rirb));
    w32(HDA_RIRBLBASE, (uint32_t)(uintptr_t)g_rirb);
    w32(HDA_RIRBUBASE, 0);
    w16(HDA_RIRBWP, 0x8000);            /* write-pointer reset             */
    w16(HDA_RINTCNT, 1);
    w8(HDA_RIRBSTS, 0x05);              /* clear stale RINTFL/overrun      */
    g_rirb_rp = 0;

    /* DMA position buffer stays disabled - hda_tick polls LPIB instead. */
    w32(HDA_DPLBASE, 0);

    w8(HDA_CORBCTL, 0x02);
    /* RINTCTL (bit0) must be on even though we poll: the response-count
     * latch only sets RIRBSTS.RINTFL when it is enabled, and clearing
     * that flag is what re-arms response delivery (QEMU and real PCHs
     * agree here).  No IRQ actually fires - INTCTL.GIE stays 0. */
    w8(HDA_RIRBCTL, 0x03);
}

/* ---- Codec output-path discovery ---------------------------------------- */
static int widget_type(int nid) {
    return (int)((hda_param(nid, PARAM_AUDIO_CAPS) >> 20) & 0xF);
}

/* Read connection-list entry `idx` (short-form: 4 packed bytes per
 * response).  Range entries (bit 7) are treated as their endpoint - good
 * enough for the codecs we target. */
static int conn_entry(int nid, int idx) {
    uint32_t resp = 0;
    if (!hda_cmd(nid, VERB_GET_CONN_LIST, (uint32_t)(idx & ~3), &resp)) return -1;
    return (int)((resp >> ((idx & 3) * 8)) & 0x7F);
}

static int conn_count(int nid) {
    uint32_t len = hda_param(nid, PARAM_CONN_LEN);
    if (len & 0x80) return 0;           /* long form - not handled        */
    return (int)(len & 0x7F);
}

/* Unmute + 0 dB on a widget's output amp (and optionally one input amp
 * index).  Gain = the amp capability offset = the 0 dB step. */
static void amp_unmute(int nid, int in_idx) {
    uint32_t caps = hda_param(nid, PARAM_OUT_AMP_CAPS);
    if (!caps) caps = hda_param(g_afg, PARAM_OUT_AMP_CAPS);
    uint32_t gain = caps & 0x7F;
    hda_cmd(nid, VERB_SET_AMP, 0xB000 | gain, NULL);          /* out L+R   */
    if (in_idx >= 0)
        hda_cmd(nid, VERB_SET_AMP, 0x7000 | ((uint32_t)in_idx << 8) | gain,
                NULL);                                        /* in  L+R   */
}

/* Score a pin's default-configuration device type: line-out beats
 * speaker beats headphone; everything else is unusable for PCM out. */
static int pin_score(uint32_t cfg) {
    uint32_t port = (cfg >> 30) & 0x3;
    uint32_t dev  = (cfg >> 20) & 0xF;
    if (port == 1) return -1;           /* no physical connection         */
    if (dev == 0x0) return 3;           /* line out                       */
    if (dev == 0x1) return 2;           /* speaker                        */
    if (dev == 0x2) return 1;           /* headphone                      */
    return -1;
}

/* Resolve `pin`'s connection list to a DAC, tolerating one mixer /
 * selector hop.  On success fills g_dac and routes/unmutes the path. */
static bool route_pin_to_dac(int pin) {
    int n = conn_count(pin);
    for (int i = 0; i < n; i++) {
        int c = conn_entry(pin, i);
        if (c <= 0) continue;
        int t = widget_type(c);
        if (t == WIDGET_DAC) {
            g_dac = c;
            hda_cmd(pin, VERB_SET_CONN_SEL, (uint32_t)i, NULL);
            return true;
        }
        if (t == WIDGET_MIXER || t == WIDGET_SELECTOR) {
            int m = conn_count(c);
            for (int j = 0; j < m; j++) {
                int c2 = conn_entry(c, j);
                if (c2 <= 0 || widget_type(c2) != WIDGET_DAC) continue;
                g_dac = c2;
                hda_cmd(pin, VERB_SET_CONN_SEL, (uint32_t)i, NULL);
                if (t == WIDGET_SELECTOR)
                    hda_cmd(c, VERB_SET_CONN_SEL, (uint32_t)j, NULL);
                hda_cmd(c, VERB_SET_POWER, 0, NULL);
                amp_unmute(c, j);
                return true;
            }
        }
    }
    return false;
}

static bool hda_setup_codec(void) {
    /* Function group walk: find the AFG. */
    uint32_t sub = hda_param(0, PARAM_NODE_COUNT);
    int fg_start = (int)((sub >> 16) & 0xFF);
    int fg_count = (int)(sub & 0xFF);
    for (int fg = fg_start; fg < fg_start + fg_count; fg++) {
        if ((hda_param(fg, PARAM_FG_TYPE) & 0x7F) == 1) { g_afg = fg; break; }
    }
    if (g_afg < 0) {
        debug_printf("[hda] codec %d has no audio function group\n", g_cad);
        return false;
    }
    hda_cmd(g_afg, VERB_SET_POWER, 0, NULL);    /* AFG -> D0              */
    pit_sleep(5);

    sub = hda_param(g_afg, PARAM_NODE_COUNT);
    int w_start = (int)((sub >> 16) & 0xFF);
    int w_count = (int)(sub & 0xFF);

    /* Pick the best output pin by default-config device type. */
    int best_pin = -1, best_score = 0;
    for (int nid = w_start; nid < w_start + w_count; nid++) {
        if (widget_type(nid) != WIDGET_PIN) continue;
        uint32_t pincaps = hda_param(nid, PARAM_PIN_CAPS);
        if (!(pincaps & 0x10)) continue;        /* not output capable     */
        uint32_t cfg = 0;
        hda_cmd(nid, VERB_GET_CONFIG_DEF, 0, &cfg);
        int score = pin_score(cfg);
        if (score > best_score) { best_score = score; best_pin = nid; }
    }
    if (best_pin >= 0 && route_pin_to_dac(best_pin)) {
        g_pin = best_pin;
    } else {
        /* Fallback: any output-capable pin that reaches a DAC. */
        for (int nid = w_start; nid < w_start + w_count; nid++) {
            if (widget_type(nid) != WIDGET_PIN) continue;
            if (!(hda_param(nid, PARAM_PIN_CAPS) & 0x10)) continue;
            if (route_pin_to_dac(nid)) { g_pin = nid; break; }
        }
    }
    if (g_pin < 0 || g_dac < 0) {
        debug_printf("[hda] no output pin->DAC path found\n");
        return false;
    }

    /* Power + unmute + enable the path.  Pin control: OUT enable (bit 6),
     * plus HP-drive (bit 7) when the pin reports headphone capability. */
    uint32_t pincaps = hda_param(g_pin, PARAM_PIN_CAPS);
    uint32_t pinctl  = 0x40 | ((pincaps & 0x08) ? 0x80 : 0);
    hda_cmd(g_dac, VERB_SET_POWER, 0, NULL);
    hda_cmd(g_pin, VERB_SET_POWER, 0, NULL);
    pit_sleep(5);
    hda_cmd(g_pin, VERB_SET_PIN_CTL, pinctl, NULL);
    if (pincaps & 0x10000)                       /* EAPD capable          */
        hda_cmd(g_pin, VERB_SET_EAPD, 0x02, NULL);
    amp_unmute(g_pin, 0);
    amp_unmute(g_dac, -1);

    /* The master-volume amp: prefer the DAC's own output amp, fall back
     * to the pin.  Bit 2 of the widget caps = "output amp present". */
    uint32_t dac_caps = hda_param(g_dac, PARAM_AUDIO_CAPS);
    uint32_t pin_caps = hda_param(g_pin, PARAM_AUDIO_CAPS);
    if (dac_caps & 0x04) {
        g_amp_nid  = g_dac;
        g_amp_caps = hda_param(g_dac, PARAM_OUT_AMP_CAPS);
    } else if (pin_caps & 0x04) {
        g_amp_nid  = g_pin;
        g_amp_caps = hda_param(g_pin, PARAM_OUT_AMP_CAPS);
    }
    if (g_amp_nid < 0 || (g_amp_caps & 0x7F00) == 0) {
        g_sw_gain = true;                /* no usable hw amp: scale in sw */
        debug_printf("[hda] no hardware output amp - software gain\n");
    }

    /* Stream rate: 44.1 kHz when the codec supports it, else 48 kHz
     * (hda_play_pcm resamples).  PCM caps: bit5 = 44.1k. */
    uint32_t pcm = hda_param(g_dac, PARAM_PCM_RATES);
    if (!pcm) pcm = hda_param(g_afg, PARAM_PCM_RATES);
    g_rate = (pcm & 0x20) ? 44100 : 48000;

    /* DAC: stream tag 1, channel 0, 16-bit stereo at g_rate.
     * Format word: bit14 = 44.1 kHz base, bits 6:4 = 001 (16-bit),
     * bits 3:0 = channels - 1. */
    uint16_t fmt = (uint16_t)(((g_rate == 44100) ? 0x4000 : 0) | 0x0011);
    hda_cmd(g_dac, VERB_SET_STREAM_CH, 0x10, NULL);
    hda_cmd(g_dac, VERB_SET_FORMAT, fmt, NULL);

    debug_printf("[hda] codec %d: AFG nid %02x, pin %02x -> DAC %02x, "
                 "%d Hz, amp nid %02x caps %08x\n",
                 g_cad, g_afg, g_pin, g_dac, g_rate, g_amp_nid, g_amp_caps);
    return true;
}

/* ---- Output stream ------------------------------------------------------ */
static bool hda_setup_stream(void) {
    uint16_t gcap = r16(HDA_GCAP);
    int iss = (gcap >> 8) & 0xF;
    int oss = (gcap >> 12) & 0xF;
    if (oss == 0) {
        debug_printf("[hda] controller has no output streams\n");
        return false;
    }
    g_sd = 0x80 + (uint32_t)iss * 0x20;   /* first OUTPUT descriptor      */

    /* Stop + reset the stream. */
    w8(g_sd + SD_CTL0, 0);
    pit_sleep(1);
    w8(g_sd + SD_CTL0, 0x01);             /* SRST                         */
    uint32_t t0 = pit_ms();
    while (!(r8(g_sd + SD_CTL0) & 0x01)) {
        if (pit_ms() - t0 > 50) break;
    }
    w8(g_sd + SD_CTL0, 0);
    t0 = pit_ms();
    while (r8(g_sd + SD_CTL0) & 0x01) {
        if (pit_ms() - t0 > 50) break;
    }

    /* Cyclic buffer: HDA_BDL_ENTRIES equal slices of the silence ring. */
    memset(g_ring, 0, sizeof(g_ring));
    memset(g_bdl, 0, sizeof(g_bdl));
    uint32_t slice = HDA_RING_BYTES / HDA_BDL_ENTRIES;
    for (int i = 0; i < HDA_BDL_ENTRIES; i++) {
        g_bdl[i].addr_lo = (uint32_t)(uintptr_t)g_ring + (uint32_t)i * slice;
        g_bdl[i].length  = slice;
    }
    w32(g_sd + SD_BDPL, (uint32_t)(uintptr_t)g_bdl);
    w32(g_sd + SD_BDPU, 0);
    w32(g_sd + SD_CBL, HDA_RING_BYTES);
    w16(g_sd + SD_LVI, HDA_BDL_ENTRIES - 1);

    uint16_t fmt = (uint16_t)(((g_rate == 44100) ? 0x4000 : 0) | 0x0011);
    w16(g_sd + SD_FMT, fmt);
    w8(g_sd + SD_CTL2, 0x10);             /* stream tag 1                 */
    w8(g_sd + SD_STS, 0x1C);              /* clear stale status bits      */
    w8(g_sd + SD_CTL0, 0x02);             /* RUN                          */

    g_ring_wr = 0;
    g_silence_run = HDA_RING_FRAMES;      /* ring starts all-silence      */
    debug_printf("[hda] output stream SD@%03x running: fmt=%04x ring=%u B "
                 "x%d BDL\n", g_sd, fmt, HDA_RING_BYTES, HDA_BDL_ENTRIES);
    return true;
}

/* ---- Public API ---------------------------------------------------------- */
bool hda_init(uint32_t mmio_base) {
    g_mmio = mmio_base;
    g_ready = false;
    g_use_immediate = false;
    g_cad = g_afg = g_dac = g_pin = g_amp_nid = -1;
    g_sw_gain = false;
    g_fifo_rd = g_fifo_wr = g_fifo_count = 0;

    if (!mmio_base) return false;
    if (!hda_reset_controller()) return false;

    uint16_t statests = r16(HDA_STATESTS);
    w16(HDA_STATESTS, statests);          /* write-1-clear                */
    if (statests == 0) {
        debug_printf("[hda] no codecs present (STATESTS=0)\n");
        return false;
    }
    hda_setup_corb_rirb();

    /* First codec that yields a working output path wins. */
    for (int cad = 0; cad < 15; cad++) {
        if (!(statests & (1u << cad))) continue;
        g_cad = cad;
        uint32_t vid = hda_param(0, 0x00);          /* vendor/device id   */
        debug_printf("[hda] codec %d: id %04x:%04x\n",
                     cad, vid >> 16, vid & 0xFFFF);
        g_afg = g_dac = g_pin = -1;
        if (hda_setup_codec()) break;
        g_cad = -1;
    }
    if (g_cad < 0) return false;
    if (!hda_setup_stream()) return false;

    g_ready = true;
    return true;
}

bool hda_ready(void) { return g_ready; }

void hda_apply_volume(int volume, bool muted) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    g_sw_volume = volume;
    g_sw_muted  = muted;
    if (!g_ready || g_sw_gain || g_amp_nid < 0) return;

    uint32_t zero_db = g_amp_caps & 0x7F;
    uint32_t gain = (uint32_t)((volume * (int)zero_db + 50) / 100);
    uint32_t payload = 0xB000 | gain;
    if (muted || volume == 0) payload |= 0x80;     /* mute bit            */
    hda_cmd(g_amp_nid, VERB_SET_AMP, payload, NULL);
}

/* Queue PCM into the FIFO: nearest-neighbour resample rate -> g_rate,
 * up-mix mono -> stereo, take the first two channels otherwise.  Returns
 * the number of INPUT frames consumed. */
int hda_play_pcm(const int16_t *samples, uint32_t frames,
                 int channels, int rate) {
    if (!g_ready || !samples || frames == 0) return 0;
    if (channels < 1) channels = 1;
    if (rate <= 0) rate = 44100;

    /* Q10 fixed-point stride avoids 64-bit division (no libgcc in this
     * kernel - same trick as audio.c's AC'97 path).  Caps keep every
     * intermediate product inside 32 bits. */
    if (frames > (2u << 20)) frames = 2u << 20;
    uint32_t stride = ((uint32_t)rate << 10) / (uint32_t)g_rate;
    if (stride == 0) stride = 1;
    uint32_t out_frames = (frames << 10) / stride;
    if (out_frames == 0) out_frames = 1;

    uint32_t queued = 0;
    for (uint32_t i = 0; i < out_frames; i++) {
        if (g_fifo_count >= HDA_FIFO_FRAMES) break;
        uint32_t src = (i * stride) >> 10;
        if (src >= frames) break;
        int16_t l = samples[src * (uint32_t)channels];
        int16_t r = (channels >= 2) ? samples[src * (uint32_t)channels + 1] : l;
        g_fifo[g_fifo_wr * 2]     = l;
        g_fifo[g_fifo_wr * 2 + 1] = r;
        g_fifo_wr = (g_fifo_wr + 1) % HDA_FIFO_FRAMES;
        g_fifo_count++;
        queued++;
    }
    /* Report consumption in caller units so audio_play_wav sees "all taken"
     * when the whole track fit. */
    return (int)((queued * stride) >> 10);
}

/* Idle-loop pump: top the DMA ring up from the FIFO.  LPIB is the live
 * consumer position; we never write past (reader - 1) so the engine
 * always chases fresh data.  On underrun the margin ahead of the reader
 * is padded with silence ONCE (g_silence_run caps the work at one full
 * ring), so an idle system costs a register read per tick. */
void hda_tick(void) {
    if (!g_ready) return;

    uint32_t lpib = r32(g_sd + SD_LPIB) % HDA_RING_BYTES;
    uint32_t rd   = lpib / 4;            /* reader position, frames       */

    /* One-shot COM1 marker so a bare-metal boot log proves the DMA engine
     * is actually consuming the ring (LPIB advancing past zero). */
    static bool dma_live_logged = false;
    if (!dma_live_logged && lpib != 0) {
        dma_live_logged = true;
        debug_printf("[hda] DMA live: lpib=%u\n", lpib);
    }

    /* g_silence_run = contiguous silence frames at the tail of what we
     * have written.  When everything still unplayed is silence (or the
     * reader already lapped us), resync the writer just ahead of the
     * reader so fresh audio starts within ~25 ms; the small lead gap is
     * zeroed because the reader will traverse it first. */
    uint32_t ahead = (g_ring_wr + HDA_RING_FRAMES - rd) % HDA_RING_FRAMES;
    if (g_fifo_count > 0 && g_silence_run > 0 &&
        ahead <= g_silence_run && ahead > HDA_FRESH_LEAD) {
        g_ring_wr = rd;
        for (uint32_t i = 0; i < HDA_FRESH_LEAD; i++) {
            g_ring[g_ring_wr * 2]     = 0;
            g_ring[g_ring_wr * 2 + 1] = 0;
            g_ring_wr = (g_ring_wr + 1) % HDA_RING_FRAMES;
        }
        g_silence_run = HDA_FRESH_LEAD;
    }

    uint32_t space = (rd + HDA_RING_FRAMES - g_ring_wr - 1) % HDA_RING_FRAMES;

    /* Drain the FIFO. */
    uint32_t n = (g_fifo_count < space) ? g_fifo_count : space;
    if (n > 0) {
        bool attenuate = g_sw_gain && (g_sw_muted || g_sw_volume < 100);
        for (uint32_t i = 0; i < n; i++) {
            int16_t l = g_fifo[g_fifo_rd * 2];
            int16_t r = g_fifo[g_fifo_rd * 2 + 1];
            if (attenuate) {
                int v = g_sw_muted ? 0 : g_sw_volume;
                l = (int16_t)(((int)l * v) / 100);
                r = (int16_t)(((int)r * v) / 100);
            }
            g_ring[g_ring_wr * 2]     = l;
            g_ring[g_ring_wr * 2 + 1] = r;
            g_ring_wr = (g_ring_wr + 1) % HDA_RING_FRAMES;
            g_fifo_rd = (g_fifo_rd + 1) % HDA_FIFO_FRAMES;
        }
        g_fifo_count -= n;
        space -= n;
        g_silence_run = 0;
    }

    /* Underrun guard: zero ALL writable space in one pass (capped at one
     * full ring per drain via g_silence_run).  Padding only a small
     * margin per tick was not enough - sparse ticks during blocking boot
     * phases let the reader lap the writer and the ring audibly looped
     * the previous sound for seconds. */
    if (g_fifo_count == 0 && g_silence_run < HDA_RING_FRAMES) {
        uint32_t pad = HDA_RING_FRAMES - g_silence_run;
        if (pad > space) pad = space;
        for (uint32_t i = 0; i < pad; i++) {
            g_ring[g_ring_wr * 2]     = 0;
            g_ring[g_ring_wr * 2 + 1] = 0;
            g_ring_wr = (g_ring_wr + 1) % HDA_RING_FRAMES;
        }
        g_silence_run += pad;
    }
}
