/* ============================================================================
 * NexxoN OS - USB HID boot-protocol driver (keyboard + mouse)
 * ----------------------------------------------------------------------------
 * Native polling driver for USB HID keyboards and mice using the Boot
 * Protocol (HID 1.11 Appendix B).  It exists because the moment usb_init()
 * resets the host controllers, the BIOS USB-Legacy PS/2 emulation that was
 * feeding the PS/2 keyboard/mouse drivers stops working — so on a machine
 * whose only keyboard/mouse are USB (i.e. every modern desktop) the system
 * had no input at all.  This driver claims HID interfaces, switches them to
 * the boot protocol, and polls their interrupt-IN endpoint, translating each
 * report into the existing keyboard_handle_byte() / mouse_inject() paths so
 * the rest of the OS is none the wiser about how input arrived.
 *
 * Hot-swap: attach/detach are driven by the PnP daemon's hot-plug callback,
 * and usbhid_poll() ignores devices whose usb_device_t went away, so kbd/mice
 * can be plugged and unplugged at runtime.
 * ============================================================================ */
#ifndef NEXXON_USBHID_H
#define NEXXON_USBHID_H

#include "types.h"
#include "usb.h"

/* Claim a freshly enumerated HID device (keyboard proto=1 / mouse proto=2).
 * Returns true if the device was taken over. */
bool usbhid_attach(usb_device_t *dev);

/* Release a HID device that has been unplugged (also releases any keys it
 * was holding so modifiers can't get stuck). */
void usbhid_detach(usb_device_t *dev);

/* Poll every claimed HID device for new reports.  Call periodically from
 * the PnP tick. */
void usbhid_poll(void);

/* Count the currently-bound native HID devices, split by kind.  Either pointer
 * may be NULL.  Used by the boot-time USB takeover to verify that native input
 * actually came up before relying on it. */
void usbhid_counts(int *keyboards, int *mice);

#endif /* NEXXON_USBHID_H */
