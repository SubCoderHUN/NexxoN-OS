/* ============================================================================
 * NexxoN OS - Intel High Definition Audio (HDA) controller + codec driver
 * ----------------------------------------------------------------------------
 * Native PCM output for HDA controllers (Intel 6/7-series PCH, QEMU
 * intel-hda, ...).  audio.c routes the mixer API here when the PCI scan
 * finds an HDA-class device; the AC'97 path is untouched.
 *
 *   hda_init()         - controller reset, CORB/RIRB bring-up, codec walk,
 *                        output-path discovery (pin -> [mixer] -> DAC),
 *                        BDL/stream programming.  Returns false when no
 *                        usable output converter was found (caller falls
 *                        back to the PC speaker).
 *   hda_play_pcm()     - queue 16-bit PCM into the software FIFO (resampled
 *                        to the negotiated stream rate, up-mixed to stereo).
 *   hda_tick()         - idle-loop pump: tops the cyclic DMA ring up from
 *                        the FIFO based on the live LPIB read-back, pads
 *                        with silence on underrun.  Cheap when idle.
 *   hda_apply_volume() - master volume / mute -> codec amplifier verbs
 *                        (falls back to software gain in hda_tick when the
 *                        output path exposes no amplifier).
 * ============================================================================ */
#ifndef NEXXON_HDA_H
#define NEXXON_HDA_H

#include "types.h"

bool hda_init        (uint32_t mmio_base);
bool hda_ready       (void);
int  hda_play_pcm    (const int16_t *samples, uint32_t frames,
                      int channels, int rate);
void hda_tick        (void);
void hda_apply_volume(int volume, bool muted);

#endif /* NEXXON_HDA_H */
