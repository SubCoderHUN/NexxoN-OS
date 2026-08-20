/* ============================================================================
 * NexxoN OS - ISR / IRQ shared register frame + dispatch tables
 * ----------------------------------------------------------------------------
 * The layout of this struct MUST match the order in which registers are
 * pushed/popped by boot/idt_flush.asm.  Adding fields above `ds` requires
 * an equivalent change in the assembly.
 * ============================================================================ */
#ifndef NEXXON_ISR_H
#define NEXXON_ISR_H

#include "types.h"

#if defined(__x86_64__)
/* x86_64 interrupt frame.  Field ORDER must match the push sequence in
 * src/boot64/idt_flush64.asm's common stub (lowest address = pushed last):
 * the stub pushes rax..r15 on top of the stub-synthesised [int_no][err_code]
 * and the CPU's iretq frame.  No segment selectors are saved - long mode runs
 * a single flat code/data segment. */
typedef struct PACKED {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;     /* GP regs (stub push order) */
    uint64_t int_no, err_code;                      /* synthesised by the stub   */
    uint64_t rip, cs, rflags, rsp, ss;              /* CPU-saved iretq frame     */
} registers_t;
#else
typedef struct PACKED {
    uint32_t ds;                                /* data segment when fault hit  */
    uint32_t edi, esi, ebp, esp_dummy,
             ebx, edx, ecx, eax;                /* pusha frame                   */
    uint32_t int_no, err_code;                  /* synthesised by the stub       */
    uint32_t eip, cs, eflags;                   /* CPU-saved IRET frame          */
    uint32_t useresp, ss;                       /* present only on CPL change    */
} registers_t;
#endif

typedef void (*isr_func_t)(registers_t *);

void isr_install_handler(uint8_t vec, isr_func_t fn);
void irq_install_handler(uint8_t irq, isr_func_t fn);

const char *exception_name(uint8_t vec);

#endif /* NEXXON_ISR_H */
