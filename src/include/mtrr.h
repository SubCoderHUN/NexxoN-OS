/* ============================================================================
 * NexxoN OS - MTRR (Memory Type Range Register) helper  (v1.0)
 * ----------------------------------------------------------------------------
 * Configures the LFB physical address range as Write-Combining (WC) so the
 * CPU coalesces our compositor's 32-bit framebuffer writes into burst
 * transactions on the PCIe / AGP bus instead of issuing them as individual
 * uncached posted writes.
 *
 * Write-combining is REQUIRED for performant blitting on bare metal:
 * without it, a single 1024x768x4 frame copy spends ~250 ms on real
 * hardware vs. ~3 ms in QEMU.  The WM compositor (see TASK 14) does all
 * its work in a SYSTEM RAM back buffer (cached WB) and then commits a
 * single linear memcpy() to the LFB - that final memcpy is the path WC
 * accelerates.
 *
 * On CPUs that don't expose MTRRs (very old VMs, ancient hardware) this
 * helper is a no-op and the compositor still works; we just don't get
 * the WC speedup.  Caller checks return value to surface in boot log.
 * ============================================================================ */
#ifndef NEXXON_MTRR_H
#define NEXXON_MTRR_H

#include "types.h"

#define MTRR_MEMTYPE_UC     0x00     /* uncacheable                       */
#define MTRR_MEMTYPE_WC     0x01     /* write-combining                   */
#define MTRR_MEMTYPE_WT     0x04     /* write-through                     */
#define MTRR_MEMTYPE_WB     0x06     /* write-back (default for RAM)      */

/* Try to enable write-combining for [base, base+size).  base + size must
 * fit in 32 bits on i386.  Returns true on success, false if MTRRs are
 * unsupported or no free variable register exists.  Logs to COM1. */
bool mtrr_set_write_combining(uint32_t base, uint32_t size);

/* Returns true when CPUID + MSR_IA32_MTRRCAP indicate the CPU supports
 * variable-range MTRRs.  Used by the boot path to decide whether to even
 * attempt the WC programming. */
bool mtrr_supported(void);

/* Effective MTRR memory type the CPU resolves for a physical address (one of
 * MTRR_MEMTYPE_*), or -1 if MTRRs are unsupported/disabled.  Used by sysrep to
 * tell whether the framebuffer is really WC or was clobbered to UC by a BIOS
 * range (the classic "fast in QEMU, crawls on bare metal" cause). */
int mtrr_effective_type(uint32_t addr);

/* Multi-line MTRR state dump for the `sysrep` command.  Returns bytes written. */
int mtrr_report(char *out, size_t cap);

#endif /* NEXXON_MTRR_H */
