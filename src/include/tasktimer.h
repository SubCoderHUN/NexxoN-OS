/* ============================================================================
 * NexxoN OS - RTC-driven background task scheduler
 * ----------------------------------------------------------------------------
 * Schedules a command line for execution at a real-world wall-clock instant.
 * Triggers are compared against rtc_seconds(), not PIT ticks, so a human-
 * readable spec like "14:30:00 today" or "in 5 minutes" actually fires at
 * the correct wall time even if the PIT drifts or the system has been
 * suspended/resumed.
 *
 * Tasks are stored in a fixed-size table; no malloc anywhere.  The shell
 * is responsible for polling tasktimer_tick() from its idle loop - the
 * scheduler does NOT install its own IRQ handler.
 * ============================================================================ */
#ifndef NEXXON_TASKTIMER_H
#define NEXXON_TASKTIMER_H

#include "types.h"

#define TASKTIMER_MAX_TASKS  16
#define TASKTIMER_PAYLOAD_MAX 192

typedef enum {
    TT_OK             = 0,
    TT_ERR_FULL       = -1,    /* no slot available                    */
    TT_ERR_PAYLOAD    = -2,    /* payload too long / empty             */
    TT_ERR_TIME       = -3,    /* target time in the past, or malformed*/
    TT_ERR_NOTFOUND   = -4,    /* unknown task id                      */
} tt_status_t;

void        tasktimer_init(void);

/* Schedule for an absolute wall-clock instant today.
 *   hour    : 0..23
 *   minute  : 0..59
 *   second  : 0..59
 *   payload : an NXScript / shell line, copied internally (need not live)
 *
 * If the requested time has already passed today, the task is implicitly
 * rolled over to the same time tomorrow.  Returns the task id (>= 1) on
 * success, a negative tt_status_t on failure.                        */
int tasktimer_schedule_at(uint8_t hour, uint8_t minute, uint8_t second,
                          const char *payload);

/* Schedule for "now + offset_seconds" seconds.  offset_seconds must be > 0. */
int tasktimer_schedule_in(uint32_t offset_seconds, const char *payload);

/* Cancel by id.  Returns TT_OK or TT_ERR_NOTFOUND. */
int tasktimer_cancel(int id);

/* Cancel every armed task.  Returns the number of slots actually cleared. */
int tasktimer_clear(void);

/* Walk every armed task in arbitrary order.  Callback parameters mirror
 * the public state needed for `tasktimer list`.  Stops early if the cb
 * returns false. */
typedef bool (*tasktimer_iter_cb_t)(int id,
                                    uint32_t target_seconds,
                                    const char *payload,
                                    void *user);
void tasktimer_iterate(tasktimer_iter_cb_t cb, void *user);

/* Returns how many slots are currently armed.  Useful for the System
 * Monitor and the `tasktimer list` empty-table message. */
int tasktimer_count(void);

/* Drive the scheduler.  Call from the shell idle loop.  Reads the RTC at
 * most once per wall-clock second; any task whose target is reached gets
 * EXECUTED IMMEDIATELY from the caller's stack frame (so a long-running
 * NXScript will block the shell until it returns - acceptable for a
 * single-threaded kernel; future versions can move execution to a job
 * queue).                                                              */
void tasktimer_tick(void);

#endif /* NEXXON_TASKTIMER_H */
