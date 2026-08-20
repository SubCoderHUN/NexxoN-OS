/* ============================================================================
 * NexxoN OS - Idle-detector screensaver  (v1.0)
 * ----------------------------------------------------------------------------
 * Drives a bouncing "NexxoN OS" logo when the system has been idle for
 * IDLE_TIMEOUT_MS milliseconds.  "Idle" means no keyboard IRQ1 packet and
 * no mouse IRQ12 packet has arrived during that window.  Both ISRs call
 * screensaver_kick() to refresh the last-activity timestamp.
 *
 * The saver paints directly to the back buffer (a regular WM tick still
 * runs underneath), grabs the modal lock so foreground windows don't
 * receive stray clicks, and releases on any input.  No window is created;
 * the WM's draw_target_t is overpainted post-compose every frame.
 * ============================================================================ */
#include "apps.h"
#include "window.h"
#include "gfx.h"
#include "font.h"
#include "vga.h"
#include "pit.h"
#include "debug.h"
#include "string.h"
#include "i18n.h"

#define IDLE_TIMEOUT_MS    60000u    /* 60 seconds before activation     */
#define LOGO_TEXT          "NexxoN OS"
#define LOGO_SCALE         5         /* 8x8 -> 40x40 per glyph           */
#define BG_COLOR           0xFF000000
#define LOGO_COLOR         0xFF40FF80
#define ACCENT_COLOR       0xFFFFE040

static uint32_t g_last_input_ms = 0;
static bool     g_active        = false;
static int      g_lx = 80, g_ly = 80;
static int      g_dx = 2,  g_dy = 2;
static uint32_t g_color = LOGO_COLOR;
static uint32_t g_last_tick_ms  = 0;

/* Boot kick: any input event before activation just notes the time. */
void screensaver_kick(void) {
    g_last_input_ms = pit_ms();
    if (g_active) {
        debug_printf("[ssave] deactivated\n");
        g_active = false;
        /* Force the compositor to repaint the desktop over the saver's
         * black frame on the next wm_tick (the WM skips compositing while
         * the saver is active, so without this the screen would stay black
         * until something else marked it dirty). */
        wm_mark_dirty();
    }
}

bool screensaver_active(void) { return g_active; }

static void draw_big_string(draw_target_t *t, int x, int y, const char *s,
                            int scale, uint32_t fg, uint32_t bg) {
    extern const uint8_t font8x8[256][8];
    int cx = x;
    while (*s) {
        const uint8_t *gly = font8x8[(uint8_t)*s];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = gly[row];
            for (int col = 0; col < 8; col++) {
                uint32_t c = (bits & (1u << col)) ? fg : bg;
                gfx_fill_rect(t, cx + col * scale, y + row * scale,
                              scale, scale, c);
            }
        }
        cx += 8 * scale;
        s++;
    }
}

void screensaver_tick(void) {
    uint32_t now = pit_ms();
    if (g_last_input_ms == 0) g_last_input_ms = now;

    if (!g_active) {
        if (now - g_last_input_ms < IDLE_TIMEOUT_MS) return;
        debug_printf("[ssave] activating after %u ms idle\n",
                     now - g_last_input_ms);
        g_active = true;
        g_lx = 100; g_ly = 100;
        g_dx = 2;   g_dy = 2;
        g_color = LOGO_COLOR;
        g_last_tick_ms = now;
    }

    /* Throttle: 50 FPS is plenty for a bouncing logo. */
    if (now - g_last_tick_ms < 20u) return;
    g_last_tick_ms = now;

    draw_target_t *s = gfx_screen();
    if (!s || !s->fb) return;
    int sw = (int)s->width;
    int sh = (int)s->height;
    int logo_w = (int)strlen(LOGO_TEXT) * 8 * LOGO_SCALE;
    int logo_h = 8 * LOGO_SCALE;

    /* Advance logo position. */
    g_lx += g_dx;
    g_ly += g_dy;
    if (g_lx < 0)               { g_lx = 0;               g_dx = -g_dx; g_color = ACCENT_COLOR; }
    if (g_ly < 0)               { g_ly = 0;               g_dy = -g_dy; g_color = ACCENT_COLOR; }
    if (g_lx + logo_w > sw)     { g_lx = sw - logo_w;     g_dx = -g_dx; g_color = LOGO_COLOR; }
    if (g_ly + logo_h > sh)     { g_ly = sh - logo_h;     g_dy = -g_dy; g_color = LOGO_COLOR; }

    /* Paint directly to the screen.  We don't bother running the WM
     * compositor while the saver is up - it'd just waste cycles. */
    gfx_clear(s, BG_COLOR);
    draw_big_string(s, g_lx, g_ly, LOGO_TEXT, LOGO_SCALE, g_color, BG_COLOR);
    /* Subtitle (localised). */
    const char *sub = L(STR_SCREENSAVER_HINT);
    int sub_w = gfx_string_pixel_width(sub);
    gfx_draw_string(s, sw / 2 - sub_w / 2, sh - 24,
                    sub, 0xFF808090, BG_COLOR);
}
