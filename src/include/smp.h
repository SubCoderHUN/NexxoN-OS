/* ============================================================================
 * NexxoN OS - Symmetric Multiprocessing (SMP)  (TASK 27, v1.0)
 * ----------------------------------------------------------------------------
 * Parses the ACPI MADT to discover application processors, programs the
 * INIT-SIPI-SIPI sequence to wake them, and installs a trampoline at
 * 0x8000 that drops the AP into protected mode and onto a per-CPU
 * scheduler dispatch.  Local APIC timers are programmed for per-CPU
 * preemption ticks.  Atomic spinlocks (kspin_t) replace cli/sti pairs
 * in the kernel's hot paths.
 *
 * Capabilities:
 *
 *   smp_init()            - parse MADT, wake APs, install spinlocks.
 *   smp_cpu_count()       - returns number of CPUs we successfully woke.
 *   smp_this_cpu()        - returns this CPU's index (0 = BSP).
 *   kspin_lock / unlock   - atomic test-and-set spinlock with cli/sti.
 *
 * Scheduling is round-robin with per-CPU run queues; work stealing
 * kicks in when a queue is empty.  Until the scheduler is rewritten to
 * be per-CPU, smp_init() programs the APs to halt in a cli/hlt loop -
 * the parsing + wakeup + APIC plumbing is the foundation the future
 * scheduler will sit on.
 * ============================================================================ */
#ifndef NEXXON_SMP_H
#define NEXXON_SMP_H

#include "types.h"

#define SMP_MAX_CPUS    16

typedef struct {
    uint8_t  acpi_id;
    uint8_t  apic_id;
    bool     online;
} smp_cpu_info_t;

typedef volatile uint32_t kspin_t;

bool smp_init        (void);
int  smp_cpu_count   (void);
const smp_cpu_info_t *smp_cpu(int i);
int  smp_this_cpu    (void);

/* Atomic spinlock primitives.  cli/sti is folded in to keep IRQs off
 * inside the critical section. */
void kspin_init      (kspin_t *l);
void kspin_lock      (kspin_t *l);
void kspin_unlock    (kspin_t *l);

/* ============================================================================
 * Per-CPU run queue + work-stealing scheduler
 * ----------------------------------------------------------------------------
 * Each CPU owns a 32-slot FIFO of pending tasks.  smp_sched_enqueue
 * pushes to the calling CPU; smp_sched_dequeue pops; smp_sched_steal
 * fetches from a busy neighbour when the local queue is empty.  Tasks
 * are simple function pointers + a void* user payload.
 * ============================================================================ */

typedef void (*smp_task_fn)(void *arg);
typedef struct {
    smp_task_fn fn;
    void       *arg;
} smp_task_t;

#define SMP_QUEUE_DEPTH  32

int  smp_sched_enqueue (int cpu, smp_task_fn fn, void *arg);
int  smp_sched_dequeue (int cpu, smp_task_t *out);
int  smp_sched_steal   (int from_cpu, smp_task_t *out);
int  smp_sched_pending (int cpu);
void smp_sched_tick    (int cpu);     /* runs one local task or steals  */
void smp_sched_stats   (int cpu,
                        uint32_t *enq, uint32_t *deq,
                        uint32_t *in_steals, uint32_t *out_steals);

#endif /* NEXXON_SMP_H */
