/* ============================================================================
 * NexxoN OS - 8259A Programmable Interrupt Controller
 * ----------------------------------------------------------------------------
 * The legacy PC has two cascaded 8259As: master at 0x20/0x21, slave at
 * 0xA0/0xA1.  After power-up they deliver IRQs 0..7 to interrupt vectors
 * 0x08..0x0F, which collide with Intel-reserved CPU exception vectors.
 * pic_remap() relocates them to 0x20..0x2F so we get a clean separation
 * between CPU exceptions (0..31) and hardware IRQs (32..47).
 * ============================================================================ */
#ifndef NEXXON_PIC_H
#define NEXXON_PIC_H

#include "types.h"

#define PIC_MASTER_CMD  0x20
#define PIC_MASTER_DATA 0x21
#define PIC_SLAVE_CMD   0xA0
#define PIC_SLAVE_DATA  0xA1

#define PIC_EOI         0x20

void pic_remap     (void);
void pic_send_eoi  (uint8_t irq);
void pic_mask      (uint8_t irq);
void pic_unmask    (uint8_t irq);
void pic_mask_all  (void);

#endif /* NEXXON_PIC_H */
