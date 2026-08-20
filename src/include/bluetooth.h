/* ============================================================================
 * NexxoN OS - Bluetooth HCI/L2CAP stub  (v1.0)
 * ----------------------------------------------------------------------------
 * Detects USB Bluetooth controllers via the USB class 0xE0 / subclass 0x01
 * (Bluetooth) and provides:
 *   - HCI command/event channel (USB bulk + interrupt endpoints)
 *   - Device inquiry (name + address scanning)
 *   - L2CAP channel open for RFCOMM (serial over BT) and HID profiles
 *
 * The full protocol stack (pairing, link key exchange, A2DP audio, HID
 * input) is incremental; this header defines the stable API surface.
 * ============================================================================ */
#ifndef NEXXON_BLUETOOTH_H
#define NEXXON_BLUETOOTH_H

#include "types.h"

#define BT_ADDR_LEN    6
#define BT_NAME_MAX   32
#define BT_MAX_DEVICES 8

typedef enum {
    BT_STATE_DOWN         = 0,
    BT_STATE_INIT         = 1,
    BT_STATE_READY        = 2,
    BT_STATE_SCANNING     = 3,
    BT_STATE_NO_HARDWARE  = 4,
} bt_state_t;

typedef enum {
    BT_CLASS_UNKNOWN   = 0,
    BT_CLASS_PHONE     = 1,
    BT_CLASS_HEADSET   = 2,
    BT_CLASS_KEYBOARD  = 3,
    BT_CLASS_MOUSE     = 4,
    BT_CLASS_COMPUTER  = 5,
} bt_dev_class_t;

typedef struct {
    uint8_t      addr[BT_ADDR_LEN];
    char         name[BT_NAME_MAX + 1];
    bt_dev_class_t dev_class;
    int8_t       rssi;
    bool         paired;
} bt_device_t;

/* Initialise: probe USB bus for a Bluetooth controller.
 * Returns true if hardware is present. */
bool bt_init         (void);
bool bt_present      (void);
bt_state_t bt_state  (void);

/* Inquiry scan.  Fills `out` with up to `max` discovered devices.
 * Returns the number found (may be > max). */
int  bt_scan         (bt_device_t *out, int max);

/* Pair with a device.  pin may be NULL for SSP/auto-pair. */
bool bt_pair         (const bt_device_t *dev, const char *pin);

/* Open the graphical Bluetooth manager window. */
bool bt_manager_open (void);

#endif /* NEXXON_BLUETOOTH_H */
