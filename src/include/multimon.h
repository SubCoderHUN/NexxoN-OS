/* ============================================================================
 * NexxoN OS - Multi-monitor display management  (v1.0)
 * ----------------------------------------------------------------------------
 * Detects and tracks connected displays.  Each entry in the display table
 * corresponds to one output connector that responded to EDID or VBE probing.
 * In the current implementation only one output (the primary VBE framebuffer)
 * is active; the second slot is reserved for a future GPU with a second CRTC.
 *
 * The GUI Display Manager app (multimon_open) shows the detected displays,
 * their EDID information, and allows switching to any supported resolution.
 * ============================================================================ */
#ifndef NEXXON_MULTIMON_H
#define NEXXON_MULTIMON_H

#include "types.h"
#include "edid.h"

#define MULTIMON_MAX_DISPLAYS   4

typedef enum {
    DISP_STATE_DISCONNECTED = 0,
    DISP_STATE_CONNECTED    = 1,
    DISP_STATE_ACTIVE       = 2,
} disp_state_t;

typedef struct {
    disp_state_t state;
    bool         primary;
    uint16_t     cur_w;
    uint16_t     cur_h;
    uint8_t      refresh_hz;
    edid_info_t  edid;
    char         name[32];
} display_info_t;

/* Probe all connectors, populate the display table. */
void multimon_init    (void);

/* Number of entries in the table (connected + disconnected). */
int  multimon_count   (void);

/* Get a display entry by index [0..multimon_count()-1]. */
const display_info_t *multimon_get(int idx);

/* Request a mode switch on display `idx`.  Returns false if not supported. */
bool multimon_set_mode(int idx, uint16_t w, uint16_t h, uint8_t hz);

/* Open the graphical Display Manager window. */
bool multimon_open    (void);

#endif /* NEXXON_MULTIMON_H */
