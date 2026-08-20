/* ============================================================================
 * NexxoN OS - Minimal PulseAudio native protocol shim
 * ============================================================================ */
#ifndef NEXXON_LIN_PULSE_H
#define NEXXON_LIN_PULSE_H

#include "types.h"

void lin_pulse_init(void);
void lin_pulse_on_connect(int link_ix);
void lin_pulse_on_client_write(int link_ix, const void *data, uint64_t len);

#endif /* NEXXON_LIN_PULSE_H */
