/* ============================================================================
 * NexxoN OS - CMOS Real-Time Clock driver
 * ----------------------------------------------------------------------------
 * Reads wall-clock time from the MC146818-compatible RTC behind the standard
 * x86 CMOS index/data ports (0x70 / 0x71).  Handles:
 *
 *   - BCD <-> binary conversion (Status Register B bit 2)
 *   - 12-hour vs 24-hour mode (Status Register B bit 1, plus the PM flag
 *     in the high bit of the hours byte for 12-hour mode)
 *   - Update-In-Progress synchronisation (Status Register A bit 7) so we
 *     don't catch the RTC mid-rollover at the second/minute boundary
 *   - NMI bit (high bit of port 0x70) is held SET during every CMOS access
 *     to keep an NMI from arriving with a half-set index and leaving the
 *     RTC in an undefined state.  Standard OSDev practice.
 *   - Best-effort century byte (FADT-advertised offset on modern machines,
 *     CMOS 0x32 on legacy, hard-coded "20" fallback otherwise).
 *
 * The driver exposes a struct snapshot and a Unix-style monotonic seconds
 * counter so the task scheduler (see tasktimer.h) can express "fire at
 * 14:30:00" as plain integer comparisons.
 * ============================================================================ */
#ifndef NEXXON_RTC_H
#define NEXXON_RTC_H

#include "types.h"

typedef struct {
    uint16_t year;          /* full 4-digit year, e.g. 2025                 */
    uint8_t  month;         /* 1..12                                        */
    uint8_t  day;           /* 1..31                                        */
    uint8_t  hour;          /* 0..23                                        */
    uint8_t  minute;        /* 0..59                                        */
    uint8_t  second;        /* 0..59                                        */
    uint8_t  weekday;       /* 1..7 (Sunday=1)  - often unreliable on VMs   */
} rtc_time_t;

void rtc_init(void);

/* Snapshot the current wall-clock time into *out.  Thread/IRQ safe: spins
 * around UIP and re-reads until two consecutive samples agree, so callers
 * never see a torn read at a minute/second rollover. */
void rtc_now(rtc_time_t *out);

/* "Unix-style" seconds counter computed from rtc_now().  Not a strict
 * POSIX epoch (we don't compensate for leap seconds and we treat every
 * year as having 365.25 days), but monotonic, real-world-aligned, and
 * good enough for the tasktimer's "fire when seconds >= target_seconds"
 * comparison. */
uint32_t rtc_seconds(void);

/* Format the time as "YYYY-MM-DD HH:MM:SS" into out (needs >=20 bytes).
 * Returns the number of bytes written, not counting the trailing NUL. */
int rtc_format_datetime(const rtc_time_t *t, char *out, size_t out_sz);

/* Format JUST the time portion "HH:MM:SS" - useful for the System Monitor
 * window where we don't want the full date eating screen space. */
int rtc_format_time(const rtc_time_t *t, char *out, size_t out_sz);

/* Format JUST the date portion "YYYY-MM-DD". */
int rtc_format_date(const rtc_time_t *t, char *out, size_t out_sz);

#endif /* NEXXON_RTC_H */
