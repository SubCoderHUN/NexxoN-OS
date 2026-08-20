/* ============================================================================
 * NexxoN OS - C-side ISR dispatcher
 * ----------------------------------------------------------------------------
 * The single isr_handler() function is called by *every* CPU-exception stub
 * (isr0 ... isr31) defined in boot/idt_flush.asm.  It looks up a per-vector
 * handler table; if no handler is installed for this vector, control falls
 * through to the kernel panic screen so the user always sees a meaningful
 * crash report instead of a triple-fault reboot.
 * ============================================================================ */
#include "isr.h"
#include "panic.h"
#include "debug.h"
#include "string.h"
#include "linuxsys.h"

#define ISR_TABLE_SZ 32
static isr_func_t g_isr_handlers[ISR_TABLE_SZ];

static const char *g_exception_names[ISR_TABLE_SZ] = {
    "Divide-by-Zero (#DE)",
    "Debug (#DB)",
    "Non-Maskable Interrupt",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "Bound Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",
    "Device Not Available (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun (legacy)",
    "Invalid TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Reserved (15)",
    "x87 Floating-Point (#MF)",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD Floating-Point (#XF)",
    "Virtualization Exception (#VE)",
    "Reserved (21)", "Reserved (22)", "Reserved (23)",
    "Reserved (24)", "Reserved (25)", "Reserved (26)",
    "Reserved (27)", "Reserved (28)", "Reserved (29)",
    "Reserved (30)", "Reserved (31)",
};

const char *exception_name(uint8_t v) {
    return (v < ISR_TABLE_SZ) ? g_exception_names[v] : "(unknown)";
}

void isr_install_handler(uint8_t vec, isr_func_t fn) {
    if (vec < ISR_TABLE_SZ) g_isr_handlers[vec] = fn;
}

/* Called from boot/idt_flush.asm.  Symbol is global so the linker resolves
 * the `call isr_handler` instruction in the common stub. */
void isr_handler(registers_t *r) {
#if defined(__x86_64__)
    debug_printf("[isr] vec=%u  err=0x%lx  rip=0x%lx\n",
                 (unsigned)r->int_no, (unsigned long)r->err_code,
                 (unsigned long)r->rip);
#else
    debug_printf("[isr] vec=%u  err=0x%x  eip=0x%08x\n",
                 r->int_no, r->err_code, r->eip);
#endif

    /* A bad userspace ELF is an ordinary process failure, not a kernel
     * failure.  Recover it before the generic panic path so malformed or
     * incompatible Linux programs cannot take down the bare-metal desktop. */
#if defined(__x86_64__)
    if ((r->cs & 3u) == 3u &&
        linux_handle_user_exception_frame(r, r->int_no, r->err_code, r->rip))
        return;
#endif

    if (r->int_no < ISR_TABLE_SZ && g_isr_handlers[r->int_no]) {
        g_isr_handlers[r->int_no](r);
        return;
    }
    /* No specific handler - escalate to the panic / debugger screen. */
    panic_from_exception(r);
}
