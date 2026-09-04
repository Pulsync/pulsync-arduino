/**
 * pulsync_portal — Captive portal for WiFi provisioning (C API)
 *
 * Starts a soft-AP + HTTP server with a web form for WiFi credentials.
 * Auto-triggered when no WiFi profiles connect.
 * Can also be triggered manually via GPIO button.
 *
 * Flow:
 *   1. WiFi module calls portal_needed callback
 *   2. Portal starts AP (SSID: "Pulsync-XXXX", open)
 *   3. User connects and gets redirected to captive portal form
 *   4. User enters SSID + password, submits
 *   5. Portal saves credentials, stops AP, triggers WiFi reconnect
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "pulsync_wifi_store.h"  /* pulsync_wifi_cred_t, PULSYNC_WIFI_MAX_CREDS */

#ifdef __cplusplus
extern "C" {
#endif

#define PULSYNC_PORTAL_AP_SSID_PREFIX  "Pulsync-"
#define PULSYNC_PORTAL_AP_CHANNEL      1
#define PULSYNC_PORTAL_AP_MAX_CONN     4
#define PULSYNC_PORTAL_HTTP_PORT       80

/** Portal state */
typedef enum {
    PULSYNC_PORTAL_STOPPED = 0,
    PULSYNC_PORTAL_STARTING,
    PULSYNC_PORTAL_RUNNING,
    PULSYNC_PORTAL_CREDENTIALS_RECEIVED
} pulsync_portal_state_t;

/** Portal mode */
typedef enum {
    PULSYNC_PORTAL_MODE_ENROLL = 0,   /* First-time setup: WiFi + pairing code + server */
    PULSYNC_PORTAL_MODE_CONFIG        /* Management: WiFi + server only, no pairing code, factory reset */
} pulsync_portal_mode_t;

/** Callback when credentials are received from portal form */
typedef void (*pulsync_portal_creds_cb_t)(const char *ssid, const char *password,
                                          const char *server, const char *pairing_code,
                                          const char *device_name);

/** Callback when factory reset is requested from config portal */
typedef void (*pulsync_portal_reset_cb_t)(void);

/**
 * Callback when the WiFi network list is saved from the config menu.
 * Receives the full ordered list (index 0 = highest priority). A cred with an
 * empty password means "keep the existing stored password for this SSID".
 */
typedef void (*pulsync_portal_wifi_save_cb_t)(const pulsync_wifi_cred_t *creds,
                                              int count);

/** Callback when the server address is saved from the config menu. */
typedef void (*pulsync_portal_server_save_cb_t)(const char *server);

/**
 * Callback when the user re-pairs from the config menu. Receives the new
 * pairing code. The handler should clear the current device token, persist the
 * new code, and trigger re-enrollment (the server issues a fresh token — and a
 * fresh config password — for the new pairing).
 */
typedef void (*pulsync_portal_repair_cb_t)(const char *pairing_code);

/**
 * Callback when the user presses Exit in the config menu. The portal has
 * already stopped its AP/HTTP/DNS by the time this fires; the handler should
 * (re)start the WiFi connection with the current credential list.
 */
typedef void (*pulsync_portal_exit_cb_t)(void);

/**
 * Set portal mode (enroll or config). Call before start.
 */
void pulsync_portal_set_mode(pulsync_portal_mode_t mode);

/**
 * Register callback for factory reset request (config mode only).
 */
void pulsync_portal_on_factory_reset(pulsync_portal_reset_cb_t cb);

/** Register callback for WiFi list save (config menu). */
void pulsync_portal_on_wifi_save(pulsync_portal_wifi_save_cb_t cb);

/** Register callback for server address save (config menu). */
void pulsync_portal_on_server_save(pulsync_portal_server_save_cb_t cb);

/** Register callback for re-pair (config menu). Receives the new pairing code. */
void pulsync_portal_on_repair(pulsync_portal_repair_cb_t cb);

/** Register callback for Exit (config menu). */
void pulsync_portal_on_exit(pulsync_portal_exit_cb_t cb);

/**
 * Provide the current saved-network list to the portal so the /wifi endpoint
 * can render it. Called before start (and after any change). SSIDs only are
 * exposed to the client; passwords are never sent.
 */
void pulsync_portal_set_wifi_list(const pulsync_wifi_cred_t *creds, int count);

/**
 * Configure the config-menu password gate. Call before start.
 *
 * In CONFIG mode, an enrolled device with a non-empty password hash requires
 * a login before the config menu (and its actions) are reachable. The AP stays
 * open — the gate is an app-layer login, not WPA2. ENROLL mode is never gated.
 *
 * @param pw_hash_hex  SHA-256 hex of the config password (server-issued). NULL
 *                     or empty disables the gate (device treated as open).
 * @param enrolled     Whether the device is enrolled. Unenrolled = always open.
 */
void pulsync_portal_set_gate(const char *pw_hash_hex, bool enrolled);

/**
 * Initialize portal module.
 */
void pulsync_portal_init(void);

/**
 * Start the captive portal (AP + HTTP server).
 * Non-blocking — AP and server start in background.
 * @return true if portal started successfully
 */
bool pulsync_portal_start(void);

/**
 * Stop the captive portal (AP + HTTP server).
 */
void pulsync_portal_stop(void);

/**
 * Get current portal state.
 */
pulsync_portal_state_t pulsync_portal_get_state(void);

/**
 * Check if portal is currently running.
 */
bool pulsync_portal_is_active(void);

/**
 * Register callback for when WiFi credentials are submitted.
 * Typically saves to WiFi profiles and restarts connection.
 */
void pulsync_portal_on_credentials(pulsync_portal_creds_cb_t cb);

/**
 * Set custom AP SSID suffix (appended to "Pulsync-").
 * If not set, last 4 hex chars of MAC address are used.
 */
void pulsync_portal_set_name(const char *suffix);

/**
 * Get the current AP SSID the portal broadcasts (e.g. "Pulsync-A1B2").
 * Valid after pulsync_portal_init().
 */
const char *pulsync_portal_get_ssid(void);

#ifdef __cplusplus
}
#endif
