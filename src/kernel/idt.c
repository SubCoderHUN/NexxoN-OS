/* ============================================================================
 * NexxoN OS - IDT setup
 * ----------------------------------------------------------------------------
 * Installs a 256-entry IDT.  Vectors 0..31 point at the ISR stubs that the
 * boot assembly generated; vectors 32..47 point at the IRQ stubs that the
 * PIC delivers hardware lines through.  All remaining slots stay zero -
 * touching one will trigger a #GP via the absent-descriptor path, which
 * funnels into the kernel panic screen.
 * ============================================================================ */
#include "idt.h"
#include "string.h"
#include "debug.h"

#if defined(__x86_64__)
/* x86_64 16-byte interrupt gate (IA-32e). */
typedef struct PACKED {
    uint16_t offset_low;
    uint16_t selector;          /* 64-bit kernel code segment, 0x08          */
    uint8_t  ist;               /* interrupt-stack-table index (0 = none)    */
    uint8_t  flags;             /* 0x8E = present, ring 0, 64-bit int gate    */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} idt_entry_t;

typedef struct PACKED {
    uint16_t limit;
    uint64_t base;
} idt_ptr_t;
#else
/* i386 8-byte interrupt gate. */
typedef struct PACKED {
    uint16_t offset_low;
    uint16_t selector;          /* code segment selector, 0x08 for ring 0    */
    uint8_t  zero;
    uint8_t  flags;             /* 0x8E = present, ring 0, 32-bit int gate   */
    uint16_t offset_high;
} idt_entry_t;

typedef struct PACKED {
    uint16_t limit;
    uint32_t base;
} idt_ptr_t;
#endif

#define IDT_ENTRIES 256
static idt_entry_t g_idt[IDT_ENTRIES];
static idt_ptr_t   g_idt_ptr;

extern void idt_flush(uintptr_t idt_ptr_addr);  /* boot[64]/idt_flush[64].asm */

/* Declarations for every stub generated in boot/idt_flush.asm */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

void idt_set_gate(uint8_t vec, uintptr_t handler, uint16_t selector, uint8_t flags) {
    g_idt[vec].offset_low  = (uint16_t)(handler & 0xFFFF);
    g_idt[vec].selector    = selector;
    g_idt[vec].flags       = flags;
#if defined(__x86_64__)
    g_idt[vec].ist         = 0;
    g_idt[vec].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    g_idt[vec].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    g_idt[vec].zero        = 0;
#else
    g_idt[vec].offset_high = (uint16_t)((handler >> 16) & 0xFFFF);
    g_idt[vec].zero        = 0;
#endif
}

void idt_init(void) {
    debug_step("idt: building 256-entry table");

    memset(g_idt, 0, sizeof(g_idt));
    g_idt_ptr.limit = sizeof(g_idt) - 1;
    g_idt_ptr.base  = (uintptr_t)&g_idt;

    /* CPU exception vectors 0..31, kernel CS=0x08, int gate type 0xE,
     * present (bit 7), DPL=0 (bits 5..6 zero) -> flags = 0x8E. */
    #define G(v) idt_set_gate((v), (uintptr_t)isr##v, 0x08, 0x8E)
    G(0);  G(1);  G(2);  G(3);  G(4);  G(5);  G(6);  G(7);
    G(8);  G(9);  G(10); G(11); G(12); G(13); G(14); G(15);
    G(16); G(17); G(18); G(19); G(20); G(21); G(22); G(23);
    G(24); G(25); G(26); G(27); G(28); G(29); G(30); G(31);
    #undef G

    /* Hardware IRQ vectors 32..47.  Same gate type. */
    #define H(v, n) idt_set_gate((v), (uintptr_t)irq##n, 0x08, 0x8E)
    H(32, 0);  H(33, 1);  H(34, 2);  H(35, 3);
    H(36, 4);  H(37, 5);  H(38, 6);  H(39, 7);
    H(40, 8);  H(41, 9);  H(42, 10); H(43, 11);
    H(44, 12); H(45, 13); H(46, 14); H(47, 15);
    #undef H

    idt_flush((uintptr_t)&g_idt_ptr);
    debug_ok("idt: ISRs 0..31 + IRQs 32..47 installed");
}
