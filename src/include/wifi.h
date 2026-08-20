/* ============================================================================
 * NexxoN OS - Wi-Fi subsystem  (802.11 / WPA2-PSK)  v1.0
 * ----------------------------------------------------------------------------
 * Supports:
 *   - PCI scan for Atheros AR9xxx, Realtek RTL8188, Intel WiFi Link
 *     and virtio-net "wireless" simulation
 *   - SSID list management (simulated scan + real results when hardware
 *     responds to probe frames)
 *   - WPA2-PSK 4-way handshake preamble (PBKDF2-SHA1 key derivation)
 *   - Exposes wifi_scan, wifi_connect, wifi_disconnect, wifi_status
 *
 * In QEMU / VirtualBox there is no real 802.11 MAC so the scan returns
 * a synthetic list of networks; the connect path forces DHCP via the
 * existing net stack so browser/ping work immediately after association.
 * ============================================================================ */
#ifndef NEXXON_WIFI_H
#define NEXXON_WIFI_H

#include "types.h"

#define WIFI_SSID_MAX       32
#define WIFI_PASS_MAX       64
#define WIFI_MAX_NETWORKS   16
#define WIFI_BSSID_LEN       6

typedef enum {
    WIFI_SEC_OPEN   = 0,
    WIFI_SEC_WEP    = 1,
    WIFI_SEC_WPA    = 2,
    WIFI_SEC_WPA2   = 3,
} wifi_security_t;

typedef enum {
    WIFI_STATE_DOWN        = 0,
    WIFI_STATE_SCANNING    = 1,
    WIFI_STATE_CONNECTING  = 2,
    WIFI_STATE_ASSOCIATED  = 3,
    WIFI_STATE_NO_HARDWARE = 4,
} wifi_state_t;

typedef struct {
    char           ssid[WIFI_SSID_MAX + 1];
    uint8_t        bssid[WIFI_BSSID_LEN];
    int8_t         rssi;          /* dBm, e.g. -60 */
    uint8_t        channel;
    wifi_security_t security;
} wifi_network_t;

/* Initialise: PCI scan for wireless hardware. */
bool wifi_init          (void);
bool wifi_present       (void);
wifi_state_t wifi_state (void);

/* Scan for nearby networks.  Fills `out` with up to `max` entries.
 * Returns the number of networks found (may be > max). */
int  wifi_scan          (wifi_network_t *out, int max);

/* Connect to an SSID.  password may be NULL for open networks. */
bool wifi_connect       (const char *ssid, const char *password);

/* Disconnect and bring the interface down. */
void wifi_disconnect    (void);

/* Current SSID (empty string if not connected). */
void wifi_current_ssid  (char *out, int sz);

/* Open the graphical Wi-Fi manager window. */
bool wifi_manager_open  (void);

#endif /* NEXXON_WIFI_H */
