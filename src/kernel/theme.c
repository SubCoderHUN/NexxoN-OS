/* ============================================================================
 * NexxoN OS - Theme engine implementation (TASK 31)
 * ---------------------------------------------------------------------------- */
#include "theme.h"
#include "debug.h"

static bool g_dark = false;

/* Two parallel palettes — index by theme_key_t.  Light is the legacy
 * default that matches the hardcoded values used before the theming
 * engine landed, so apps that haven't migrated yet still look right. */
static const uint32_t g_light[THEME_KEY_COUNT] = {
    [THEME_PANEL_TOP]    = 0xFF383848,
    [THEME_PANEL_BOT]    = 0xFF181820,
    [THEME_PANEL_FG]     = 0xFFE8E8F0,
    [THEME_MENU_BG]      = 0xFFE8E8EC,
    [THEME_MENU_FG]      = 0xFF101010,
    [THEME_MENU_HDR_TOP] = 0xFF002878,
    [THEME_MENU_HDR_BOT] = 0xFF003898,
    [THEME_WINDOW_BG]    = 0xFFF0F0F4,
    [THEME_WINDOW_FG]    = 0xFF101010,
    [THEME_ACCENT]       = 0xFF1850C8,
};

static const uint32_t g_dark_palette[THEME_KEY_COUNT] = {
    [THEME_PANEL_TOP]    = 0xFF101018,
    [THEME_PANEL_BOT]    = 0xFF000004,
    [THEME_PANEL_FG]     = 0xFFD0D0D8,
    [THEME_MENU_BG]      = 0xFF202028,
    [THEME_MENU_FG]      = 0xFFE0E0E8,
    [THEME_MENU_HDR_TOP] = 0xFF00204C,
    [THEME_MENU_HDR_BOT] = 0xFF002878,
    [THEME_WINDOW_BG]    = 0xFF1C1C24,
    [THEME_WINDOW_FG]    = 0xFFE8E8F0,
    [THEME_ACCENT]       = 0xFF4080FF,
};

void theme_init(void) {
    g_dark = false;
    debug_printf("[theme] light mode (default)\n");
}

void theme_set_dark(bool dark) {
    if (dark == g_dark) return;
    g_dark = dark;
    debug_printf("[theme] switched to %s mode\n", dark ? "dark" : "light");
}

bool theme_is_dark(void) { return g_dark; }

uint32_t theme_color(theme_key_t key) {
    if (key < 0 || key >= THEME_KEY_COUNT) return 0xFF000000;
    return g_dark ? g_dark_palette[key] : g_light[key];
}

/* Ease-out cubic in fixed point: progress p = elapsed/duration clamped to
 * [0,1], returns 256 * (1 - (1-p)^3).  All integer maths (no FPU in the
 * compositor path).  Returns 256 once the animation has elapsed so callers
 * can treat >=256 as "settled". */
uint32_t anim_ease_out_cubic(uint32_t elapsed_ms, uint32_t duration_ms) {
    if (duration_ms == 0 || elapsed_ms >= duration_ms) return 256u;
    /* inv = (1 - p) in 0..256 */
    uint32_t inv = 256u - (elapsed_ms * 256u) / duration_ms;   /* 0..256 */
    /* inv^3 / 256^2 -> back to 0..256, then 256 - that */
    uint32_t cube = (inv * inv) / 256u;
    cube = (cube * inv) / 256u;
    return 256u - cube;
}
