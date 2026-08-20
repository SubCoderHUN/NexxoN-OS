/* ============================================================================
 * NexxoN OS - Unified driver / Plug-and-Play framework  (drvmgr, v1.0)
 * ----------------------------------------------------------------------------
 * One place that enumerates every bus (PCI, USB, platform), matches each
 * discovered device against a registry of drivers, and binds/brings up the
 * matching driver.  See docs/DRIVER_FRAMEWORK.md for the design.
 *
 * Future-proofing baked in:
 *   - Drivers are described by a small vtable + a wildcard MATCH TABLE, so a
 *     driver can bind by exact ID (one specific NIC) or by class (any AHCI).
 *   - drv_register() lets a driver self-register; an nxl-loaded .nxd module
 *     uses the SAME entry point, so modular drivers plug in identically.
 *   - probe()==NULL marks a "self-initialised" driver (brought up by kernel.c
 *     in the tuned boot order); the framework just records the binding.  A
 *     non-NULL probe() is called by the framework -> the path used by new and
 *     future modular/repo drivers.
 * ============================================================================ */
#ifndef NEXXON_DRIVER_H
#define NEXXON_DRIVER_H

#include "types.h"

/* ---- Buses the framework knows how to enumerate -------------------------- */
typedef enum {
    DRV_BUS_PLATFORM = 0,   /* fixed/legacy: PS/2, PIT, RTC, VGA, speaker     */
    DRV_BUS_PCI,
    DRV_BUS_USB,
    DRV_BUS__COUNT
} drv_bus_t;

/* ---- Coarse device class (cross-bus, for grouping + the UI) -------------- */
typedef enum {
    DEV_CLASS_OTHER = 0,
    DEV_CLASS_STORAGE,
    DEV_CLASS_NETWORK,
    DEV_CLASS_DISPLAY,
    DEV_CLASS_AUDIO,
    DEV_CLASS_USB_HOST,
    DEV_CLASS_INPUT,
    DEV_CLASS_HID,
    DEV_CLASS_HUB,
    DEV_CLASS_BRIDGE,
    DEV_CLASS_SERIAL,
    DEV_CLASS_SYSTEM,
    DEV_CLASS__COUNT
} dev_class_t;

const char *dev_class_name(dev_class_t c);
const char *drv_bus_name(drv_bus_t b);

/* ---- A discovered hardware device (bus-agnostic identity) ---------------- */
struct drv_driver;   /* fwd */

typedef struct drv_device {
    drv_bus_t    bus;
    dev_class_t  cls;
    uint16_t     vendor_id;     /* PCI vendor / USB idVendor                  */
    uint16_t     device_id;     /* PCI device / USB idProduct                 */
    uint8_t      class_code;    /* PCI class  / USB bInterfaceClass           */
    uint8_t      subclass;      /* PCI sub    / USB bInterfaceSubClass        */
    uint8_t      prog_if;       /* PCI prog-if/ USB bInterfaceProtocol        */
    uint8_t      loc[3];        /* PCI: bus,dev,fn  |  USB: ctrl,addr,port     */
    char         name[40];      /* human-readable description                 */
    const struct drv_driver *driver;   /* bound driver, or NULL              */
    const char  *status;        /* "active" / "no driver" / "built-in" ...    */
} drv_device_t;

/* ---- Match descriptor: which devices a driver claims --------------------- *
 * Any field set to DRV_ANY* is a wildcard.  Match = bus equal AND every
 * non-wildcard field equal. */
#define DRV_ANY16  0xFFFFu
#define DRV_ANY8   0xFFu
typedef struct drv_match {
    drv_bus_t bus;
    uint16_t  vendor_id;
    uint16_t  device_id;
    uint8_t   class_code;
    uint8_t   subclass;
    uint8_t   prog_if;
} drv_match_t;

/* ---- Driver descriptor (vtable + match table) ---------------------------- */
typedef struct drv_driver {
    const char        *name;
    const char        *version;
    const drv_match_t *matches;     /* array of n_matches descriptors         */
    int                n_matches;
    /* Called by the framework when a device matches.  Return true if the
     * device was claimed/initialised OK.  NULL => self-initialised driver
     * (framework only records the binding, does not init). */
    bool             (*probe)(drv_device_t *dev);
    /* Optional one-line status string (e.g. link state, sectors). */
    const char      *(*status)(void);
} drv_driver_t;

/* ---- Registry / framework API -------------------------------------------- */

/* Register a driver.  Safe to call at boot or at runtime (modular drivers).
 * Returns false only if the registry is full. */
bool drv_register(const drv_driver_t *drv);

/* Enumerate every bus, match the registry, bind drivers, probe framework-
 * driven ones.  Call once at boot AFTER the self-initialised core drivers. */
void drvmgr_init(void);

/* Re-run matching for any still-unbound device (call after a driver registers
 * at runtime, e.g. an nxl-loaded module).  Returns # newly bound. */
int  drvmgr_rematch(void);

/* ---- Inventory access (Device Manager window / `drivers` command) -------- */
int  drvmgr_device_count(void);
const drv_device_t *drvmgr_device(int idx);
int  drvmgr_driver_count(void);
const drv_driver_t *drvmgr_driver(int idx);
/* # of enumerated devices with no bound driver (unknown hardware). */
int  drvmgr_unbound_count(void);

/* Render a compact text report (used by the `drivers` shell command). */
int  drvmgr_format(char *out, size_t cap);

#endif /* NEXXON_DRIVER_H */
