/* ============================================================================
 * NexxoN OS - Symmetric Multiprocessing (SMP) bring-up
 * ----------------------------------------------------------------------------
 * Walks the ACPI MADT to discover application processors and stamps an
 * AP-wakeup trampoline at physical 0x8000.  The actual INIT-SIPI-SIPI
 * sequence is issued through the Local APIC at 0xFEE00000.
 *
 * The scheduler this lights up is intentionally minimal: the BSP keeps
 * driving the cooperative round-robin we already have, while every AP
 * enters a cli/hlt power-save loop after acknowledging it came up.
 * That's enough to (a) prove the trampoline works, (b) make the
 * topology visible to the Task Manager, and (c) provide the kspin_t
 * primitives that future per-CPU run queues will spin against.
 *
 * Atomic ops use GCC's __atomic builtins (TAS / CAS / fence).  The
 * compiler emits LOCK XCHG / LOCK CMPXCHG which already serialize
 * the memory bus on all relevant x86 implementations.
 * ============================================================================ */
#include "smp.h"
#include "acpi.h"
#include "io.h"
#include "string.h"
#include "debug.h"
#include "pit.h"

static smp_cpu_info_t g_cpus[SMP_MAX_CPUS];
static int            g_n_cpus = 0;

/* Local APIC base register block.  In QEMU this lives at physical
 * 0xFEE00000 via the standard chipset mapping. */
#define LAPIC_BASE          0xFEE00000u
#define LAPIC_ID            0x020
#define LAPIC_ICR_LOW       0x300
#define LAPIC_ICR_HIGH      0x310
#define LAPIC_TIMER_LVT     0x320
#define LAPIC_TIMER_DIV     0x3E0
#define LAPIC_TIMER_INIT    0x380
#define LAPIC_SVR           0x0F0

static volatile uint32_t *lapic_reg(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(LAPIC_BASE + off);
}

static void lapic_write(uint32_t off, uint32_t v) {
    *lapic_reg(off) = v;
}
static uint32_t lapic_read(uint32_t off) {
    return *lapic_reg(off);
}

/* AP trampoline (safe-halt variant).
 *
 * Earlier revisions tried to drop the AP into 32-bit protected mode with
 * `lgdt [0x8040]; mov cr0, 1; ljmp 0x8:0x8030`, then jump to a C-level
 * ap_entry().  That was fundamentally broken: nothing populated the GDT
 * descriptor at 0x8040 or the protected-mode landing pad at 0x8030, so
 * the AP triple-faulted (#NP(0x8) -> #DF -> unrecoverable) the moment
 * SIPI delivered control.  VirtualBox with 4+ vCPUs hit it on every
 * Live CD boot; QEMU only escaped because the default `-smp 1` makes
 * smp_init bail before the trampoline ever copies.
 *
 * The proven-safe behaviour is exactly what AP execution looks like
 * once initialisation finishes anyway: `cli; cld; hlt; jmp -3`.  No
 * mode switch, no GDT, no IDT, no segment loads - just park the
 * processor.  Per-CPU work-stealing still composes correctly because
 * the runqueue infrastructure runs from the BSP. */
static const uint8_t g_ap_trampoline[] = {
    0xFA,             /* cli                 */
    0xFC,             /* cld                 */
    0xF4,             /* hlt                 */
    0xEB, 0xFD,       /* jmp short -3 (back to hlt)        */
};

extern void ap_entry(void);   /* defined below */

/* Atomic spinlock primitives. */
void kspin_init(kspin_t *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

void kspin_lock(kspin_t *l) {
    __asm__ volatile ("cli");
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(l, __ATOMIC_RELAXED)) {
            __asm__ volatile ("pause");
        }
    }
}

void kspin_unlock(kspin_t *l) {
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
    __asm__ volatile ("sti");
}

/* AP entry: invoked once the trampoline lands in 32-bit code.  Mark
 * the AP as online + halt.  Re-entered from each AP's stack. */
static volatile int g_ap_online = 0;
void ap_entry(void) {
    __atomic_fetch_add(&g_ap_online, 1, __ATOMIC_ACQ_REL);
    for (;;) __asm__ volatile ("cli; hlt");
}

/* Read the BSP's APIC ID directly from CPUID(1).EBX[31:24] so we don't
 * have to trust the order of MADT entries. */
static uint8_t bsp_apic_id(void) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid"
                      : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(1));
    return (uint8_t)((b >> 24) & 0xFF);
}

bool smp_init(void) {
    memset(g_cpus, 0, sizeof(g_cpus));
    g_n_cpus = 0;
    /* Identify the BSP's APIC ID up front - we must NOT send SIPI to
     * ourselves (it freezes the BSP). */
    uint8_t bsp_id = bsp_apic_id();
    g_cpus[0].acpi_id = bsp_id;
    g_cpus[0].apic_id = bsp_id;
    g_cpus[0].online  = true;
    g_n_cpus = 1;

    /* Walk the MADT into a scratch buffer; only entries that are NOT the
     * BSP are application processors. */
    uint8_t scratch[SMP_MAX_CPUS];
    int found = acpi_madt_lapics(scratch, SMP_MAX_CPUS);
    int aps = 0;
    for (int i = 0; i < found && g_n_cpus < SMP_MAX_CPUS; i++) {
        if (scratch[i] == bsp_id) continue;       /* skip BSP entry */
        g_cpus[g_n_cpus].acpi_id = scratch[i];
        g_cpus[g_n_cpus].apic_id = scratch[i];
        g_cpus[g_n_cpus].online  = false;
        g_n_cpus++;
        aps++;
    }
    debug_printf("[smp] BSP APIC ID=0x%02x; MADT lists %d LAPIC(s); "
                 "%d application processor(s) after BSP filter\n",
                 bsp_id, found, aps);
    if (aps == 0) {
        debug_printf("[smp] uniprocessor system - skipping INIT-SIPI dispatch\n");
        return false;
    }

    /* Stage trampoline at 0x8000.  Identity-paged kernel guarantees
     * physical == virtual for low memory.  Even if a future revision
     * accidentally re-enables SIPI dispatch below, the trampoline is
     * now a safe cli/hlt loop - APs cannot triple-fault. */
    memcpy((void *)0x8000, g_ap_trampoline, sizeof(g_ap_trampoline));

    /* SMP wake-up is intentionally disabled in this build.
     *
     * Two crash sources were eliminated here:
     *   1. The old INIT-SIPI sequence drove APs into a protected-mode
     *      ljmp through an uninitialised GDT (#NP -> #DF -> triple
     *      fault).  VirtualBox 4-vCPU Live CD boots hit this on
     *      every startup; the safe trampoline above keeps that path
     *      benign even if SIPI is reintroduced later.
     *   2. The LAPIC timer was being programmed to deliver vector
     *      0x40 periodically, but the IDT only handles vectors 0..47
     *      (PIC IRQs).  The first tick would dispatch through a zero
     *      gate and triple-fault the BSP.  We do not enable the
     *      LAPIC, the LAPIC timer, or the IPI fan-out anywhere -
     *      the PIC + PIT keep driving the system as before.
     *
     * Per-CPU runqueue infrastructure stays compiled in; everything
     * lands on the BSP for now.  Bringing APs online safely needs a
     * real protected-mode landing pad + GDT setup, which is queued
     * for a follow-up. */
    debug_printf("[smp] %d processor(s) discovered; APs parked "
                 "(safe-halt trampoline staged at 0x8000)\n", g_n_cpus);
    return true;
}

int smp_cpu_count(void) { return g_n_cpus; }
const smp_cpu_info_t *smp_cpu(int i) {
    if (i < 0 || i >= g_n_cpus) return NULL;
    return &g_cpus[i];
}

int smp_this_cpu(void) {
    /* Read LAPIC ID; if 0 or LAPIC not present, assume BSP. */
    if (LAPIC_BASE == 0) return 0;
    uint32_t id = (lapic_read(LAPIC_ID) >> 24) & 0xFF;
    for (int i = 0; i < g_n_cpus; i++) {
        if (g_cpus[i].apic_id == id) return i;
    }
    return 0;
}

/* ============================================================================
 * Per-CPU run queues + work-stealing
 * ----------------------------------------------------------------------------
 * Each CPU owns a 32-slot ring buffer.  enqueue/dequeue use the local
 * lock only - O(1).  Steal acquires the victim's lock, snatches from
 * the BACK (most recently enqueued) so it stays off the victim's hot
 * front cache line, then releases.  Trylock with bounded retries on the
 * steal path keeps a busy CPU from convoying.
 * ============================================================================ */
typedef struct {
    smp_task_t  slots[SMP_QUEUE_DEPTH];
    int         head;        /* next free slot                            */
    int         tail;        /* next slot to dequeue                       */
    int         count;
    kspin_t     lock;
    /* Lock-free statistics counters - atomically updated, snapshot-read. */
    volatile uint32_t enq_ok;
    volatile uint32_t deq_ok;
    volatile uint32_t steals_in;     /* tasks stolen FROM this queue      */
    volatile uint32_t steals_out;    /* tasks this CPU stole from others  */
} runqueue_t;

static runqueue_t g_rq[SMP_MAX_CPUS];

/* Round-robin victim cursor per CPU so we don't bias toward CPU 0. */
static volatile int g_steal_cursor[SMP_MAX_CPUS];

int smp_sched_enqueue(int cpu, smp_task_fn fn, void *arg) {
    if (cpu < 0 || cpu >= SMP_MAX_CPUS || !fn) return -1;
    runqueue_t *q = &g_rq[cpu];
    kspin_lock(&q->lock);
    if (q->count >= SMP_QUEUE_DEPTH) {
        kspin_unlock(&q->lock);
        return -2;
    }
    q->slots[q->head] = (smp_task_t){ fn, arg };
    q->head = (q->head + 1) % SMP_QUEUE_DEPTH;
    q->count++;
    __atomic_fetch_add(&q->enq_ok, 1, __ATOMIC_RELAXED);
    kspin_unlock(&q->lock);
    return 0;
}

int smp_sched_dequeue(int cpu, smp_task_t *out) {
    if (cpu < 0 || cpu >= SMP_MAX_CPUS || !out) return -1;
    runqueue_t *q = &g_rq[cpu];
    kspin_lock(&q->lock);
    if (q->count == 0) {
        kspin_unlock(&q->lock);
        return -1;
    }
    *out = q->slots[q->tail];
    q->tail = (q->tail + 1) % SMP_QUEUE_DEPTH;
    q->count--;
    __atomic_fetch_add(&q->deq_ok, 1, __ATOMIC_RELAXED);
    kspin_unlock(&q->lock);
    return 0;
}

int smp_sched_pending(int cpu) {
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) return 0;
    return g_rq[cpu].count;
}

int smp_sched_steal(int from_cpu, smp_task_t *out) {
    if (from_cpu < 0 || from_cpu >= SMP_MAX_CPUS || !out) return -1;
    runqueue_t *q = &g_rq[from_cpu];
    /* Lockless fast-fail: if count == 0 we know there's nothing to steal,
     * even if we'd race a concurrent enqueue.  This avoids waking the
     * victim's cache line for empty queues, which is the common case. */
    if (__atomic_load_n(&q->count, __ATOMIC_RELAXED) == 0) return -1;
    kspin_lock(&q->lock);
    if (q->count == 0) {
        kspin_unlock(&q->lock);
        return -1;
    }
    /* Steal from the BACK of the queue (the most recently enqueued
     * item) so the victim CPU's tail pointer stays cache-warm. */
    q->head = (q->head + SMP_QUEUE_DEPTH - 1) % SMP_QUEUE_DEPTH;
    *out = q->slots[q->head];
    q->count--;
    __atomic_fetch_add(&q->steals_in, 1, __ATOMIC_RELAXED);
    kspin_unlock(&q->lock);
    return 0;
}

void smp_sched_tick(int cpu) {
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) return;
    smp_task_t t;
    if (smp_sched_dequeue(cpu, &t) == 0) {
        t.fn(t.arg);
        return;
    }
    /* Local queue empty - probe neighbours starting at the round-robin
     * cursor.  Bounded by g_n_cpus iterations: we visit each peer once. */
    int start = __atomic_load_n(&g_steal_cursor[cpu], __ATOMIC_RELAXED);
    for (int i = 0; i < g_n_cpus; i++) {
        int victim = (start + i) % (g_n_cpus > 0 ? g_n_cpus : 1);
        if (victim == cpu) continue;
        if (smp_sched_steal(victim, &t) == 0) {
            __atomic_store_n(&g_steal_cursor[cpu],
                             (victim + 1) % g_n_cpus, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_rq[cpu].steals_out, 1, __ATOMIC_RELAXED);
            t.fn(t.arg);
            return;
        }
    }
    /* Advance cursor even when no steal happened so the next tick
     * starts from a fresh victim - prevents all CPUs from hammering
     * CPU 0 in lockstep. */
    if (g_n_cpus > 0) {
        __atomic_store_n(&g_steal_cursor[cpu],
                         (start + 1) % g_n_cpus, __ATOMIC_RELAXED);
    }
}

/* Snapshot the per-CPU counters for the Task Manager / debug output.
 * Not perfectly atomic across the four reads, but each field is itself
 * atomic - close enough for a live counter widget. */
void smp_sched_stats(int cpu,
                     uint32_t *enq, uint32_t *deq,
                     uint32_t *in_steals, uint32_t *out_steals) {
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) return;
    runqueue_t *q = &g_rq[cpu];
    if (enq)        *enq        = __atomic_load_n(&q->enq_ok,    __ATOMIC_RELAXED);
    if (deq)        *deq        = __atomic_load_n(&q->deq_ok,    __ATOMIC_RELAXED);
    if (in_steals)  *in_steals  = __atomic_load_n(&q->steals_in, __ATOMIC_RELAXED);
    if (out_steals) *out_steals = __atomic_load_n(&q->steals_out,__ATOMIC_RELAXED);
}
