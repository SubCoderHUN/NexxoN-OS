/* ============================================================================
 * NexxoN OS - Notification center
 * ----------------------------------------------------------------------------
 * Four toast slots stacked at the bottom-right of the screen.  Each
 * slot carries a posted_ms timestamp - notify_tick() reclaims slots
 * whose age has exceeded NOTIFY_TIMEOUT_MS.  notify_draw() is the
 * compositor hook called once per frame after the main scene.
 * ============================================================================ */
#include "notify.h"
#include "gfx.h"
#include "font.h"
#include "vga.h"
#include "pit.h"
#include "theme.h"
#include "string.h"
#include "debug.h"
#include "window.h"   /* wm_mark_dirty() to drive the slide-in */

static notify_slot_t g_slots[NOTIFY_MAX];

void notify_init(void) {
    memset(g_slots, 0, sizeof(g_slots));
}

int notify_post(notify_kind_t kind, const char *title, const char *body) {
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!g_slots[i].in_use) {
            g_slots[i].in_use    = true;
            g_slots[i].kind      = kind;
            g_slots[i].posted_ms = pit_ms();
            if (title) {
                int n = 0;
                while (title[n] && n < NOTIFY_TITLE_MAX - 1) {
                    g_slots[i].title[n] = title[n]; n++;
                }
                g_slots[i].title[n] = 0;
            }
            if (body) {
                int n = 0;
                while (body[n] && n < NOTIFY_BODY_MAX - 1) {
                    g_slots[i].body[n] = body[n]; n++;
                }
                g_slots[i].body[n] = 0;
            }
            g_slots[i].action_label[0] = 0;
            g_slots[i].action_cb = NULL;
            debug_printf("[notify] %s: %s\n",
                         g_slots[i].title, g_slots[i].body);
            return i;
        }
    }
    /* Evict the oldest toast. */
    int victim = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (g_slots[i].posted_ms < oldest) { oldest = g_slots[i].posted_ms; victim = i; }
    }
    g_slots[victim].in_use = false;
    return notify_post(kind, title, body);
}

int notify_post_action(notify_kind_t kind, const char *title, const char *body,
                       const char *action_label, notify_action_cb_t action_cb) {
    int idx = notify_post(kind, title, body);
    if (idx < 0 || idx >= NOTIFY_MAX) return idx;
    if (action_label) {
        int n = 0;
        while (action_label[n] && n < NOTIFY_ACTION_LABEL_MAX - 1) {
            g_slots[idx].action_label[n] = action_label[n]; n++;
        }
        g_slots[idx].action_label[n] = 0;
    }
    g_slots[idx].action_cb = action_cb;
    return idx;
}

void notify_tick(void) {
    uint32_t now = pit_ms();
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (g_slots[i].in_use &&
            now - g_slots[i].posted_ms >= NOTIFY_TIMEOUT_MS) {
            g_slots[i].in_use = false;
        }
    }
}

int           notify_count(void)      { int n = 0; for (int i = 0; i < NOTIFY_MAX; i++) if (g_slots[i].in_use) n++; return n; }
notify_slot_t *notify_slot(int idx)   { return (idx < 0 || idx >= NOTIFY_MAX) ? NULL : &g_slots[idx]; }

/* ---- Compositor draw ------------------------------------------------- */
#define TOAST_W       320
#define TOAST_H        72   /* 16 px title + body + action button */
#define TOAST_PAD      8
#define TOAST_MARGIN  18
#define TOAST_BTN_H   16
#define TOAST_BTN_W   60

static uint32_t kind_color(notify_kind_t k) {
    switch (k) {
        case NOTIFY_INFO:    return 0xFF1850C8;
        case NOTIFY_WARNING: return 0xFFC09020;
        case NOTIFY_ERROR:   return 0xFFC02030;
        case NOTIFY_SUCCESS: return 0xFF208030;
    }
    return 0xFF606060;
}

void notify_draw(draw_target_t *t) {
    if (!t || !t->fb) return;
    uint32_t fg = 0xFFFFFFFF;
    uint32_t bg_base = theme_is_dark() ? 0xFF202020 : 0xFFF0F0F0;
    int y = (int)t->height - TOAST_H - TOAST_MARGIN - 30;   /* above taskbar */
    int x = (int)t->width  - TOAST_W - TOAST_MARGIN;
    uint32_t now = pit_ms();
    for (int i = 0; i < NOTIFY_MAX; i++) {
        notify_slot_t *s = &g_slots[i];
        if (!s->in_use) continue;
        uint32_t age = now - s->posted_ms;
        uint32_t alpha = 240;
        /* Fade out during last 500ms. */
        if (age > NOTIFY_TIMEOUT_MS - 500u) {
            alpha = (uint32_t)(240u *
                    (NOTIFY_TIMEOUT_MS - age) / 500u);
        }
        uint32_t accent = kind_color(s->kind) | (alpha << 24);
        /* Eased slide-in from the right edge during the first ANIM_WIN_MS. */
        int slide = 0;
        if (age < ANIM_WIN_MS) {
            int p = (int)anim_ease_out_cubic(age, ANIM_WIN_MS);   /* 0..256 */
            slide = (TOAST_W + TOAST_MARGIN) * (256 - p) / 256;
            wm_mark_dirty();
        }
        int xx = x + slide;
        /* Soft drop shadow + frosted-glass card (blur -> frost -> bevel). */
        gfx_drop_shadow_round(t, xx, y, TOAST_W, TOAST_H, RADIUS_PANEL, 8);
        gfx_glass_panel(t, xx, y, TOAST_W, TOAST_H, RADIUS_PANEL, GLASS_POPUP);
        /* Accent stripe on the left. */
        gfx_fill_round_rect(t, xx + 2, y + 4, 4, TOAST_H - 8, 2, accent);
        /* Title: 16 px chrome face, dark on the light frosted card. */
        gfx_draw_string_aa2x_clipped(t, xx + TOAST_PAD + 6, y + 7,
                                   TOAST_W - 2 * TOAST_PAD - 18,
                                   s->title, GLASS_TEXT_DARK, 0x00000000u);
        /* Body. */
        gfx_draw_string_clipped(t, xx + TOAST_PAD + 6, y + 30,
                                TOAST_W - 2 * TOAST_PAD - 10,
                                s->body, GLASS_TEXT_MUTED, 0x00000000u);
        /* Action button (if present). */
        if (s->action_label[0]) {
            int bx = xx + TOAST_W - TOAST_BTN_W - TOAST_PAD;
            int by = y + TOAST_H - TOAST_BTN_H - 4;
            gfx_draw_button_aero(t, bx, by, TOAST_BTN_W, TOAST_BTN_H,
                                 s->action_label, kind_color(s->kind), false);
        }
        /* Dismiss 'x' in top-right. */
        gfx_draw_string(t, xx + TOAST_W - 14, y + 5, "x",
                        GLASS_TEXT_MUTED, 0x00000000u);
        (void)fg; (void)bg_base;
        y -= (TOAST_H + 6);
    }
}

bool notify_handle_click(int sx, int sy) {
    extern uint32_t vga_width(void);
    extern uint32_t vga_height(void);
    int sw = (int)vga_width();
    int sh = (int)vga_height();
    int y = sh - TOAST_H - TOAST_MARGIN - 30;
    int x = sw - TOAST_W - TOAST_MARGIN;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        notify_slot_t *s = &g_slots[i];
        if (!s->in_use) { y -= (TOAST_H + 6); continue; }
        /* Dismiss 'x' hit area. */
        if (sx >= x + TOAST_W - 14 && sx < x + TOAST_W &&
            sy >= y && sy < y + 16) {
            s->in_use = false;
            return true;
        }
        /* Action button hit area. */
        if (s->action_label[0] && s->action_cb) {
            int bx = x + TOAST_W - TOAST_BTN_W - TOAST_PAD;
            int by = y + TOAST_H - TOAST_BTN_H - 4;
            if (sx >= bx && sx < bx + TOAST_BTN_W &&
                sy >= by && sy < by + TOAST_BTN_H) {
                notify_action_cb_t cb = s->action_cb;
                int idx = i;
                s->in_use = false;
                cb(idx);
                return true;
            }
        }
        y -= (TOAST_H + 6);
    }
    return false;
}
