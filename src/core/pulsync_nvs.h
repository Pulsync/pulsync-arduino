/**
 * pulsync_nvs — NVS management (C API)
 *
 * Initializes NVS flash, owns the 'pulsync' namespace.
 * Stores device token, WiFi profiles, config values.
 * Handles corruption by erasing and re-initializing.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** NVS keys used by the library */
#define PULSYNC_NVS_NAMESPACE    "pulsync"
#define PULSYNC_NVS_KEY_TOKEN    "dev_token"
#define PULSYNC_NVS_KEY_SERVER   "server_url"
#define PULSYNC_NVS_KEY_MQTT_URL "mqtt_url"   /* server-provided broker endpoint */
#define PULSYNC_NVS_KEY_TOKEN_SRV "token_srv" /* HTTP host the token was issued by */
#define PULSYNC_NVS_KEY_CFG_PW_HASH "cfg_pw_hash" /* SHA-256 hex of config-portal password */

/* ---- WiFi credential list (up to 3, ordered = priority) ----
 * Stored as indexed key pairs. wifi_cnt holds the number of valid slots.
 * wifi_seeded is set to 1 after the first-boot seed from setWiFi(); once set,
 * hardcoded setWiFi() creds are ignored and NVS is the source of truth.
 *
 * Keys are built at runtime: "wifi_ssid_0", "wifi_pw_0", etc.
 */
#define PULSYNC_WIFI_MAX_CREDS       3
#define PULSYNC_NVS_KEY_WIFI_COUNT   "wifi_cnt"
#define PULSYNC_NVS_KEY_WIFI_SEEDED  "wifi_seeded"
#define PULSYNC_NVS_KEY_WIFI_SSID_FMT "wifi_ssid_%d"
#define PULSYNC_NVS_KEY_WIFI_PW_FMT   "wifi_pw_%d"

/* Legacy single-profile keys (pre-multi-network). Read once for migration. */
#define PULSYNC_NVS_KEY_WIFI0_LEGACY    "wifi0"
#define PULSYNC_NVS_KEY_WIFI0_PW_LEGACY "wifi0_pw"

/**
 * Initialize NVS flash and open the pulsync namespace.
 * If NVS is corrupted, erases and re-inits.
 * Returns true on success, false if NVS partition is missing.
 */
bool pulsync_nvs_init(void);

/**
 * Read a string value from NVS.
 * @param key    NVS key
 * @param out    Output buffer
 * @param maxlen Buffer size (including null terminator)
 * @return true if key exists and was read, false otherwise
 */
bool pulsync_nvs_get_str(const char *key, char *out, size_t maxlen);

/**
 * Write a string value to NVS. Commits immediately.
 * @return true on success
 */
bool pulsync_nvs_set_str(const char *key, const char *value);

/**
 * Read a 32-bit integer from NVS.
 * @return true if key exists and was read
 */
bool pulsync_nvs_get_i32(const char *key, int32_t *out);

/**
 * Write a 32-bit integer to NVS. Commits immediately.
 */
bool pulsync_nvs_set_i32(const char *key, int32_t value);

/**
 * Delete a key from NVS.
 * @return true if key was deleted (or didn't exist)
 */
bool pulsync_nvs_erase_key(const char *key);

/**
 * Erase entire pulsync namespace. Used for factory reset.
 */
bool pulsync_nvs_erase_all(void);

/**
 * Check if NVS has been initialized successfully.
 */
bool pulsync_nvs_is_ready(void);

#ifdef __cplusplus
}
#endif
