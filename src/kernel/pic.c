/* ============================================================================
 * NexxoN OS - 8259A PIC remap + masking  (v2.1)
 * ----------------------------------------------------------------------------
 * Bare-metal IRQ-routing rewrite:
 *
 *   * pic_remap() preserves the BIOS-saved IMRs (which on QEMU and most
 *     BIOSes already have the legacy lines unmasked correctly) but
 *     ALWAYS opens the master IR2 cascade so slave IRQs reach the CPU.
 *     This is backward-compatible with subsystems that don't yet call
 *     pic_unmask explicitly, while the new driver code base layers
 *     explicit unmasks on top - belt and braces.
 *
 *   * pic_unmask() now reads the line back to verify it cleared,
 *     re-writes once if the controller dropped the first write, and
 *     auto-opens the master cascade when a slave IRQ is unmasked.
 *
 *   * pic_send_eoi() unchanged - slave EOI for IRQ >= 8, master EOI
 *     always.
 * ============================================================================ */
#include "pic.h"
#include "io.h"
#include "debug.h"

/* ICW (Initialisation Command Word) bits used during the four-step setup. */
#define ICW1_ICW4   0x01
#define ICW1_INIT   0x10
#define ICW4_8086   0x01

void pic_remap(void) {
    debug_step("pic: remapping legacy IRQs to vectors 32..47");

    /* Snapshot the current mask so the post-remap state matches what
     * the BIOS handed us PLUS the cascade open.  Reseting to "all
     * masked" used to deliver a fully dead IRQ line set on hardware
     * where the per-driver init flow hadn't yet been audited to call
     * pic_unmask. */
    uint8_t mask_m = inb(PIC_MASTER_DATA);
    uint8_t mask_s = inb(PIC_SLAVE_DATA);

    /* ICW1: start init, expect ICW4 */
    outb(PIC_MASTER_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC_SLAVE_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();

    /* ICW2: vector offsets */
    outb(PIC_MASTER_DATA, 0x20); io_wait();      /* master -> 32..39 */
    outb(PIC_SLAVE_DATA,  0x28); io_wait();      /* slave  -> 40..47 */

    /* ICW3: cascade wiring (slave on IR2 of master) */
    outb(PIC_MASTER_DATA, 0x04); io_wait();      /* tell master  */
    outb(PIC_SLAVE_DATA,  0x02); io_wait();      /* tell slave   */

    /* ICW4: 8086 mode (vs. legacy 8080) */
    outb(PIC_MASTER_DATA, ICW4_8086); io_wait();
    outb(PIC_SLAVE_DATA,  ICW4_8086); io_wait();

    /* Restore BIOS masks but ALWAYS open the master IR2 cascade so
     * slave-side IRQs (12 = mouse, 14/15 = ATA, 11 = e1000) can reach
     * the CPU regardless of what the BIOS left behind.  Subsystems
     * still call pic_unmask() in their init for the belt-and-braces
     * fix on PCH BIOSes that leave specific lines masked - we just no
     * longer wedge the bus by force-masking everything at remap. */
    outb(PIC_MASTER_DATA, mask_m & (uint8_t)~(1u << 2));
    outb(PIC_SLAVE_DATA,  mask_s);

    debug_printf("[pic] BIOS handover masks: M=0x%02x S=0x%02x, cascade opened\n",
                 mask_m, mask_s);
    debug_ok("pic: master=0x20..0x27  slave=0x28..0x2F  cascade open");
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC_SLAVE_CMD, PIC_EOI);
    outb(PIC_MASTER_CMD, PIC_EOI);
}

void pic_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC_MASTER_DATA : PIC_SLAVE_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) | (uint8_t)(1 << irq));
}

void pic_unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC_MASTER_DATA : PIC_SLAVE_DATA;
    uint8_t  bit  = (irq >= 8) ? (uint8_t)(1 << (irq - 8)) : (uint8_t)(1 << irq);
    uint8_t  cur  = inb(port);
    uint8_t  new_ = cur & (uint8_t)~bit;
    outb(port, new_);
    /* Verify the write actually stuck.  On some bare-metal PCHs an
     * outstanding EOI in the middle of a port-write hands us back the
     * old mask; one retry clears it. */
    if ((inb(port) & bit) != 0) {
        io_wait();
        outb(port, new_);
    }
    /* If we unmasked anything on the slave, ensure the master cascade
     * (IR2) is open - a slave IRQ never reaches the CPU otherwise. */
    if (irq >= 8) {
        uint8_t m = inb(PIC_MASTER_DATA);
        if (m & (1u << 2)) {
            outb(PIC_MASTER_DATA, m & (uint8_t)~(1u << 2));
        }
    }
}

void pic_mask_all(void) {
    outb(PIC_MASTER_DATA, 0xFF);
    outb(PIC_SLAVE_DATA,  0xFF);
}
