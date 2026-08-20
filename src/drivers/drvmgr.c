/* ============================================================================
 * NexxoN OS - Unified driver / Plug-and-Play framework  (drvmgr, v1.0)
 * ----------------------------------------------------------------------------
 * Enumerates PCI + USB + platform buses into one device inventory, matches
 * each device against the driver registry, and binds / probes the driver.
 * See docs/DRIVER_FRAMEWORK.md and docs/DRIVER_FRAMEWORK_CHECKLIST.md.
 * ============================================================================ */
#include "driver.h"
#include "pci.h"
#include "usb.h"
#include "string.h"
#include "debug.h"

#define DRV_MAX_DRIVERS   48
#define DRV_MAX_DEVICES   80

static const drv_driver_t *g_drivers[DRV_MAX_DRIVERS];
static int                 g_n_drivers = 0;

static drv_device_t        g_devices[DRV_MAX_DEVICES];
static int                 g_n_devices = 0;

/* ---- Name helpers -------------------------------------------------------- */
const char *drv_bus_name(drv_bus_t b) {
    switch (b) {
        case DRV_BUS_PLATFORM: return "platform";
        case DRV_BUS_PCI:      return "pci";
        case DRV_BUS_USB:      return "usb";
        default:               return "?";
    }
}

const char *dev_class_name(dev_class_t c) {
    switch (c) {
        case DEV_CLASS_STORAGE:  return "Storage";
        case DEV_CLASS_NETWORK:  return "Network";
        case DEV_CLASS_DISPLAY:  return "Display";
        case DEV_CLASS_AUDIO:    return "Audio";
        case DEV_CLASS_USB_HOST: return "USB host";
        case DEV_CLASS_INPUT:    return "Input";
        case DEV_CLASS_HID:      return "HID";
        case DEV_CLASS_HUB:      return "Hub";
        case DEV_CLASS_BRIDGE:   return "Bridge";
        case DEV_CLASS_SERIAL:   return "Serial bus";
        case DEV_CLASS_SYSTEM:   return "System";
        default:                 return "Other";
    }
}

static dev_class_t pci_to_class(uint8_t cls, uint8_t sub) {
    switch (cls) {
        case 0x01: return DEV_CLASS_STORAGE;
        case 0x02: return DEV_CLASS_NETWORK;
        case 0x03: return DEV_CLASS_DISPLAY;
        case 0x04: return DEV_CLASS_AUDIO;
        case 0x06: return DEV_CLASS_BRIDGE;
        case 0x0C: return (sub == 0x03) ? DEV_CLASS_USB_HOST : DEV_CLASS_SERIAL;
        default:   return DEV_CLASS_OTHER;
    }
}

static dev_class_t usb_to_class(uint8_t iface_class) {
    switch (iface_class) {
        case 0x03: return DEV_CLASS_HID;
        case 0x08: return DEV_CLASS_STORAGE;
        case 0x09: return DEV_CLASS_HUB;
        default:   return DEV_CLASS_OTHER;
    }
}

/* ---- Registry ------------------------------------------------------------ */
bool drv_register(const drv_driver_t *drv) {
    if (!drv || g_n_drivers >= DRV_MAX_DRIVERS) return false;
    /* Ignore duplicate registrations (same pointer). */
    for (int i = 0; i < g_n_drivers; i++)
        if (g_drivers[i] == drv) return true;
    g_drivers[g_n_drivers++] = drv;
    return true;
}

/* ---- Matching ------------------------------------------------------------ */
static bool field_match16(uint16_t want, uint16_t have) {
    return want == DRV_ANY16 || want == have;
}
static bool field_match8(uint8_t want, uint8_t have) {
    return want == DRV_ANY8 || want == have;
}

static bool match_one(const drv_device_t *dev, const drv_match_t *m) {
    if (m->bus != dev->bus) return false;
    return field_match16(m->vendor_id, dev->vendor_id)
        && field_match16(m->device_id, dev->device_id)
        && field_match8 (m->class_code, dev->class_code)
        && field_match8 (m->subclass,  dev->subclass)
        && field_match8 (m->prog_if,   dev->prog_if);
}

static const drv_driver_t *find_driver(const drv_device_t *dev) {
    for (int i = 0; i < g_n_drivers; i++) {
        const drv_driver_t *d = g_drivers[i];
        for (int j = 0; j < d->n_matches; j++)
            if (match_one(dev, &d->matches[j])) return d;
    }
    return NULL;
}

/* Bind a single device to its driver (probe if framework-driven). */
static bool bind_device(drv_device_t *dev) {
    if (dev->driver) return false;                 /* already bound */
    const drv_driver_t *drv = find_driver(dev);
    if (!drv) { dev->status = "no driver"; return false; }
    dev->driver = drv;
    if (drv->probe) {
        bool ok = drv->probe(dev);
        dev->status = ok ? "active" : "probe failed";
    } else {
        dev->status = "built-in";                  /* self-initialised */
    }
    return true;
}

/* ---- Inventory builders -------------------------------------------------- */
static drv_device_t *new_device(void) {
    if (g_n_devices >= DRV_MAX_DEVICES) return NULL;
    drv_device_t *d = &g_devices[g_n_devices++];
    memset(d, 0, sizeof(*d));
    d->vendor_id = DRV_ANY16; d->device_id = DRV_ANY16;  /* placeholder */
    d->driver = NULL; d->status = "no driver";
    return d;
}

static void enum_pci(void) {
    pci_device_t devs[48];
    int n = pci_enumerate(devs, 48);
    for (int i = 0; i < n; i++) {
        drv_device_t *d = new_device();
        if (!d) break;
        d->bus        = DRV_BUS_PCI;
        d->vendor_id  = devs[i].vendor_id;
        d->device_id  = devs[i].device_id;
        d->class_code = devs[i].class_code;
        d->subclass   = devs[i].subclass;
        d->prog_if    = devs[i].prog_if;
        d->cls        = pci_to_class(devs[i].class_code, devs[i].subclass);
        d->loc[0] = devs[i].bus; d->loc[1] = devs[i].device; d->loc[2] = devs[i].function;
        ksnprintf(d->name, sizeof(d->name), "%s",
                  pci_class_name(devs[i].class_code, devs[i].subclass, devs[i].prog_if));
    }
}

static void enum_usb(void) {
    int n = usb_device_count();
    for (int i = 0; i < n; i++) {
        usb_device_t *u = usb_get_device(i);
        if (!u) continue;
        drv_device_t *d = new_device();
        if (!d) break;
        d->bus        = DRV_BUS_USB;
        d->vendor_id  = u->vendor_id;
        d->device_id  = u->product_id;
        d->class_code = u->iface_class ? u->iface_class : u->dev_class;
        d->subclass   = u->iface_subclass;
        d->prog_if    = u->iface_protocol;
        d->cls        = usb_to_class(d->class_code);
        d->loc[0] = (uint8_t)u->ctrl_idx; d->loc[1] = u->address; d->loc[2] = (uint8_t)u->port;
        usb_describe(u, d->name, sizeof(d->name));
    }
}

/* Fixed legacy devices that are always present on a PC.  class_code encodes a
 * synthetic "platform kind" the platform drivers match on. */
enum { PLAT_KBD=1, PLAT_MOUSE, PLAT_TIMER, PLAT_RTC, PLAT_DISPLAY, PLAT_SPEAKER };
static void add_platform(uint8_t kind, dev_class_t cls, const char *name) {
    drv_device_t *d = new_device();
    if (!d) return;
    d->bus = DRV_BUS_PLATFORM;
    d->class_code = kind;
    d->cls = cls;
    ksnprintf(d->name, sizeof(d->name), "%s", name);
}

static void enum_platform(void) {
    add_platform(PLAT_KBD,     DEV_CLASS_INPUT,   "PS/2 Keyboard");
    add_platform(PLAT_MOUSE,   DEV_CLASS_INPUT,   "PS/2 Mouse");
    add_platform(PLAT_TIMER,   DEV_CLASS_SYSTEM,  "PIT 8254 timer");
    add_platform(PLAT_RTC,     DEV_CLASS_SYSTEM,  "CMOS RTC");
    add_platform(PLAT_DISPLAY, DEV_CLASS_DISPLAY, "VESA framebuffer");
    add_platform(PLAT_SPEAKER, DEV_CLASS_AUDIO,   "PC speaker");
}

/* ===========================================================================
 * Built-in driver table — ADD A DRIVER = ADD A ROW.
 * For now every built-in is "self-initialised" (probe=NULL): kernel.c brings
 * it up in the tuned boot order and the framework records the binding.  As
 * drivers migrate to framework-driven init (Phase 2) they gain a probe().
 * =========================================================================== */
#define MPCI(v,d,c,s,p)  { DRV_BUS_PCI, (v),(d),(c),(s),(p) }
#define MUSB(c,s,p)      { DRV_BUS_USB, DRV_ANY16, DRV_ANY16, (c),(s),(p) }
#define MPLAT(kind)      { DRV_BUS_PLATFORM, DRV_ANY16, DRV_ANY16, (kind), DRV_ANY8, DRV_ANY8 }

static const drv_match_t m_ahci[]  = { MPCI(DRV_ANY16,DRV_ANY16,0x01,0x06,DRV_ANY8) };
static const drv_match_t m_ide[]   = { MPCI(DRV_ANY16,DRV_ANY16,0x01,0x01,DRV_ANY8) };
static const drv_match_t m_nic[]   = { MPCI(DRV_ANY16,DRV_ANY16,0x02,DRV_ANY8,DRV_ANY8) };
static const drv_match_t m_vga[]   = { MPCI(DRV_ANY16,DRV_ANY16,0x03,DRV_ANY8,DRV_ANY8) };
static const drv_match_t m_audio[] = { MPCI(DRV_ANY16,DRV_ANY16,0x04,DRV_ANY8,DRV_ANY8) };
static const drv_match_t m_usbhc[] = { MPCI(DRV_ANY16,DRV_ANY16,0x0C,0x03,DRV_ANY8) };
static const drv_match_t m_bridge[]= { MPCI(DRV_ANY16,DRV_ANY16,0x06,DRV_ANY8,DRV_ANY8) };
static const drv_match_t m_hid[]   = { MUSB(0x03,DRV_ANY8,DRV_ANY8) };
static const drv_match_t m_umsc[]  = { MUSB(0x08,DRV_ANY8,DRV_ANY8) };
static const drv_match_t m_uhub[]  = { MUSB(0x09,DRV_ANY8,DRV_ANY8) };
static const drv_match_t m_pkbd[]  = { MPLAT(PLAT_KBD) };
static const drv_match_t m_pmouse[]= { MPLAT(PLAT_MOUSE) };
static const drv_match_t m_ptimer[]= { MPLAT(PLAT_TIMER) };
static const drv_match_t m_prtc[]  = { MPLAT(PLAT_RTC) };
static const drv_match_t m_pdisp[] = { MPLAT(PLAT_DISPLAY) };
static const drv_match_t m_pspk[]  = { MPLAT(PLAT_SPEAKER) };

#define BUILTIN(nm,ver,mt) { nm, ver, mt, (int)(sizeof(mt)/sizeof((mt)[0])), NULL, NULL }
static const drv_driver_t g_builtin[] = {
    BUILTIN("ahci",     "1.0", m_ahci),
    BUILTIN("ata-ide",  "1.0", m_ide),
    BUILTIN("netif",    "1.0", m_nic),
    BUILTIN("vga-fb",   "1.0", m_vga),
    BUILTIN("ac97",     "1.0", m_audio),
    BUILTIN("usb-hcd",  "1.0", m_usbhc),
    BUILTIN("pci-bridge","1.0",m_bridge),
    BUILTIN("usbhid",   "1.0", m_hid),
    BUILTIN("usb-msc",  "1.0", m_umsc),
    BUILTIN("usb-hub",  "1.0", m_uhub),
    BUILTIN("ps2-kbd",  "1.0", m_pkbd),
    BUILTIN("ps2-mouse","1.0", m_pmouse),
    BUILTIN("pit",      "1.0", m_ptimer),
    BUILTIN("rtc",      "1.0", m_prtc),
    BUILTIN("vesafb",   "1.0", m_pdisp),
    BUILTIN("pcspk",    "1.0", m_pspk),
};

static void register_builtins(void) {
    for (unsigned i = 0; i < sizeof(g_builtin)/sizeof(g_builtin[0]); i++)
        drv_register(&g_builtin[i]);
}

/* ---- Public entry points ------------------------------------------------- */
void drvmgr_init(void) {
    debug_step("drvmgr: enumerating buses + matching drivers");
    register_builtins();
    g_n_devices = 0;
    enum_platform();
    enum_pci();
    enum_usb();

    int bound = 0, unknown = 0;
    for (int i = 0; i < g_n_devices; i++) {
        if (bind_device(&g_devices[i])) bound++;
        else if (!g_devices[i].driver)  unknown++;
    }
    debug_printf("[drvmgr] %d device(s), %d bound, %d without a driver\n",
                 g_n_devices, bound, unknown);
    debug_ok("drvmgr: device inventory ready");
}

int drvmgr_rematch(void) {
    int bound = 0;
    for (int i = 0; i < g_n_devices; i++)
        if (!g_devices[i].driver && bind_device(&g_devices[i])) bound++;
    return bound;
}

int  drvmgr_device_count(void) { return g_n_devices; }
const drv_device_t *drvmgr_device(int idx) {
    return (idx >= 0 && idx < g_n_devices) ? &g_devices[idx] : NULL;
}
int  drvmgr_driver_count(void) { return g_n_drivers; }
const drv_driver_t *drvmgr_driver(int idx) {
    return (idx >= 0 && idx < g_n_drivers) ? g_drivers[idx] : NULL;
}
int drvmgr_unbound_count(void) {
    int n = 0;
    for (int i = 0; i < g_n_devices; i++) if (!g_devices[i].driver) n++;
    return n;
}

int drvmgr_format(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    size_t n = 0;
    n += ksnprintf(out + n, cap - n,
                   "===============  NexxoN drivers (drvmgr)  ===============\n");
    n += ksnprintf(out + n, cap - n,
                   " %-5s %-9s %-22s %-11s %s\n",
                   "BUS", "CLASS", "DEVICE", "DRIVER", "STATUS");
    for (int i = 0; i < g_n_devices && n < cap - 1; i++) {
        const drv_device_t *d = &g_devices[i];
        n += ksnprintf(out + n, cap - n, " %-5s %-9s %-22s %-11s %s\n",
                       drv_bus_name(d->bus), dev_class_name(d->cls),
                       d->name, d->driver ? d->driver->name : "-",
                       d->status ? d->status : "");
    }
    n += ksnprintf(out + n, cap - n,
                   " %d device(s), %d driver(s) registered, %d without a driver\n",
                   g_n_devices, g_n_drivers, drvmgr_unbound_count());
    n += ksnprintf(out + n, cap - n,
                   "========================================================\n");
    return (int)n;
}
