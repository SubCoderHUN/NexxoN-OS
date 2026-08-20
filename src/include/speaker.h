/* ============================================================================
 * NexxoN OS - PC speaker (PIT channel 2 + port 0x61 gate)
 * ----------------------------------------------------------------------------
 * The classic IBM-PC speaker has been there since 1981 and every BIOS still
 * wires it up.  Channel 2 of the 8254 PIT is wired to the speaker through a
 * gate controlled by bits 0 (gate enable) and 1 (output enable) of port
 * 0x61.  Programming a frequency f means writing a 16-bit divisor of
 * PIT_BASE_FREQ / f to channel 2.
 * ============================================================================ */
#ifndef NEXXON_SPEAKER_H
#define NEXXON_SPEAKER_H

#include "types.h"

/* Begin emitting `freq_hz` from the speaker.  Returns instantly. */
void speaker_on (uint32_t freq_hz);

/* Stop the speaker (drops the gate). */
void speaker_off(void);

/* Convenience: emit `freq_hz` for `duration_ms` milliseconds, then stop.
 * Blocks via pit_sleep() so callers must hold a system-tick context. */
void speaker_beep(uint32_t freq_hz, uint32_t duration_ms);

/* Play a short major-chord-up sweep used as the "system ready" jingle
 * once the desktop environment has finished loading.  Blocks for ~ 350 ms. */
void speaker_boot_jingle(void);

/* TASK 22: system-wide mixer state shared between the Gephaz Audio tab
 * and the taskbar volume widget.  PC speaker honours mute / volume==0
 * by short-circuiting tones; future AC'97 / HDA drivers will use these
 * getters to set the master gain. */
int  audio_get_volume(void);
void audio_set_volume(int v);
bool audio_is_muted (void);
void audio_set_muted(bool m);

#endif /* NEXXON_SPEAKER_H */
