#ifndef NEXXON_ACPI_H
#define NEXXON_ACPI_H
#include "types.h"

/* Scan for RSDP, walk RSDT to FADT, extract PM1a_CNT_BLK + SLP_TYPa/S3.
 * Safe to call with interrupts on; reads only memory-mapped firmware tables. */
void acpi_init    (void);

/* Power off.  Tries ACPI first, then QEMU/Bochs/VirtualBox port fallbacks.
 * Never returns on success; halts with cli if all paths fail. */
void acpi_shutdown(void);

/* Safe reboot. */
void acpi_reboot  (void);

/* S3 suspend-to-RAM.  Writes SLP_TYP=3 to the PM1a_CNT register.
 * The system suspends; resumption re-runs POST (BIOS handles the wakeup
 * vector in the FACS table).  Returns false if S3 is not supported. */
bool acpi_suspend_s3(void);

/* ACPI MADT walker.  Fills `out` with up to `max` LAPIC IDs of
 * application processors.  Returns count written (0 on uniprocessor). */
int acpi_madt_lapics(uint8_t *out, int max);

/* ---- CPU Performance Scaling ----------------------------------------- */
/* Detect Intel Enhanced SpeedStep / AMD P-state support.
 * Returns true if the CPU exposes MSR_IA32_PERF_CTL. */
bool cpu_perf_supported(void);

/* Get/set CPU performance as a percentage [0..100].
 * 0 = lowest P-state (power save), 100 = highest P-state (performance).
 * Returns -1 if not supported. */
int  cpu_perf_get(void);
void cpu_perf_set(int pct);

/* Human-readable description of current P-state, e.g. "800 MHz" or "P3". */
void cpu_perf_desc(char *buf, int sz);

#endif /* NEXXON_ACPI_H */
