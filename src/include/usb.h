/* ============================================================================
 * NexxoN OS - USB host stack (UHCI + EHCI + Mass Storage + HID)  (v2.0)
 * ----------------------------------------------------------------------------
 * Maximum-effort rewrite of the v1.0 USB skeleton.  This header is the
 * single public API for every USB-related milestone (TASK 3 / TASK 4 /
 * TASK 26):
 *
 *   * usb_init() probes the PCI fabric for UHCI / OHCI / EHCI / xHCI
 *     host controllers, maps their register windows, takes ownership
 *     from the BIOS legacy SMI handlers, and brings up the root hubs.
 *   * usb_poll() runs the periodic plug-and-play loop: it walks every
 *     controller, samples its PORTSC bits, and fires the hot-plug
 *     callbacks for newly attached / detached devices.
 *   * usb_control_transfer() / usb_bulk_in() / usb_bulk_out() expose a
 *     SETUP / IN / OUT transaction interface used by the mass-storage
 *     and HID class drivers.  UHCI runs the transactions through a
 *     queue head + transfer-descriptor chain in physical RAM, looped
 *     through the framelist; EHCI uses its async queue head with a
 *     freshly stitched qTD chain.
 *   * usb_set_hotplug_cb() lets a higher-level subsystem (the PnP daemon,
 *     the VFS auto-mounter) register a callback that fires whenever a
 *     new device is enumerated or an existing one disappears.
 *
 * The implementation is deliberately polled - no IRQ wiring - because
 * NexxoN OS still has a cooperative scheduler.  Polling the controllers
 * once per scheduler tick is cheap (PORTSC reads are a single MMIO load
 * each) and removes a whole class of locking concerns.
 * ============================================================================ */
#ifndef NEXXON_USB_H
#define NEXXON_USB_H

#include "types.h"

#define USB_CTRL_UNKNOWN 0
#define USB_CTRL_UHCI    1
#define USB_CTRL_OHCI    2
#define USB_CTRL_EHCI    3
#define USB_CTRL_XHCI    4

#define USB_MAX_PORTS    8
#define USB_MAX_DEVICES  8

/* Device classes we care about. */
#define USB_CLASS_HID    0x03
#define USB_CLASS_MSC    0x08
#define USB_CLASS_HUB    0x09
#define USB_CLASS_VENDOR 0xFF

/* Standard endpoint / transfer types. */
#define USB_DIR_OUT      0x00
#define USB_DIR_IN       0x80

#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_SET_CONFIG      0x09

#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05

/* Setup packet layout per USB 2.0 §9.3. */
typedef struct PACKED {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

/* Device descriptor (USB 2.0 §9.6.1). */
typedef struct PACKED {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_descriptor_t;

/* Configuration descriptor (truncated to the header). */
typedef struct PACKED {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_descriptor_t;

/* Interface descriptor. */
typedef struct PACKED {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_interface_descriptor_t;

/* Endpoint descriptor. */
typedef struct PACKED {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_descriptor_t;

typedef struct {
    bool     present;
    int      kind;
    uint16_t io_base;
    uint32_t mmio_base;
    uint8_t  irq;
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  fn;
    uint16_t vendor_id;
    uint16_t device_id;
    int      num_ports;
    bool     port_attached[USB_MAX_PORTS];
    bool     port_was_attached[USB_MAX_PORTS];   /* prev tick - for hotplug */
    int8_t   xhci_hc;   /* index into the xHCI instance pool; -1 = none    */
} usb_controller_t;

/* Per-enumerated-device handle.  The class-driver layer (MSC / HID) hangs
 * its own state off of `private_data` once it claims the device. */
typedef struct usb_device {
    bool        in_use;
    int         ctrl_idx;
    int         port;
    uint8_t     address;             /* 1..127 once SET_ADDRESS succeeded */
    uint8_t     speed;               /* 0=low, 1=full, 2=high             */
    uint8_t     tt_hub_addr;         /* TT hub USB address (0 = root/UHCI) */
    uint8_t     tt_hub_port;         /* TT hub port number (1-based)       */
    uint8_t     ep_in;               /* bulk-IN endpoint                  */
    uint8_t     ep_out;              /* bulk-OUT endpoint                 */
    uint16_t    ep_in_mps;           /* max packet size for IN endpoint   */
    uint16_t    ep_out_mps;
    uint8_t     dev_class;
    uint8_t     dev_subclass;
    uint8_t     dev_protocol;
    uint16_t    vendor_id;
    uint16_t    product_id;
    char        product_str[32];
    uint8_t     iface_num;
    uint8_t     iface_class;
    uint8_t     iface_subclass;
    uint8_t     iface_protocol;
    uint8_t     ep_in_interval;      /* bInterval of the chosen IN endpoint */
    uint8_t     n_ifaces;            /* interfaces seen in the config (diag) */
    bool        bot_ready;           /* MSC Bulk-Only-Transport probed OK */
    uint8_t     toggle_in;           /* DATA0/DATA1 toggle, IN endpoint   */
    uint8_t     toggle_out;
    void       *private_data;        /* class driver bookkeeping          */
} usb_device_t;

/* Hot-plug callback signature. */
typedef void (*usb_hotplug_cb_t)(usb_device_t *dev, bool attached);

/* ---------- Public API ------------------------------------------------- */

int  usb_init             (void);
int  usb_controller_count (void);
bool usb_get_controller   (int idx, usb_controller_t *out);

/* Switch UHCI / EHCI controllers out of BIOS legacy emulation. */
bool usb_disable_legacy   (void);

/* True when usb_init() preserved firmware USB-legacy input (did not reset the
 * host controllers) because BIOS SMM still drives the boot keyboard/mouse. */
bool usb_legacy_preserved (void);

/* Leave BIOS-legacy preserve mode and bring the USB stack up NATIVELY at
 * runtime: hand the controllers off from firmware SMM, reset, and enumerate
 * everything (keyboard + mouse + storage), incl. HID behind the chipset
 * rate-matching hub.  This is how a native USB mouse becomes available on a
 * box whose firmware emulates only the legacy keyboard.  Returns the number of
 * USB devices enumerated.  A reboot returns to the SMM keyboard if needed. */
int  usb_force_native     (void);

/* Non-destructive pre-flight for an AUTOMATIC boot-time native takeover.  Reads
 * (never writes) the detected xHCI controllers' port state and decides whether
 * an auto-takeover is safe + worthwhile on this hardware: it requires an add-in
 * xHCI (non-zero PCI bus) — so the chipset USB hosting the boot device and the
 * SMM input emulation is left untouched — carrying at least one Full/Low-Speed
 * device (a HID keyboard/mouse, the only thing the takeover gains and the input
 * we must drive natively before its SMM emulation is disabled).  Fills `why`
 * with a one-line reason for the boot log.  Returns the number of FS/LS
 * connections found (0 = do not auto-take-over). */
int  usb_native_takeover_advisable(char *why, size_t cap);

/* Per-controller port summary (connection + speed) from the last
 * usb_force_native() takeover, for on-screen bare-metal diagnostics. */
const char *usb_takeover_diag(void);

/* Brutal xHCI state dump (controller status, event ring, per-slot endpoint
 * state, PORTSC) for diagnosing why interrupt reports don't flow.  Writes a
 * multi-line report into `out`; returns bytes written. */
int  xhci_brutal_dump(char *out, size_t cap);

/* Periodic plug-and-play tick.  Returns the number of state changes
 * detected this call (0 = nothing happened). */
int  usb_poll             (void);

/* Register / clear the hot-plug callback fired by usb_poll when a new
 * device arrives or an existing one disappears. */
void usb_set_hotplug_cb   (usb_hotplug_cb_t cb);

/* ---------- Device introspection -------------------------------------- */
int  usb_device_count     (void);
usb_device_t *usb_get_device(int idx);
/* Returns NULL when idx is out of range. */
usb_device_t *usb_device_by_address(uint8_t address);

/* ---------- Transfer API (used by class drivers) ---------------------- *
 * All transfer entry points return 0 on success, negative on failure.
 * `dev` must have been returned by usb_get_device() / a hot-plug event. */
int  usb_control_transfer (usb_device_t *dev,
                           const usb_setup_t *setup,
                           void *data, uint16_t length);
int  usb_bulk_out         (usb_device_t *dev, const void *data, uint32_t len);
int  usb_bulk_in          (usb_device_t *dev, void *data, uint32_t cap,
                           uint32_t *out_len);
/* Interrupt-IN poll for HID drivers.  Quiet + bounded-spin so polling an
 * idle (NAKing) keyboard/mouse endpoint returns fast.  spin_budget=0 keeps
 * the default.  Returns 0 with *out_len>0 on a report, negative otherwise. */
int  usb_intr_in          (usb_device_t *dev, void *data, uint32_t cap,
                           uint32_t *out_len, uint32_t spin_budget);

/* ---------- Diagnostics ----------------------------------------------- *
 * Renders a single-line human-readable description of a device for the
 * device manager / shell `lsusb` command.  Returns the number of bytes
 * written to `out` (excluding the NUL terminator). */
int  usb_describe         (const usb_device_t *dev, char *out, size_t cap);

#endif /* NEXXON_USB_H */
