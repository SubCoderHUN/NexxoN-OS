/* ============================================================================
 * NexxoN OS - Plug & Play daemon  (v1.0)
 * ----------------------------------------------------------------------------
 * Sits between the raw usb_poll() hot-plug events and the rest of the
 * system.  Owns the policy that decides:
 *
 *   * Mass-Storage devices  -> claim with msc_attach(), parse the partition
 *                              table, mount each FAT/NTFS partition under
 *                              "/usbN", and notify the desktop so an icon
 *                              appears on the file manager sidebar.
 *   * HID devices           -> hand off to the native usbhid driver so the
 *                              kernel input ring receives mouse / keyboard
 *                              packets without going through PS/2 legacy.
 *   * Anything else         -> log + ignore.
 *
 * On detach the daemon reverses the operation: vfs_unmount, msc_detach,
 * and a desktop notification.  Everything is polled from pnp_tick() which
 * the scheduler calls once per 100 Hz frame, so there is no IRQ wiring.
 * ============================================================================ */
#ifndef NEXXON_PNP_H
#define NEXXON_PNP_H

#include "types.h"

void pnp_init     (void);
void pnp_tick     (void);

int  pnp_mount_count (void);
/* Fills `out` with the i'th mount summary line (e.g. "/usb0  16.0 GiB  FAT32 [MY-PENDRIVE]").
 * Returns the number of bytes written or -1 if idx is out of range. */
int  pnp_mount_describe(int idx, char *out, size_t cap);

#endif /* NEXXON_PNP_H */
