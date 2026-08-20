/* ============================================================================
 * NexxoN OS - Preemptive Round-Robin Scheduler  (v1.0)
 * ----------------------------------------------------------------------------
 * Key design points
 * -----------------
 *   - Up to SCHED_MAX_TASKS (32) concurrent tasks.  Each gets its own 8 KiB
 *     kernel stack in BSS (g_task_stacks[]).
 *   - Task[0] is the "init" task: created by sched_init() to wrap the current
 *     kernel_main() execution.  Its kstack slot is reserved but its *actual*
 *     running stack is whatever stack kernel_main already uses (GDT g_kstack).
 *     The scheduler captures task[0]'s ESP on the first preemption just like
 *     any other task.
 *   - ctx_switch (boot/ctx_switch.asm) swaps the kernel stack pointer: saves
 *     EBP/EBX/ESI/EDI + ESP on the old task's stack, then loads the new
 *     task's saved ESP and restores those four registers.
 *   - Preemption fires via IRQ0 (PIT, 100 Hz) every SCHED_TICK_QUANTUM ticks.
 *     The PIC EOI for IRQ0 is sent INSIDE sched_tick, before ctx_switch, so
 *     that brand-new tasks (which start through task_trampoline bypassing
 *     irq_common_stub) do not starve the interrupt controller.
 *   - Cooperative yield (sched_yield) calls ctx_switch directly without
 *     touching the PIC.
 *   - task_trampoline issues STI explicitly because it never unwinds through
 *     irq_common_stub's "sti; iret".
 * ============================================================================ */
#include "sched.h"
#include "pit.h"
#include "pic.h"
#include "gdt.h"
#include "vmm.h"
#include "smp.h"
#include "string.h"
#include "debug.h"

/* Assembly context switch: boot/ctx_switch.asm (-m32) or
 * boot64/ctx_switch64.asm (-m64).  Saves/restores the callee-saved GP regs
 * (+ RFLAGS on 64-bit) and swaps the stack pointer.  uintptr_t args carry the
 * 32-bit ESP / 64-bit RSP transparently. */
extern void ctx_switch(uintptr_t *old_sp_ptr, uintptr_t new_sp);

/* ---- Static storage ------------------------------------------------------ */
static tcb_t   g_tasks[SCHED_MAX_TASKS];
static uint8_t g_task_stacks[SCHED_MAX_TASKS][SCHED_KSTACK_SIZE] ALIGNED(16);

static tcb_t  *g_current      = NULL;
static bool    g_sched_enabled = false;
static uint32_t g_tick_count  = 0;
static uint32_t g_next_pid    = 1;

static kspin_t g_sched_lock   = 0;

/* ---- Forward declarations ------------------------------------------------ */
static void __attribute__((noreturn)) task_trampoline(void);

/* ---- Internal helpers ---------------------------------------------------- */
static tcb_t *alloc_tcb(void) {
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_DEAD) return &g_tasks[i];
    }
    return NULL;
}

static tcb_t *pick_next(void) {
    if (!g_current) return NULL;
    int start = (int)(g_current - g_tasks);
    int n     = SCHED_MAX_TASKS;
    for (int i = 1; i <= n; i++) {
        int idx = (start + i) % n;
        tcb_t *t = &g_tasks[idx];

        /* Wake sleeping tasks whose deadline has passed. */
        if (t->state == TASK_SLEEPING && pit_ms() >= t->sleep_until_ms)
            t->state = TASK_READY;

        if (t->state == TASK_READY) return t;
    }
    /* No other ready task: check if current can keep running. */
    if (g_current->state == TASK_RUNNING) return g_current;
    return NULL;
}

/* ---- task_trampoline ----------------------------------------------------- */
/* Entry point for every new task.  ctx_switch arrives here via ret when a
 * brand-new task is first scheduled.  At this point interrupts are disabled
 * (IF=0 from the last irq_common_stub cli, carried through ctx_switch).
 * We issue sti, then call the task function.  sched_exit() cleans up. */
static void __attribute__((noreturn)) task_trampoline(void) {
    __asm__ volatile ("sti");           /* re-enable interrupts              */
    tcb_t *t = g_current;
    t->fn(t->arg);
    sched_exit();
    for (;;) __asm__ volatile ("hlt"); /* unreachable                        */
}

/* ---- Public API ---------------------------------------------------------- */

bool sched_init(void) {
    memset(g_tasks, 0, sizeof(g_tasks));
    kspin_init(&g_sched_lock);

    /* Task 0 = the current execution context (kernel_main).  We don't
     * allocate a new stack — the existing kernel stack is already in use.
     * kstack points to the reserved slot (used for TSS.ESP0 bookkeeping
     * when this task runs apps later via usermode.c). */
    g_tasks[0].state      = TASK_RUNNING;
    g_tasks[0].pid        = g_next_pid++;
    g_tasks[0].name       = "init";
    g_tasks[0].fn         = NULL;
    g_tasks[0].arg        = 0;
    g_tasks[0].pd         = vmm_kernel_pd();
    g_tasks[0].kstack     = g_task_stacks[0];
    g_tasks[0].kstack_size= SCHED_KSTACK_SIZE;
    g_tasks[0].esp        = 0;  /* set on first preemption by ctx_switch    */

    g_current      = &g_tasks[0];
    g_sched_enabled = true;
    g_tick_count   = 0;

    debug_printf("[sched] initialized: %d task slots, %u KiB stack each\n",
                 SCHED_MAX_TASKS, SCHED_KSTACK_SIZE / 1024);
    return true;
}

tcb_t *sched_spawn(const char *name, task_fn_t fn, uint32_t arg, vmm_pd_t *pd) {
    kspin_lock(&g_sched_lock);

    tcb_t *t = alloc_tcb();
    if (!t) {
        kspin_unlock(&g_sched_lock);
        debug_printf("[sched] sched_spawn: no free task slot\n");
        return NULL;
    }

    int idx = (int)(t - g_tasks);
    uint8_t *kstack = g_task_stacks[idx];

    t->state       = TASK_READY;
    t->pid         = g_next_pid++;
    t->name        = name ? name : "?";
    t->fn          = fn;
    t->arg         = arg;
    t->pd          = pd ? pd : vmm_kernel_pd();
    t->kstack      = kstack;
    t->kstack_size = SCHED_KSTACK_SIZE;
    t->exit_code   = 0;

    /* Set up the initial kernel stack so the first ctx_switch into this task
     * `ret`s straight into task_trampoline.  The primed slots must mirror the
     * pop order of the arch's ctx_switch. */
#if defined(__x86_64__)
    /* ctx_switch64 pops r15,r14,r13,r12,rbp,rbx, then popfq, then ret.
     * Layout (low -> high): [r15][r14][r13][r12][rbp][rbx][rflags][trampoline].
     * rflags = 0x202 (IF=1 + reserved bit 1) so the task starts interruptible. */
    uintptr_t *sp = (uintptr_t *)(kstack + SCHED_KSTACK_SIZE);
    *(--sp) = (uintptr_t)task_trampoline;  /* ret addr */
    *(--sp) = 0x202;                        /* rflags (IF=1) */
    *(--sp) = 0;  /* rbx */
    *(--sp) = 0;  /* rbp */
    *(--sp) = 0;  /* r12 */
    *(--sp) = 0;  /* r13 */
    *(--sp) = 0;  /* r14 */
    *(--sp) = 0;  /* r15 */
    t->esp = (uintptr_t)sp;
#else
    /* ctx_switch pops edi,esi,ebx,ebp, then ret.
     * Layout (low -> high): [edi][esi][ebx][ebp][task_trampoline]. */
    uint32_t *sp = (uint32_t *)(kstack + SCHED_KSTACK_SIZE);
    *(--sp) = (uint32_t)(uintptr_t)task_trampoline; /* ret addr */
    *(--sp) = 0;  /* ebp */
    *(--sp) = 0;  /* ebx */
    *(--sp) = 0;  /* esi */
    *(--sp) = 0;  /* edi */
    t->esp = (uint32_t)(uintptr_t)sp;
#endif

    kspin_unlock(&g_sched_lock);

    debug_printf("[sched] spawned '%s' pid=%u idx=%d esp=0x%x\n",
                 t->name, t->pid, idx, (uint32_t)t->esp);
    return t;
}

/* Called from pit_isr every tick.  Runs with IF=0 (inside irq_common_stub). */
void sched_tick(registers_t *r) {
    (void)r;
    if (!g_sched_enabled || !g_current) return;

    if (++g_tick_count < SCHED_TICK_QUANTUM) return;
    g_tick_count = 0;

    tcb_t *next = pick_next();
    if (!next || next == g_current) return;

    /* NOTE: the IRQ0 EOI is sent by pit_isr() *before* it calls us, so we
     * must NOT send it again here -- doing so would be a duplicate EOI for
     * the same timer tick.  pit_isr's up-front EOI already keeps IRQ0 live
     * for brand-new tasks that enter via task_trampoline and never unwind
     * through irq_handler. */

    tcb_t *old = g_current;
    if (old->state == TASK_RUNNING) old->state = TASK_READY;
    next->state = TASK_RUNNING;
    g_current   = next;

    /* Update TSS.ESP0 to the top of the next task's kernel stack so that
     * ring-3 → ring-0 transitions land on the right kernel stack. */
    gdt_set_kernel_stack((uintptr_t)(next->kstack + next->kstack_size));

    /* Switch page directory if tasks use different address spaces. */
    if (next->pd && next->pd != old->pd) {
        vmm_switch(next->pd);
    }

    ctx_switch(&old->esp, next->esp);
    /* When ctx_switch returns, we are 'old' again (resumed after suspension). */
}

void sched_yield(void) {
    if (!g_sched_enabled || !g_current) return;

    tcb_t *next = pick_next();
    if (!next || next == g_current) return;

    tcb_t *old = g_current;
    /* Only demote RUNNING→READY.  If the caller already set state to
     * SLEEPING or BLOCKED (e.g. sched_sleep), preserve that state. */
    if (old->state == TASK_RUNNING) old->state = TASK_READY;
    next->state = TASK_RUNNING;
    g_current   = next;

    gdt_set_kernel_stack((uintptr_t)(next->kstack + next->kstack_size));
    if (next->pd && next->pd != old->pd) vmm_switch(next->pd);

    ctx_switch(&old->esp, next->esp);
}

void sched_sleep(uint32_t ms) {
    if (!g_sched_enabled || !g_current) return;

    g_current->sleep_until_ms = pit_ms() + ms;
    g_current->state = TASK_SLEEPING;
    sched_yield();
}

void sched_exit(void) {
    if (!g_current) return;

    g_current->state = TASK_ZOMBIE;
    debug_printf("[sched] task '%s' pid=%u exited\n",
                 g_current->name, g_current->pid);

    /* Release page directory if task had its own (not the kernel PD). */
    if (g_current->pd && g_current->pd != vmm_kernel_pd()) {
        vmm_pd_t *dead_pd = g_current->pd;
        g_current->pd = vmm_kernel_pd();
        vmm_switch(vmm_kernel_pd());
        vmm_release(dead_pd);
    }

    /* We must schedule another task; yield handles that.  Since state is
     * ZOMBIE, pick_next() won't select us again. */
    sched_yield();

    /* If sched_yield returned (no other task ready), mark slot free. */
    g_current->state = TASK_DEAD;
    for (;;) __asm__ volatile ("hlt");
}

/* ---- Heartbeat task -------------------------------------------------------
 * Exported so kernel.c can spawn it without pulling in pit.h directly.
 * Prints a serial message every 2 seconds to confirm ctx_switch is alive. */
void heartbeat_task(uint32_t arg) {
    (void)arg;
    uint32_t n = 0;
    for (;;) {
        debug_printf("[sched] heartbeat %u — preemptive ctx_switch OK\n", n++);
        sched_sleep(2000);
    }
}

tcb_t *sched_current(void)   { return g_current; }
void sched_set_current_pd(vmm_pd_t *pd) {
    if (g_current) g_current->pd = pd ? pd : vmm_kernel_pd();
}
int    sched_task_count(void) {
    int n = 0;
    for (int i = 0; i < SCHED_MAX_TASKS; i++)
        if (g_tasks[i].state != TASK_DEAD) n++;
    return n;
}
bool sched_running(void) { return g_sched_enabled; }
