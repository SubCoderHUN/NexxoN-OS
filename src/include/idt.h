/* ============================================================================
 * NexxoN OS - Interrupt Descriptor Table
 * ============================================================================ */
#ifndef NEXXON_IDT_H
#define NEXXON_IDT_H

#include "types.h"

void idt_init(void);
/* handler is uintptr_t: 32-bit under -m32 (identical ABI to the old uint32_t),
 * 64-bit under -m64 so the gate's full offset fits across off_low/mid/high. */
void idt_set_gate(uint8_t vector, uintptr_t handler, uint16_t selector, uint8_t flags);

#endif /* NEXXON_IDT_H */
