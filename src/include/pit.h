/* ============================================================================
 * NexxoN OS - Programmable Interval Timer (8253/8254)
 * ----------------------------------------------------------------------------
 * Programs channel 0 to deliver a periodic IRQ0.  The default frequency is
 * 100 Hz so that 1 tick == 10 ms - small enough for the spinner animation
 * and big enough to keep ISR overhead low.
 * ============================================================================ */
#ifndef NEXXON_PIT_H
#define NEXXON_PIT_H

#include "types.h"

#define PIT_DEFAULT_HZ  100

void     pit_init  (uint32_t hz);
uint32_t pit_ticks (void);          /* monotonic ticks since boot      */
uint32_t pit_ms    (void);          /* milliseconds since boot         */
void     pit_sleep (uint32_t ms);   /* busy-wait until N ms have passed */
void     pit_reprogram(void);       /* re-assert ch0 rate after a BIOS call */
uint32_t pit_now_us(void);          /* sub-tick us clock; deltas only (wraps) */
void     pit_idle_hlt(void);        /* idle-flagged hlt for the CPU-usage gauge */
uint32_t pit_cpu_usage_pct(void);   /* real CPU% since last call (resets window) */

#endif /* NEXXON_PIT_H */
