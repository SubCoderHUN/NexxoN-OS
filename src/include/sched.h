/* ============================================================================
 * NexxoN OS - Preemptive Scheduler  (v1.0)
 * ----------------------------------------------------------------------------
 * Round-robin task scheduler with kernel-thread preemption via IRQ0 (PIT).
 *
 * Each task owns a private kernel stack (SCHED_KSTACK_SIZE bytes in BSS).
 * Context switches use ctx_switch() (boot/ctx_switch.asm) which saves and
 * restores four callee-saved GP registers (EBP, EBX, ESI, EDI) plus swaps
 * ESP.  No EFLAGS save/restore in ctx_switch -- interrupts are managed by
 * the IRQ stub (cli at entry, sti;iret on exit) and by task_trampoline
 * (explicit sti before calling the task function).
 *
 * API for kernel code:
 *   sched_init()    - register init task and enable preemption
 *   sched_spawn()   - create a new task
 *   sched_yield()   - cooperatively give up the CPU
 *   sched_sleep(ms) - block for N milliseconds
 *   sched_exit()    - terminate current task and reschedule
 *   sched_current() - return the running TCB pointer
 *   sched_tick(r)   - called from PIT ISR every tick
 * ============================================================================ */
#ifndef NEXXON_SCHED_H
#define NEXXON_SCHED_H

#include "types.h"
#include "isr.h"
#include "vmm.h"

/* ---------- Tuning constants ----------------------------------------------- */
#define SCHED_MAX_TASKS     32
#define SCHED_KSTACK_SIZE   (8 * 1024)      /* 8 KiB kernel stack per task  */
#define SCHED_TICK_QUANTUM  4               /* ticks per time slice (40 ms)  */

/* ---------- Task states ---------------------------------------------------- */
typedef enum {
    TASK_DEAD    = 0,   /* slot unused                                       */
    TASK_READY   = 1,   /* runnable, waiting for CPU                         */
    TASK_RUNNING = 2,   /* currently executing                               */
    TASK_BLOCKED = 3,   /* waiting for an event (I/O, IPC, ...)             */
    TASK_SLEEPING= 4,   /* pit_ms() deadline in sleep_until_ms              */
    TASK_ZOMBIE  = 5,   /* exited, not yet reaped                           */
} task_state_t;

typedef void (*task_fn_t)(uint32_t arg);

/* ---------- Task Control Block --------------------------------------------- */
/* Fields at fixed offsets so ctx_switch.asm can reference them without
 * recompilation if layout ever changes -- but currently ctx_switch uses
 * only 'esp' (offset 0) via C pointer arithmetic. */
typedef struct tcb {
    /* offset  0 */ uintptr_t     esp;           /* saved kernel stack ptr (ESP/RSP) */
    /*           */ vmm_pd_t     *pd;            /* page directory (NULL=kernel) */
    /*           */ uint8_t      *kstack;        /* stack allocation base   */
    /*           */ uint32_t      kstack_size;   /* stack size in bytes     */
    /* offset 16 */ task_state_t  state;
    /* offset 20 */ uint32_t      pid;
    /* offset 24 */ uint32_t      sleep_until_ms;
    /* offset 28 */ const char   *name;
    /* offset 32 */ task_fn_t     fn;
    /* offset 36 */ uint32_t      arg;
    /* offset 40 */ int32_t       exit_code;
} tcb_t;

/* ---------- Scheduler API -------------------------------------------------- */
bool    sched_init      (void);
tcb_t  *sched_spawn     (const char *name, task_fn_t fn, uint32_t arg,
                         vmm_pd_t *pd);
void    sched_tick      (registers_t *r);   /* called from PIT ISR          */
void    sched_yield     (void);             /* cooperative CPU relinquish   */
void    sched_sleep     (uint32_t ms);      /* block for N milliseconds     */
void    sched_exit      (void);             /* terminate current task       */
tcb_t  *sched_current   (void);
/* Update the current task's CR3 identity when a synchronous userspace
 * launcher temporarily borrows the task (Linux compatibility runtime). */
void    sched_set_current_pd(vmm_pd_t *pd);
int     sched_task_count(void);
bool    sched_running   (void);

#endif /* NEXXON_SCHED_H */
