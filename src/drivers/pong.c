/* ============================================================================
 * NexxoN OS - Pong  (v1.0)
 * ----------------------------------------------------------------------------
 * Classic two-paddle pong rendered inside a regular WM window.
 *   - User paddle (right):  follows the mouse cursor's vertical position
 *                           when the cursor is inside the play field.
 *   - AI paddle (left):     simple proportional tracking towards the ball
 *                           with a small reaction lag.
 *   - Ball:                 starts in the centre with random-ish velocity,
 *                           bounces off paddles and top/bottom walls, exits
 *                           past a paddle = the OTHER side scores.
 *   - First to 5 points wins; either side scoring re-centres the ball.
 *
 * The tick is driven from the shell's idle loop via pong_tick().  Each
 * tick advances physics by a fixed delta tied to PIT ms so the game runs
 * at a consistent speed regardless of compositor frame rate.
 * ============================================================================ */
#include "apps.h"
#include "window.h"
#include "gfx.h"
#include "font.h"
#include "vga.h"
#include "i18n.h"
#include "string.h"
#include "debug.h"
#include "mouse.h"
#include "pit.h"

#define PG_W            520
#define PG_H            360
#define PG_MIN_W        320
#define PG_MIN_H        220
#define PG_BG           0xFF000010
#define PG_FG           0xFFFFFFFF
#define PG_AI_COLOR     0xFFFF6060
#define PG_USR_COLOR    0xFF60D0FF
#define PG_NET          0xFF606078
#define PG_BALL         0xFFFFE040
#define PG_PADDLE_H     56
#define PG_PADDLE_W     8
#define PG_BALL_SZ      8
#define PG_WIN_SCORE    5
#define PG_TICK_MS      16          /* ~60 game updates per second */

typedef enum { PG_PLAYING, PG_WON_AI, PG_WON_USR } pong_state_t;

static window_t *g_pong_win = NULL;
static int       g_ball_x, g_ball_y;
static int       g_ball_dx, g_ball_dy;
static int       g_ai_y;
static int       g_usr_y;
static int       g_score_ai;
static int       g_score_usr;
static uint32_t  g_last_tick_ms;
static pong_state_t g_state;
static uint32_t  g_rng = 0xdeadbeefu;

static int pong_xorshift(void) {
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return (int)(x & 0x7fffffffu);
}

static void pong_reset_ball(int towards_player) {
    if (!g_pong_win || !g_pong_win->in_use) return;
    int fw = (int)g_pong_win->content.width;
    int fh = (int)g_pong_win->content.height;
    g_ball_x = fw / 2 - PG_BALL_SZ / 2;
    g_ball_y = fh / 2 - PG_BALL_SZ / 2;
    int speed = 3;
    g_ball_dx = (towards_player ? speed : -speed);
    g_ball_dy = ((pong_xorshift() & 1) ? speed : -speed);
}

static void pong_redraw(void);

static bool pong_click(window_t *w, int cx, int cy, uint8_t pressed,
                       uint8_t btn) {
    (void)cy; (void)btn;
    if (w != g_pong_win) return false;
    if (!(pressed & MOUSE_BTN_LEFT)) return true;
    if (g_state != PG_PLAYING) {
        /* Click restarts the match. */
        g_score_ai = g_score_usr = 0;
        g_state = PG_PLAYING;
        pong_reset_ball((cx & 1));
        pong_redraw();
    }
    return true;
}

static void pong_resize_cb(window_t *w) {
    int fh = (int)w->content.height;
    if (g_ai_y  + PG_PADDLE_H > fh) g_ai_y  = fh - PG_PADDLE_H;
    if (g_usr_y + PG_PADDLE_H > fh) g_usr_y = fh - PG_PADDLE_H;
    pong_redraw();
}
static void pong_destroy_cb(window_t *w) {
    if (w == g_pong_win) g_pong_win = NULL;
}

bool pong_open(void) {
    if (g_pong_win && g_pong_win->in_use) {
        wm_set_focus(g_pong_win);
        pong_redraw();
        return true;
    }
    g_pong_win = wm_create_window(100, 60, PG_W, PG_H, L(STR_APP_PONG));
    if (!g_pong_win) return false;
    wm_set_content_click(g_pong_win, pong_click, NULL);
    wm_set_resizable(g_pong_win, true, PG_MIN_W, PG_MIN_H);
    wm_set_resize_cb(g_pong_win, pong_resize_cb);
    wm_set_destroy_cb(g_pong_win, pong_destroy_cb);
    g_ai_y  = (PG_H - PG_PADDLE_H) / 2;
    g_usr_y = (PG_H - PG_PADDLE_H) / 2;
    g_score_ai = g_score_usr = 0;
    g_state = PG_PLAYING;
    g_last_tick_ms = pit_ms();
    g_rng ^= pit_ms() * 1664525u + 1013904223u;
    pong_reset_ball((pong_xorshift() & 1));
    pong_redraw();
    return true;
}

static void pong_step_physics(int fw, int fh) {
    if (g_state != PG_PLAYING) return;

    /* User paddle follows the mouse Y within the play field. */
    int mx = mouse_x(), my = mouse_y();
    int abs_top    = g_pong_win->y + WM_BORDER + WM_TITLE_H + 2;
    int abs_left   = g_pong_win->x + WM_BORDER;
    if (mx >= abs_left && mx < abs_left + fw) {
        int local_y = my - abs_top;
        g_usr_y = local_y - PG_PADDLE_H / 2;
    }
    if (g_usr_y < 0) g_usr_y = 0;
    if (g_usr_y + PG_PADDLE_H > fh) g_usr_y = fh - PG_PADDLE_H;

    /* AI tracks the ball with a small reaction step. */
    int target = g_ball_y + PG_BALL_SZ / 2 - PG_PADDLE_H / 2;
    int diff = target - g_ai_y;
    int max_step = 4;
    if (diff > max_step) diff = max_step;
    if (diff < -max_step) diff = -max_step;
    g_ai_y += diff;
    if (g_ai_y < 0) g_ai_y = 0;
    if (g_ai_y + PG_PADDLE_H > fh) g_ai_y = fh - PG_PADDLE_H;

    /* Move the ball. */
    g_ball_x += g_ball_dx;
    g_ball_y += g_ball_dy;

    if (g_ball_y < 0)                  { g_ball_y = 0; g_ball_dy = -g_ball_dy; }
    if (g_ball_y + PG_BALL_SZ > fh)    { g_ball_y = fh - PG_BALL_SZ; g_ball_dy = -g_ball_dy; }

    /* AI paddle collision (left). */
    if (g_ball_x < PG_PADDLE_W + 2 &&
        g_ball_y + PG_BALL_SZ >= g_ai_y &&
        g_ball_y <= g_ai_y + PG_PADDLE_H) {
        g_ball_dx = -g_ball_dx;
        g_ball_x  = PG_PADDLE_W + 2;
        /* Add a slight angle change based on where it hit the paddle. */
        int hit = (g_ball_y + PG_BALL_SZ / 2) - (g_ai_y + PG_PADDLE_H / 2);
        g_ball_dy = hit / 6;
        if (g_ball_dy == 0) g_ball_dy = (pong_xorshift() & 1) ? 2 : -2;
    }

    /* User paddle collision (right). */
    int rp_x = fw - PG_PADDLE_W - 2;
    if (g_ball_x + PG_BALL_SZ > rp_x &&
        g_ball_y + PG_BALL_SZ >= g_usr_y &&
        g_ball_y <= g_usr_y + PG_PADDLE_H) {
        g_ball_dx = -g_ball_dx;
        g_ball_x  = rp_x - PG_BALL_SZ;
        int hit = (g_ball_y + PG_BALL_SZ / 2) - (g_usr_y + PG_PADDLE_H / 2);
        g_ball_dy = hit / 6;
        if (g_ball_dy == 0) g_ball_dy = (pong_xorshift() & 1) ? 2 : -2;
    }

    /* Out of bounds left = user scored; right = AI scored. */
    if (g_ball_x + PG_BALL_SZ < 0) {
        g_score_usr++;
        pong_reset_ball(0);
    } else if (g_ball_x > fw) {
        g_score_ai++;
        pong_reset_ball(1);
    }

    if (g_score_ai >= PG_WIN_SCORE) g_state = PG_WON_AI;
    if (g_score_usr >= PG_WIN_SCORE) g_state = PG_WON_USR;
}

static void pong_redraw(void) {
    if (!g_pong_win || !g_pong_win->in_use) { g_pong_win = NULL; return; }
    draw_target_t *t = &g_pong_win->content;
    int fw = (int)t->width;
    int fh = (int)t->height;

    gfx_clear(t, PG_BG);

    /* Centre net (dashed vertical). */
    for (int y = 0; y < fh; y += 12) {
        gfx_fill_rect(t, fw / 2 - 1, y, 2, 6, PG_NET);
    }

    /* Paddles. */
    gfx_fill_rect(t, 2,             g_ai_y,  PG_PADDLE_W, PG_PADDLE_H, PG_AI_COLOR);
    gfx_fill_rect(t, fw - PG_PADDLE_W - 2, g_usr_y, PG_PADDLE_W, PG_PADDLE_H, PG_USR_COLOR);

    /* Ball. */
    gfx_fill_rect(t, g_ball_x, g_ball_y, PG_BALL_SZ, PG_BALL_SZ, PG_BALL);

    /* Score. */
    char score[24];
    ksnprintf(score, sizeof(score), "%d   :   %d", g_score_ai, g_score_usr);
    int sw = (int)strlen(score) * FONT_GLYPH_W;
    gfx_draw_string(t, fw / 2 - sw / 2, 6, score, PG_FG, PG_BG);

    if (g_state != PG_PLAYING) {
        const char *msg = (g_state == PG_WON_USR)
                          ? "YOU WIN!  Click to play again."
                          : "AI WINS.  Click to retry.";
        int w = (int)strlen(msg) * FONT_GLYPH_W;
        gfx_fill_rect(t, fw / 2 - w / 2 - 8, fh / 2 - 12,
                      w + 16, 24, 0xFF202040);
        gfx_draw_rect(t, fw / 2 - w / 2 - 8, fh / 2 - 12,
                      w + 16, 24, PG_FG);
        gfx_draw_string(t, fw / 2 - w / 2, fh / 2 - FONT_GLYPH_H / 2,
                        msg, PG_FG, 0xFF202040);
    }
    wm_mark_dirty();
}

void pong_tick(void) {
    if (!g_pong_win) return;
    if (!g_pong_win->in_use) { g_pong_win = NULL; return; }
    uint32_t now = pit_ms();
    if (now - g_last_tick_ms < PG_TICK_MS) return;
    g_last_tick_ms = now;
    pong_step_physics((int)g_pong_win->content.width,
                      (int)g_pong_win->content.height);
    pong_redraw();
}
