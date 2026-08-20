/* ============================================================================
 * NexxoN OS - CMOS Real-Time Clock driver (implementation)
 * ----------------------------------------------------------------------------
 * Pure polling driver - no IRQ8 plumbing.  The tasktimer ticks once per
 * second from the shell idle loop, which is plenty for human-scale
 * scheduling and avoids the complications of an extra periodic IRQ
 * (which would need its own ack via reading CMOS register 0x0C).
 * ============================================================================ */
#include "rtc.h"
#include "io.h"
#include "string.h"
#include "debug.h"

/* ---------- CMOS port + register layout --------------------------------- */
#define CMOS_ADDR       0x70
#define CMOS_DATA       0x71
#define CMOS_NMI_OFF    0x80    /* OR'd into the address byte to hold NMI
                                   disabled while we hold the index */

#define RTC_SECONDS     0x00
#define RTC_MINUTES     0x02
#define RTC_HOURS       0x04
#define RTC_WEEKDAY     0x06
#define RTC_DAY         0x07
#define RTC_MONTH       0x08
#define RTC_YEAR        0x09
#define RTC_STATUS_A    0x0A
#define RTC_STATUS_B    0x0B
#define RTC_CENTURY     0x32    /* legacy hint - many machines also expose
                                   the century byte through the FADT */

#define RTC_A_UIP       0x80
#define RTC_B_24HR      0x02
#define RTC_B_BINARY    0x04
#define RTC_HOUR_PM     0x80    /* high bit of hours byte in 12-hour mode */

/* ---------- Cached configuration (sampled once at init) ----------------- */
static uint8_t  g_status_b      = 0;
static bool     g_use_bcd       = true;
static bool     g_use_12hr      = false;
static bool     g_have_century  = false;
static uint16_t g_fallback_year = 2025;     /* used if century byte = 0 */

/* ---------- Low-level CMOS access --------------------------------------- *
 * Always set the NMI-disable bit (0x80) in the address byte so that an NMI
 * arriving between the index write and the data read can't catch us with
 * a stale index loaded.  The companion outb_idx_safe() leaves the CMOS
 * pointing at register 0x0D (a harmless one whose value the BIOS / OS
 * never depend on) so subsequent ad-hoc CMOS reads from other subsystems
 * see a stable starting state. */
static inline uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, (uint8_t)(reg | CMOS_NMI_OFF));
    io_wait();
    return inb(CMOS_DATA);
}

static inline void cmos_park(void) {
    /* Park the index at 0x0D (the date alarm flag register - unused on
     * any modern setup).  Keep NMI disabled; re-enabling is the BIOS's
     * job during the next #NMI vector dispatch. */
    outb(CMOS_ADDR, (uint8_t)(0x0D | CMOS_NMI_OFF));
}

/* ---------- Helpers ----------------------------------------------------- */
static inline uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)(((v >> 4) & 0x0F) * 10 + (v & 0x0F));
}

static inline uint8_t maybe_bcd_to_bin(uint8_t v) {
    return g_use_bcd ? bcd_to_bin(v) : v;
}

/* Wait until Status A bit 7 (Update-In-Progress) is clear.  No timeout:
 * the RTC takes at most ~2 ms to finish an update, and if it's wedged
 * that's a hardware problem the user can see from the resulting hang
 * with a recognisable last-message banner. */
static void wait_not_uip(void) {
    /* If UIP is already set, the RTC is mid-update; wait for it to clear.
     * Then there's a guaranteed window of ~244 us before the next update
     * starts, which is more than enough to read all seven byte registers. */
    while (cmos_read(RTC_STATUS_A) & RTC_A_UIP) { /* spin */ }
}

/* ---------- Raw read of all date/time fields ---------------------------- *
 * Implements the "double read" technique: after the first set of seven
 * reads, re-sample and compare.  If anything changed (because a one-
 * second rollover happened mid-read) we sample again.  Two consecutive
 * agreeing reads guarantee a coherent snapshot.                          */
typedef struct {
    uint8_t sec, min, hr, day, mon, yr, cent, wd;
} rtc_raw_t;

static void read_raw_once(rtc_raw_t *r) {
    wait_not_uip();
    r->sec  = cmos_read(RTC_SECONDS);
    r->min  = cmos_read(RTC_MINUTES);
    r->hr   = cmos_read(RTC_HOURS);
    r->wd   = cmos_read(RTC_WEEKDAY);
    r->day  = cmos_read(RTC_DAY);
    r->mon  = cmos_read(RTC_MONTH);
    r->yr   = cmos_read(RTC_YEAR);
    r->cent = g_have_century ? cmos_read(RTC_CENTURY) : 0;
}

static void read_raw_consistent(rtc_raw_t *out) {
    rtc_raw_t a, b;
    read_raw_once(&a);
    for (;;) {
        read_raw_once(&b);
        if (a.sec == b.sec && a.min == b.min && a.hr == b.hr &&
            a.day == b.day && a.mon == b.mon && a.yr == b.yr &&
            a.cent == b.cent && a.wd == b.wd) {
            *out = a;
            return;
        }
        a = b;
    }
}

/* ---------- Public API -------------------------------------------------- */
void rtc_init(void) {
    debug_step("rtc: probing CMOS Status B + century support");

    g_status_b = cmos_read(RTC_STATUS_B);
    g_use_bcd  = !(g_status_b & RTC_B_BINARY);    /* bit 2 SET = binary */
    g_use_12hr = !(g_status_b & RTC_B_24HR);      /* bit 1 SET = 24-hour */

    /* Probe the century byte.  On a fresh machine CMOS register 0x32 is
     * usually 0 when not implemented; a non-zero BCD value like 0x20
     * tells us the BIOS is maintaining it.  Anything sane (>= 19) is
     * good enough for us to trust. */
    uint8_t cent_raw = cmos_read(RTC_CENTURY);
    uint8_t cent_bin = g_use_bcd ? bcd_to_bin(cent_raw) : cent_raw;
    if (cent_bin >= 19 && cent_bin <= 99) {
        g_have_century  = true;
        g_fallback_year = (uint16_t)(cent_bin * 100);
    } else {
        g_have_century  = false;
        /* Pick a plausible default century so the year reads correctly
         * even on machines without RTC 0x32.  20XX is right for the
         * next 75 years.  __DATE__ would be more precise but is a
         * compile-time string, not a number - this is good enough. */
        g_fallback_year = 2000;
    }

    cmos_park();

    debug_printf("[rtc] status_b=0x%x  bcd=%d  12hr=%d  century_reg=%d (raw=0x%x)\n",
                 g_status_b, g_use_bcd ? 1 : 0, g_use_12hr ? 1 : 0,
                 g_have_century ? 1 : 0, cent_raw);

    /* Sanity-print one snapshot so a misconfigured CMOS shows up loud. */
    rtc_time_t t;
    rtc_now(&t);
    debug_printf("[rtc] now = %04u-%02u-%02u %02u:%02u:%02u (wd=%u)\n",
                 t.year, t.month, t.day,
                 t.hour, t.minute, t.second, t.weekday);
    debug_ok("rtc: CMOS wall clock available");
}

void rtc_now(rtc_time_t *out) {
    if (!out) return;

    rtc_raw_t r;
    read_raw_consistent(&r);
    cmos_park();

    /* Hours need special handling: in 12-hour mode the high bit of the
     * raw register encodes PM independently of the BCD conversion, so
     * we strip it BEFORE converting and re-apply afterwards. */
    bool pm = false;
    uint8_t hr_raw = r.hr;
    if (g_use_12hr) {
        pm = (hr_raw & RTC_HOUR_PM) != 0;
        hr_raw &= (uint8_t)~RTC_HOUR_PM;
    }

    out->second  = maybe_bcd_to_bin(r.sec);
    out->minute  = maybe_bcd_to_bin(r.min);
    out->hour    = maybe_bcd_to_bin(hr_raw);
    out->weekday = maybe_bcd_to_bin(r.wd);
    out->day     = maybe_bcd_to_bin(r.day);
    out->month   = maybe_bcd_to_bin(r.mon);

    /* Map 12-hour back to a 24-hour value:
     *   12 AM -> 00, 1..11 AM -> 1..11, 12 PM -> 12, 1..11 PM -> 13..23 */
    if (g_use_12hr) {
        if (out->hour == 12) out->hour = 0;
        if (pm)              out->hour = (uint8_t)(out->hour + 12);
    }

    uint16_t year2 = maybe_bcd_to_bin(r.yr);
    if (g_have_century) {
        uint16_t cent = maybe_bcd_to_bin(r.cent);
        out->year = (uint16_t)(cent * 100 + year2);
    } else {
        out->year = (uint16_t)(g_fallback_year + year2);
    }
}

/* ---------- Unix-like seconds counter ----------------------------------
 * Treats years as 365-day, with a +1 leap correction for every fourth
 * year since 1970.  Good to within a day for the timeframes a hobby
 * scheduler cares about and, more importantly, monotonic and
 * deterministic - which is all the tasktimer needs.                     */
static uint32_t days_in_month(uint16_t year, uint8_t m) {
    static const uint8_t mlen[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12) return 30;
    if (m == 2) {
        bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
        return leap ? 29u : 28u;
    }
    return mlen[m - 1];
}

uint32_t rtc_seconds(void) {
    rtc_time_t t;
    rtc_now(&t);

    if (t.year < 1970) return 0;

    uint32_t days = 0;
    for (uint16_t y = 1970; y < t.year; y++) {
        bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
        days += leap ? 366u : 365u;
    }
    for (uint8_t m = 1; m < t.month; m++) {
        days += days_in_month(t.year, m);
    }
    days += (uint32_t)(t.day - 1);

    return days * 86400u
         + (uint32_t)t.hour   * 3600u
         + (uint32_t)t.minute * 60u
         + (uint32_t)t.second;
}

/* ---------- Formatters -------------------------------------------------- */
int rtc_format_datetime(const rtc_time_t *t, char *out, size_t out_sz) {
    if (!t || !out || out_sz == 0) return 0;
    return ksnprintf(out, out_sz, "%04u-%02u-%02u %02u:%02u:%02u",
                     t->year, t->month, t->day,
                     t->hour, t->minute, t->second);
}

int rtc_format_time(const rtc_time_t *t, char *out, size_t out_sz) {
    if (!t || !out || out_sz == 0) return 0;
    return ksnprintf(out, out_sz, "%02u:%02u:%02u",
                     t->hour, t->minute, t->second);
}

int rtc_format_date(const rtc_time_t *t, char *out, size_t out_sz) {
    if (!t || !out || out_sz == 0) return 0;
    return ksnprintf(out, out_sz, "%04u-%02u-%02u",
                     t->year, t->month, t->day);
}
