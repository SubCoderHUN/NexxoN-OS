/* ============================================================================
 * NexxoN OS - EDID + dynamic display resolution  (TASK 32, v1.0)
 * ----------------------------------------------------------------------------
 * Extracts EDID via the I2C/DDC interface or, when no DDC channel is
 * available (every BIOS-era VESA framebuffer), via INT 10h AX=4F15.
 * Exposes the supported mode list to Gephaz's Display tab and the
 * `edid` shell command.
 *
 * Mode switching is gated by vga_switch_mode() which reallocates the
 * VESA backbuffer and broadcasts a "viewport-changed" event through
 * the WM so every window can reflow.
 * ============================================================================ */
#ifndef NEXXON_EDID_H
#define NEXXON_EDID_H

#include "types.h"

#define EDID_MAX_MODES  8

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  refresh_hz;
    bool     preferred;
} edid_mode_t;

typedef struct {
    bool         present;
    char         manufacturer[4];     /* 3 ASCII letters + NUL */
    uint16_t     product_code;
    uint32_t     serial;
    uint8_t      week_of_manufacture;
    uint16_t     year_of_manufacture;
    int          n_modes;
    edid_mode_t  modes[EDID_MAX_MODES];
} edid_info_t;

bool edid_query   (edid_info_t *out);
bool edid_apply   (uint16_t width, uint16_t height);

#endif /* NEXXON_EDID_H */
