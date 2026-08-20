/* ============================================================================
 * NexxoN OS - RTC-driven task scheduler (implementation)
 * ----------------------------------------------------------------------------
 * Stores up to TASKTIMER_MAX_TASKS scheduled NXScript / shell lines, each
 * keyed to a "Unix-style" absolute seconds counter sourced from the CMOS
 * RTC (see rtc.c).  Polled once per wall-clock second from the shell's
 * idle loop.
 *
 * The executor delegates to NXScript directly: every shell command also
 * round-trips through the same dispatcher when not a builtin keyword, so
 * scheduling either is uniform for the user.  Keeping the executor in
 * NXScript also means a script can legally schedule another script -
 * useful for periodic tasks that re-arm themselves.
 * ============================================================================ */
#include "tasktimer.h"
#include "rtc.h"
#include "nxscript.h"
#include "terminal.h"
#include "vga.h"
#include "string.h"
#include "debug.h"

typedef struct {
    bool     in_use;
    int      id;
    uint32_t target_seconds;
    char     payload[TASKTIMER_PAYLOAD_MAX];
} tt_slot_t;

static tt_slot_t g_slots[TASKTIMER_MAX_TASKS];
static int       g_next_id      = 1;
static uint32_t  g_last_poll    = 0;  /* RTC second of last tick eval */
static bool      g_initialised  = false;

/* ---------- Init -------------------------------------------------------- */
void tasktimer_init(void) {
    memset(g_slots, 0, sizeof(g_slots));
    g_next_id     = 1;
    g_last_poll   = rtc_seconds();
    g_initialised = true;
    debug_ok("tasktimer: scheduler initialised");
}

/* ---------- Slot helpers ----------------------------------------------- */
static tt_slot_t *find_free_slot(void) {
    for (int i = 0; i < TASKTIMER_MAX_TASKS; i++) {
        if (!g_slots[i].in_use) return &g_slots[i];
    }
    return NULL;
}

static tt_slot_t *find_by_id(int id) {
    for (int i = 0; i < TASKTIMER_MAX_TASKS; i++) {
        if (g_slots[i].in_use && g_slots[i].id == id) return &g_slots[i];
    }
    return NULL;
}

/* ---------- Public scheduling API -------------------------------------- */
int tasktimer_schedule_at(uint8_t hour, uint8_t minute, uint8_t second,
                          const char *payload) {
    if (!payload || !payload[0])                   return TT_ERR_PAYLOAD;
    if (strlen(payload) >= TASKTIMER_PAYLOAD_MAX)  return TT_ERR_PAYLOAD;
    if (hour > 23 || minute > 59 || second > 59)   return TT_ERR_TIME;

    /* Compute today's target as an absolute rtc_seconds value.  rtc_seconds
     * is monotonic and includes today's elapsed H/M/S, so we just subtract
     * today's H/M/S contribution and add the requested one; if that's
     * already in the past, roll forward by 86400 (= one whole day).      */
    rtc_time_t now;
    rtc_now(&now);
    uint32_t now_secs  = rtc_seconds();
    uint32_t today_hms = (uint32_t)now.hour   * 3600u
                       + (uint32_t)now.minute * 60u
                       + (uint32_t)now.second;
    uint32_t want_hms  = (uint32_t)hour   * 3600u
                       + (uint32_t)minute * 60u
                       + (uint32_t)second;
    uint32_t target_secs = now_secs - today_hms + want_hms;
    if (target_secs <= now_secs) target_secs += 86400u;     /* tomorrow */

    tt_slot_t *s = find_free_slot();
    if (!s) return TT_ERR_FULL;

    s->in_use         = true;
    s->id             = g_next_id++;
    s->target_seconds = target_secs;
    strncpy(s->payload, payload, sizeof(s->payload) - 1);
    s->payload[sizeof(s->payload) - 1] = 0;

    debug_printf("[tasktimer] +id=%d  at=%02u:%02u:%02u  (in %us)  payload='%s'\n",
                 s->id, hour, minute, second,
                 target_secs - now_secs, s->payload);
    return s->id;
}

int tasktimer_schedule_in(uint32_t offset_seconds, const char *payload) {
    if (!payload || !payload[0])                   return TT_ERR_PAYLOAD;
    if (strlen(payload) >= TASKTIMER_PAYLOAD_MAX)  return TT_ERR_PAYLOAD;
    if (offset_seconds == 0)                       return TT_ERR_TIME;

    tt_slot_t *s = find_free_slot();
    if (!s) return TT_ERR_FULL;

    uint32_t now_secs = rtc_seconds();
    s->in_use         = true;
    s->id             = g_next_id++;
    s->target_seconds = now_secs + offset_seconds;
    strncpy(s->payload, payload, sizeof(s->payload) - 1);
    s->payload[sizeof(s->payload) - 1] = 0;

    debug_printf("[tasktimer] +id=%d  in=%us  target_secs=%u  payload='%s'\n",
                 s->id, offset_seconds, s->target_seconds, s->payload);
    return s->id;
}

int tasktimer_cancel(int id) {
    tt_slot_t *s = find_by_id(id);
    if (!s) return TT_ERR_NOTFOUND;
    s->in_use = false;
    s->payload[0] = 0;
    debug_printf("[tasktimer] -id=%d  (cancelled)\n", id);
    return TT_OK;
}

int tasktimer_clear(void) {
    int n = 0;
    for (int i = 0; i < TASKTIMER_MAX_TASKS; i++) {
        if (g_slots[i].in_use) { g_slots[i].in_use = false; n++; }
    }
    return n;
}

void tasktimer_iterate(tasktimer_iter_cb_t cb, void *user) {
    if (!cb) return;
    for (int i = 0; i < TASKTIMER_MAX_TASKS; i++) {
        if (g_slots[i].in_use) {
            if (!cb(g_slots[i].id, g_slots[i].target_seconds,
                    g_slots[i].payload, user)) return;
        }
    }
}

int tasktimer_count(void) {
    int n = 0;
    for (int i = 0; i < TASKTIMER_MAX_TASKS; i++) {
        if (g_slots[i].in_use) n++;
    }
    return n;
}

/* ---------- Tick: evaluate triggers ------------------------------------ *
 * Polled once per wall-clock second.  Reading the CMOS RTC twice per
 * second is cheap (~8 port reads), well below the 8042 PS/2 cost the
 * shell already eats on every keypress.  We rate-limit to once per
 * second so multiple shell-idle iterations between RTC rollovers don't
 * call into NXScript repeatedly for the same trigger.                   */
static void fire(tt_slot_t *s) {
    /* Mark released BEFORE the script runs so a re-entrant schedule from
     * inside the payload doesn't see this slot as "still armed". */
    char    payload[TASKTIMER_PAYLOAD_MAX];
    int     id  = s->id;
    uint32_t at = s->target_seconds;
    strncpy(payload, s->payload, sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = 0;
    s->in_use   = false;
    s->payload[0] = 0;

    debug_printf("[tasktimer] FIRE id=%d at_secs=%u payload='%s'\n",
                 id, at, payload);

    /* Visible breadcrumb in the terminal so the user knows a scheduled
     * job ran - they may have switched windows since arming it. */
    vga_color_t fg, bg;
    term_get_color(&fg, &bg);
    term_set_color(VGA_MAGENTA, bg);
    term_printf("\n[tasktimer] firing task #%d: %s\n", id, payload);
    term_set_color(fg, bg);

    int r = nxscript_eval(payload);
    if (r != NX_OK) {
        term_set_color(VGA_RED, bg);
        term_printf("[tasktimer] task #%d error: %s\n",
                    id, nxscript_last_error());
        term_set_color(fg, bg);
    }
}

void tasktimer_tick(void) {
    if (!g_initialised) return;

    uint32_t now = rtc_seconds();
    if (now == g_last_poll) return;          /* sub-second wakeup, skip */
    g_last_poll = now;

    /* Snapshot ids/targets BEFORE iterating because fire() may both
     * release the slot AND re-schedule something into another slot,
     * which we don't want to re-evaluate inside the same tick. */
    int fired = 0;
    for (int i = 0; i < TASKTIMER_MAX_TASKS && fired < TASKTIMER_MAX_TASKS; i++) {
        if (g_slots[i].in_use && g_slots[i].target_seconds <= now) {
            fire(&g_slots[i]);
            fired++;
        }
    }
}
