/* ============================================================================
 * NexxoN OS - VESA / VGA framebuffer driver  (v4.0)
 * ----------------------------------------------------------------------------
 * Sets up the global "screen" draw_target_t from the multiboot framebuffer
 * descriptor, then delegates every drawing primitive to gfx.c.  Callers
 * that don't (yet) thread a target through their API keep working via the
 * vga_* shims at the bottom of this file.
 * ============================================================================ */
#include "vga.h"
#include "gfx.h"
#include "vbe_table.h"
#include "string.h"
#include "debug.h"
#include "pit.h"
#include "boot_info.h"

extern int  vbe_tramp_switch(uint16_t mode_num);
extern int  vbe_tramp_switch_res(uint16_t w, uint16_t h);
extern void vbe_tramp_get_result(uint32_t *phys, uint16_t *w, uint16_t *h,
                                 uint16_t *pitch);

#define FALLBACK_FB_ADDR    ((uint8_t *)0xFD000000)

/* ---- Bochs Graphics Adapter (BGA) direct I/O mode switch -------------- */
#define BGA_INDEX_PORT 0x01CE
#define BGA_DATA_PORT  0x01CF
#define BGA_ID         0x00
#define BGA_XRES       0x01
#define BGA_YRES       0x02
#define BGA_BPP        0x03
#define BGA_ENABLE     0x04

#define BGA_ENABLE_ON       0x01
#define BGA_ENABLE_LFB      0x40

static inline void bga_write(uint16_t reg, uint16_t val) {
    __asm__ volatile ("outw %0, %1" :: "a"(reg), "Nd"((uint16_t)BGA_INDEX_PORT));
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"((uint16_t)BGA_DATA_PORT));
}
static inline uint16_t bga_read(uint16_t reg) {
    __asm__ volatile ("outw %0, %1" :: "a"(reg), "Nd"((uint16_t)BGA_INDEX_PORT));
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"((uint16_t)BGA_DATA_PORT));
    return val;
}

static bool bga_present(void) {
    uint16_t id = bga_read(BGA_ID);
    return (id >= 0xB0C0 && id <= 0xB0CF);
}

/* `forcetramp` kernel cmdline flag: pretend the BGA isn't there so QEMU
 * exercises the REAL bare-metal path (VBE trampoline + firmware mode enum).
 * QEMU passing on the BGA path proved nothing about the trampoline — this is
 * the §3 lesson "force bare-metal-only paths in QEMU" made permanent. */
static bool tramp_forced(void) {
    return boot_info_cmdline_has("forcetramp");
}

bool vga_bga_available(void) { return !tramp_forced() && bga_present(); }

static bool bga_switch(uint16_t w, uint16_t h) {
    if (!bga_present()) return false;
    bga_write(BGA_ENABLE, 0);
    bga_write(BGA_XRES, w);
    bga_write(BGA_YRES, h);
    bga_write(BGA_BPP, 32);
    bga_write(BGA_ENABLE, BGA_ENABLE_ON | BGA_ENABLE_LFB);
    uint16_t check_w = bga_read(BGA_XRES);
    uint16_t check_h = bga_read(BGA_YRES);
    debug_printf("[vga] BGA set %ux%u, readback %ux%u\n", w, h, check_w, check_h);
    return (check_w == w && check_h == h);
}

static draw_target_t *g_screen = NULL;

/* Runtime-detected pixel-format positions.  Multiboot's color_info
 * tells us where Red / Green / Blue sit in each 32-bit pixel.  We
 * default to standard XRGB8888 (R @ 16, G @ 8, B @ 0) which matches
 * QEMU `-vga std` and the vast majority of VBE BIOSes.  When the
 * multiboot record carries different positions (e.g. some BGR-layout
 * VirtualBox 3D modes), we honour them.  Browser pixel conversion
 * and any code that hand-rolls a uint32_t pixel use these globals via
 * the vga_*_pos() / vga_compose_pixel() helpers. */
static uint8_t g_red_pos   = 16;
static uint8_t g_green_pos = 8;
static uint8_t g_blue_pos  = 0;

uint8_t  vga_red_pos        (void) { return g_red_pos;   }
uint8_t  vga_green_pos      (void) { return g_green_pos; }
uint8_t  vga_blue_pos       (void) { return g_blue_pos;  }
bool     vga_is_native_argb (void) {
    return g_red_pos == 16 && g_green_pos == 8 && g_blue_pos == 0;
}
/* Compose a hardware pixel from 8-bit R/G/B channels.  Same return
 * value as the ARGB literal on a native-XRGB framebuffer, but produces
 * the correct word for any other detected layout. */
uint32_t vga_compose_pixel  (uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u |
           ((uint32_t)r << g_red_pos)   |
           ((uint32_t)g << g_green_pos) |
           ((uint32_t)b << g_blue_pos);
}

bool vga_init(const multiboot_info_t *mbi) {
    g_screen = gfx_screen();
    bool fb_ok = false;

    /* Primary path: GRUB (or stage2) set MB_FLAG_FRAMEBUFFER and filled in
     * framebuffer_addr with the VBE PhysBasePtr.  This is the normal path
     * for both LIVE CD (GRUB + gfxpayload=keep) and HDD (stage2 MBI). */
    if (mbi && (mbi->flags & MB_FLAG_FRAMEBUFFER) && mbi->framebuffer_addr) {
        g_screen->fb     = (uint8_t *)(uintptr_t)mbi->framebuffer_addr;
        g_screen->width  = mbi->framebuffer_width;
        g_screen->height = mbi->framebuffer_height;
        g_screen->pitch  = mbi->framebuffer_pitch;
        debug_printf("[vga] fb=0x%x  %ux%u@%u  pitch=%u\n",
                     (uint32_t)mbi->framebuffer_addr,
                     g_screen->width, g_screen->height,
                     mbi->framebuffer_bpp, g_screen->pitch);
        /* For direct-color framebuffers (type 1) the next 6 bytes of
         * color_info give us the channel positions.  Anything else
         * (palette, EGA text, ...) keeps the defaults.  SANITY-CHECK the
         * record before trusting it: QEMU/GRUB delivers a degenerate
         * "R@0 size 0" block for the standard XRGB mode, and honouring it
         * R/B-swapped every browser frame (the page rendered green/orange)
         * while the rest of the UI — which writes XRGB literals directly —
         * looked fine.  A direct-colour 32bpp field is only believable if
         * each channel is 8 bits wide and the positions are distinct. */
        if (mbi->framebuffer_type == 1) {
            uint8_t rp = mbi->color_info[0], rs = mbi->color_info[1];
            uint8_t gp = mbi->color_info[2], gs = mbi->color_info[3];
            uint8_t bp = mbi->color_info[4], bs = mbi->color_info[5];
            bool sane = rs == 8 && gs == 8 && bs == 8 &&
                        rp <= 24 && gp <= 24 && bp <= 24 &&
                        rp != gp && gp != bp && rp != bp;
            if (sane) {
                g_red_pos   = rp;
                g_green_pos = gp;
                g_blue_pos  = bp;
            }
            debug_printf("[vga] pixel format: R@%u G@%u B@%u (sizes %u/%u/%u)%s\n",
                         rp, gp, bp, rs, gs, bs,
                         sane ? "" : "  IGNORED (degenerate) -> XRGB8888");
        }
        fb_ok = true;
    }

    /* Fallback: GRUB filled in vbe_mode_info but not framebuffer_addr.
     * This happens on some VMware configurations where the VBE BIOS returns
     * PhysBasePtr=0 yet still provides a valid mode-info block.  Read the
     * framebuffer address directly from VBE mode info offset 0x28. */
    if (!fb_ok && mbi && mbi->vbe_mode_info) {
        const uint8_t *vmi = (const uint8_t *)(uintptr_t)mbi->vbe_mode_info;
        uint32_t phys_base = *(const uint32_t *)(vmi + 0x28);
        uint16_t bps  = *(const uint16_t *)(vmi + 0x10);
        uint16_t xres = *(const uint16_t *)(vmi + 0x12);
        uint16_t yres = *(const uint16_t *)(vmi + 0x14);
        debug_printf("[vga] vbe_mode_info=0x%x: phys=0x%x %ux%u pitch=%u\n",
                     mbi->vbe_mode_info, phys_base, xres, yres, bps);
        if (phys_base) {
            g_screen->fb     = (uint8_t *)(uintptr_t)phys_base;
            g_screen->width  = xres ? xres : VGA_W_DEFAULT;
            g_screen->height = yres ? yres : VGA_H_DEFAULT;
            g_screen->pitch  = bps  ? bps  : VGA_W_DEFAULT * 4;
            fb_ok = true;
        }
    }

    if (!fb_ok) {
        g_screen->fb     = FALLBACK_FB_ADDR;
        g_screen->width  = VGA_W_DEFAULT;
        g_screen->height = VGA_H_DEFAULT;
        g_screen->pitch  = VGA_W_DEFAULT * 4;
        debug_printf("[vga] hardcoded fallback: fb=0x%x %ux%u\n",
                     (uint32_t)(uintptr_t)FALLBACK_FB_ADDR,
                     g_screen->width, g_screen->height);
    }

    gfx_clear(g_screen, VGA_BLACK);
    return g_screen->fb != NULL;
}

uint32_t vga_width   (void) { return g_screen ? g_screen->width  : 0; }
uint32_t vga_height  (void) { return g_screen ? g_screen->height : 0; }
uint32_t vga_bpp     (void) { return VGA_BPP; }
void    *vga_framebuffer(void) { return g_screen ? g_screen->fb : NULL; }

/* ---- Resolution-change listeners ---------------------------------------- */
#define VGA_MAX_RES_LISTENERS 8
static vga_res_listener_t g_res_listeners[VGA_MAX_RES_LISTENERS];
static int g_n_res_listeners = 0;

void vga_register_res_listener(vga_res_listener_t cb) {
    if (!cb || g_n_res_listeners >= VGA_MAX_RES_LISTENERS) return;
    for (int i = 0; i < g_n_res_listeners; i++)
        if (g_res_listeners[i] == cb) return;
    g_res_listeners[g_n_res_listeners++] = cb;
}

static void fire_res_changed(uint32_t w, uint32_t h) {
    for (int i = 0; i < g_n_res_listeners; i++)
        if (g_res_listeners[i]) g_res_listeners[i](w, h);
}

/* ---- Runtime VBE mode switching ----------------------------------------- */
bool vga_switch_mode(uint16_t width, uint16_t height) {
    if (!g_screen) return false;
    if (width == (uint16_t)g_screen->width &&
        height == (uint16_t)g_screen->height) return true;

    debug_printf("[vga] switching to %ux%u ...\n", width, height);

    /* Strategy 1: BGA direct I/O (QEMU, Bochs, VirtualBox) */
    if (!tramp_forced() && bga_switch(width, height)) {
        uint8_t *old_fb = g_screen->fb;
        g_screen->width  = width;
        g_screen->height = height;
        g_screen->pitch  = (uint32_t)width * 4;
        debug_printf("[vga] BGA switch OK  fb=0x%x  %ux%u  pitch=%u\n",
                     (uint32_t)(uintptr_t)old_fb, width, height, g_screen->pitch);
        gfx_clear(g_screen, VGA_BLACK);
        fire_res_changed(width, height);
        return true;
    }

    /* Strategy 2: VBE real-mode trampoline (bare metal).  Ask the BIOS which
     * mode is exactly width x height x 32bpp with a linear framebuffer and set
     * THAT, instead of guessing a VESA mode number — the hardcoded numbers
     * differ per firmware and caused "switch failed" (and back-switch/revert
     * failures) plus sheared modes on the user's real BIOS. */
    debug_printf("[vga] VBE: probing BIOS for %ux%u x32 ...\n", width, height);
    /* Instrument the BIOS round-trip: the trampoline runs with the kernel
     * IDT swapped out, so PIT ticks are lost while the video BIOS works, and
     * some firmware leaves channel 0 reprogrammed.  Log the tick counter
     * around the call and re-assert our 100 Hz rate immediately after —
     * otherwise every later pit_ms() timeout (the 12 s keep/revert grace
     * included) runs at firmware speed, not ours. */
    uint32_t tick0 = pit_ticks();
    int rc = vbe_tramp_switch_res(width, height);
    pit_reprogram();
    debug_printf("[vga] trampoline round-trip: ticks %u -> %u (rc=%d)\n",
                 tick0, pit_ticks(), rc);
    if (rc != 0) {
        debug_printf("[vga] no BIOS VBE mode for %ux%u (rc=%d)\n",
                     width, height, rc);
        return false;
    }

    uint32_t phys = 0;
    uint16_t rw = 0, rh = 0, rp = 0;
    vbe_tramp_get_result(&phys, &rw, &rh, &rp);

    if (!phys || !rw || !rh) {
        debug_printf("[vga] mode switch returned invalid fb info\n");
        return false;
    }

    g_screen->fb     = (uint8_t *)(uintptr_t)phys;
    g_screen->width  = rw;
    g_screen->height = rh;
    g_screen->pitch  = rp ? rp : (uint32_t)rw * 4;

    debug_printf("[vga] VBE switch OK  fb=0x%x  %ux%u  pitch=%u\n",
                 phys, rw, rh, g_screen->pitch);

    /* The VBE LFB lives at a NEW physical address than the boot mode, so the
     * boot-time PAT/MTRR write-combining range no longer covers it.  Re-tag
     * the new LFB as Write-Combining or every blit at the new resolution
     * crawls over uncached writes (the user's "responsiveness must not drop
     * when the resolution is raised"). */
    {
        extern bool vmm_set_pat_write_combining(uint32_t, uint32_t);
        (void)vmm_set_pat_write_combining(phys, (uint32_t)g_screen->pitch * rh);
    }

    gfx_clear(g_screen, VGA_BLACK);
    fire_res_changed(rw, rh);
    return true;
}

/* --- thin shims that still take (x, y) coordinates against the screen --- */
void vga_putpixel   (int x, int y, vga_color_t c)              { gfx_putpixel  (g_screen, x, y, c); }
void vga_fill_rect  (int x, int y, int w, int h, vga_color_t c){ gfx_fill_rect (g_screen, x, y, w, h, c); }
void vga_clear      (vga_color_t c)                            { gfx_clear     (g_screen, c); }
void vga_draw_char  (int x, int y, char c, vga_color_t f, vga_color_t b) { gfx_draw_char(g_screen, x, y, c, f, b); }
void vga_draw_string(int x, int y, const char *s, vga_color_t f, vga_color_t b) { gfx_draw_string(g_screen, x, y, s, f, b); }
void vga_scroll_up  (int pixels, vga_color_t bg)               { gfx_scroll_up (g_screen, pixels, bg); }
