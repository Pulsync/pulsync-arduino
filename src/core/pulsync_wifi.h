/**
 * pulsync_wifi — ESP32 WiFi manager (C API)
 *
 * Event-driven STA mode. Up to 3 stored profiles tried in priority order.
 * Auto-reconnect with exponential backoff on disconnect.
 * Triggers captive portal callback when all profiles fail.
 *
 * Uses ESP-IDF WiFi APIs (available in Arduino-ESP32 framework too).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PULSYNC_WIFI_MAX_PROFILES  3
#define PULSYNC_WIFI_SSID_MAXLEN   33  /* 32 chars + null */
#define PULSYNC_WIFI_PASS_MAXLEN   65  /* 64 chars + null */

/** WiFi connection state */
typedef enum {
    PULSYNC_WIFI_DISCONNECTED = 0,
    PULSYNC_WIFI_CONNECTING,
    PULSYNC_WIFI_CONNECTED,
    PULSYNC_WIFI_AP_MODE       /* captive portal active */
} pulsync_wifi_state_t;

/** WiFi profile */
typedef struct {
    char ssid[PULSYNC_WIFI_SSID_MAXLEN];
    char password[PULSYNC_WIFI_PASS_MAXLEN];
    bool in_use;
} pulsync_wifi_profile_t;

/** Callback when WiFi state changes */
typedef void (*pulsync_wifi_event_cb_t)(pulsync_wifi_state_t state);

/** Callback when all profiles fail — library should start captive portal */
typedef void (*pulsync_wifi_portal_cb_t)(void);

/**
 * Initialize WiFi subsystem (event loop, netif, STA mode).
 * Call once before any other WiFi function.
 */
bool pulsync_wifi_init(void);

/**
 * Add a WiFi profile (SSID + password). Up to 3 profiles.
 * Profiles are tried in the order they're added.
 * @param slot  Profile slot 0-2, or -1 for next available
 * @return true if profile was stored
 */
bool pulsync_wifi_add_profile(int slot, const char *ssid, const char *password);

/**
 * Remove all stored profiles. Does not disconnect an active connection.
 * Used when re-applying a fresh credential list.
 */
void pulsync_wifi_clear_profiles(void);

/**
 * Start WiFi connection. Tries profiles in order.
 * Non-blocking — connection events arrive via callback.
 */
void pulsync_wifi_start(void);

/**
 * Disconnect WiFi. Does not clear profiles.
 */
void pulsync_wifi_disconnect(void);

/**
 * Get current WiFi state.
 */
pulsync_wifi_state_t pulsync_wifi_get_state(void);

/**
 * Check if connected to an AP.
 */
bool pulsync_wifi_is_connected(void);

/**
 * Get assigned IP address as string (e.g., "192.168.1.100").
 * Returns empty string if not connected.
 */
const char *pulsync_wifi_get_ip(void);

/**
 * Register a callback for WiFi state changes.
 */
void pulsync_wifi_on_state_change(pulsync_wifi_event_cb_t cb);

/**
 * Register a callback for "all profiles failed" event.
 * Typically triggers captive portal.
 */
void pulsync_wifi_on_portal_needed(pulsync_wifi_portal_cb_t cb);

/**
 * Process WiFi tick — handles reconnect backoff timing.
 * Call periodically from main loop or network task.
 */
void pulsync_wifi_tick(void);

/**
 * Get current retry backoff time in ms (for diagnostics).
 */
uint32_t pulsync_wifi_get_backoff_ms(void);

#ifdef __cplusplus
}
#endif
