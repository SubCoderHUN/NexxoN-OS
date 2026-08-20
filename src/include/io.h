/* ============================================================================
 * NexxoN OS - x86 port I/O primitives
 * ----------------------------------------------------------------------------
 * Tiny inline wrappers around the IN / OUT instruction family.  Marked
 * always-inline so the compiler is forced to emit them straight into the
 * caller - this matters because some callers (PIC remap, ATA polling, ...)
 * must hit exact port-write timing.
 * ============================================================================ */
#ifndef NEXXON_IO_H
#define NEXXON_IO_H

#include "types.h"

INLINE void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

INLINE uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

INLINE void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

INLINE uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

INLINE void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

INLINE uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Issue a dummy IO read on 0x80 (unused diagnostic port).  Used between
 * back-to-back port writes that target old (PIC, PIT, ...) hardware which
 * needs a few hundred ns between accesses to settle. */
INLINE void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

INLINE void cli(void) { __asm__ volatile ("cli"); }
INLINE void sti(void) { __asm__ volatile ("sti"); }
INLINE void hlt(void) { __asm__ volatile ("hlt"); }

/* Read EFLAGS - handy for "are interrupts on?" checks. */
INLINE uint32_t read_eflags(void) {
    /* (R)FLAGS push/pop width must match the CPU mode: pushfq/popq in long
     * mode, pushfl/popl in 32-bit PM.  Only the low 32 bits matter (IF +
     * arithmetic flags), so the uint32_t return keeps callers unchanged. */
    uintptr_t flags;
#if defined(__x86_64__)
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
#else
    __asm__ volatile ("pushfl; popl %0" : "=r"(flags));
#endif
    return (uint32_t)flags;
}

#endif /* NEXXON_IO_H */
