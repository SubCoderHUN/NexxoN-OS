/* ============================================================================
 * NexxoN OS - Hardware IRQ C-level dispatcher
 * ----------------------------------------------------------------------------
 * Vectors 32..47 land here.  We dispatch to whichever C handler the driver
 * subsystem registered, then *always* send EOI to the PIC even if no handler
 * was installed.  Forgetting EOI is a classic stuck-interrupt bug, so we
 * centralise it.
 * ============================================================================ */
#include "irq.h"
#include "pic.h"
#include "debug.h"

#define IRQ_COUNT 16
static isr_func_t g_irq_handlers[IRQ_COUNT];

void irq_install_handler(uint8_t irq, isr_func_t fn) {
    if (irq < IRQ_COUNT) g_irq_handlers[irq] = fn;
}

void irq_remove_handler(uint8_t irq) {
    if (irq < IRQ_COUNT) g_irq_handlers[irq] = NULL;
}

/* Called from boot/idt_flush.asm (irq_common_stub -> call irq_handler). */
void irq_handler(registers_t *r) {
    uint8_t irq = (uint8_t)(r->int_no - 32);

    if (irq < IRQ_COUNT && g_irq_handlers[irq]) {
        g_irq_handlers[irq](r);
    }

    /* EOI must be sent so that the PIC re-enables the line.  We do this
     * *after* the handler so the handler can read the data port (e.g.
     * keyboard) before another IRQ pre-empts it.
     *
     * IRQ0 (PIT) is the ONE exception: pit_isr() sends its own EOI up
     * front because the preemptive scheduler may ctx_switch away inside
     * sched_tick() and not unwind back through this function until much
     * later (after the preempted task is rescheduled).  If we also sent
     * an EOI here we would issue a *second*, spurious EOI for the same
     * IRQ0 assertion, which silently corrupts the 8259 in-service /
     * priority state on real hardware (and stricter emulators) -- the
     * classic failure mode being keyboard/mouse IRQs that mysteriously
     * stop being delivered once timer + PS/2 traffic overlap. */
    if (irq != 0) pic_send_eoi(irq);
}
