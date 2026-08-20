/* ============================================================================
 * NexxoN OS - EDID + dynamic resolution support
 * ----------------------------------------------------------------------------
 * In protected mode we cannot just call INT 10h, so the canonical EDID
 * source is the I2C/DDC path: send slave address 0x50 over the DDC SDA
 * line and read 128 bytes.  On hardware without an exposed I2C port
 * (almost every VBE-only setup), we synthesise a plausible EDID from
 * the framebuffer dimensions the bootloader already gave us, so the
 * UI has *something* to show.
 *
 * Mode switching: we don't dynamically reprogram the VBE here - that
 * requires real-mode entry which the kernel can't do trivially.  We
 * surface the request through vga_switch_mode() and the operator can
 * pick the desired resolution at the GRUB prompt next boot.  The
 * groundwork for live mode switching (reallocate backbuffer + dispatch
 * WM-reflow event) is in place.
 * ============================================================================ */
#include "edid.h"
#include "vga.h"
#include "string.h"
#include "debug.h"

static edid_info_t g_edid;

bool edid_query(edid_info_t *out) {
    if (g_edid.present) {
        if (out) *out = g_edid;
        return true;
    }
    /* Synthesise a single-mode EDID from the current framebuffer. */
    g_edid.present = true;
    memcpy(g_edid.manufacturer, "NXN", 3);
    g_edid.manufacturer[3] = 0;
    g_edid.product_code    = 0x0001;
    g_edid.serial          = 0;
    g_edid.week_of_manufacture = 1;
    g_edid.year_of_manufacture = 2026;
    g_edid.n_modes = 0;
    /* Always offer 1024x768 + 800x600 + the current native size. */
    static const struct { uint16_t w, h, hz; } common[] = {
        { 1920, 1080, 60 },
        { 1280, 1024, 60 },
        { 1280,  720, 60 },
        { 1024,  768, 60 },
        {  800,  600, 60 },
        {  640,  480, 60 },
    };
    uint32_t cur_w = vga_width();
    uint32_t cur_h = vga_height();
    g_edid.modes[g_edid.n_modes].width  = (uint16_t)cur_w;
    g_edid.modes[g_edid.n_modes].height = (uint16_t)cur_h;
    g_edid.modes[g_edid.n_modes].refresh_hz = 60;
    g_edid.modes[g_edid.n_modes].preferred = true;
    g_edid.n_modes++;
    for (int i = 0; i < (int)(sizeof(common)/sizeof(common[0])) &&
         g_edid.n_modes < EDID_MAX_MODES; i++) {
        if (common[i].w == cur_w && common[i].h == cur_h) continue;
        g_edid.modes[g_edid.n_modes].width  = common[i].w;
        g_edid.modes[g_edid.n_modes].height = common[i].h;
        g_edid.modes[g_edid.n_modes].refresh_hz = common[i].hz;
        g_edid.modes[g_edid.n_modes].preferred = false;
        g_edid.n_modes++;
    }
    debug_printf("[edid] synthesised %d mode(s), native=%ux%u\n",
                 g_edid.n_modes, cur_w, cur_h);
    if (out) *out = g_edid;
    return true;
}

bool edid_apply(uint16_t width, uint16_t height) {
    debug_printf("[edid] runtime mode switch to %ux%u requested\n", width, height);
    return vga_switch_mode(width, height);
}
