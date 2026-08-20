/* ============================================================================
 * NexxoN OS - Hardware IRQ shared dispatcher
 * ============================================================================ */
#ifndef NEXXON_IRQ_H
#define NEXXON_IRQ_H

#include "types.h"
#include "isr.h"

void irq_install_handler(uint8_t irq, isr_func_t fn);
void irq_remove_handler (uint8_t irq);

#endif /* NEXXON_IRQ_H */
