/**
 * pulsync_ota — OTA firmware update manager (C API)
 *
 * Triggered by server via heartbeat response or WSS push.
 * Uses ESP-IDF OTA APIs (esp_ota_ops, esp_https_ota) with rollback support.
 *
 * Flow:
 *   1. Server signals OTA available (URL + version in heartbeat or WSS message)
 *   2. Library compares version to current PULSYNC_FW_VERSION
 *   3. If newer, downloads firmware from URL
 *   4. Writes to next OTA partition
 *   5. Marks new partition as boot, restarts
 *   6. After restart, app confirms (or auto-rollback on crash)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** OTA state */
typedef enum {
    PULSYNC_OTA_IDLE = 0,
    PULSYNC_OTA_AVAILABLE,     /* server indicated update available */
    PULSYNC_OTA_DOWNLOADING,   /* download in progress */
    PULSYNC_OTA_VERIFYING,     /* verifying downloaded image */
    PULSYNC_OTA_READY,         /* ready to reboot */
    PULSYNC_OTA_FAILED         /* OTA failed */
} pulsync_ota_state_t;

/** OTA info from server */
typedef struct {
    char url[256];             /* firmware download URL */
    char version[32];          /* target version (semver) */
    uint32_t size;             /* firmware size in bytes (0 if unknown) */
    char checksum[65];         /* expected sha256 of the image (hex), or "" */
} pulsync_ota_info_t;

/** OTA progress callback */
typedef void (*pulsync_ota_progress_cb_t)(uint32_t downloaded, uint32_t total);

/** OTA completion callback */
typedef void (*pulsync_ota_complete_cb_t)(bool success, const char *version);

/**
 * Initialize OTA module. Call once at startup.
 * Also confirms the current partition if running after an OTA update.
 */
void pulsync_ota_init(void);

/**
 * Whether OTA is possible on this device.
 * false when the flashed partition table lacks a second app slot
 * (ota_0/ota_1 + otadata). Valid only after pulsync_ota_init().
 * @return true if the device can receive OTA updates
 */
bool pulsync_ota_is_available(void);

/**
 * Check if an OTA update should be applied.
 * Compares offered version against current PULSYNC_FW_VERSION.
 * @param version  Server-offered version string (semver)
 * @return true if version is newer than current
 */
bool pulsync_ota_should_update(const char *version);

/**
 * Start OTA download and flash.
 * Non-blocking — runs on a separate FreeRTOS task.
 * @param info  OTA info (URL, version, size)
 * @return true if OTA started, false if already in progress
 */
bool pulsync_ota_start(const pulsync_ota_info_t *info);

/**
 * Handle OTA trigger from heartbeat or WSS message.
 * Parses the JSON "ota" object and starts update if applicable.
 * @param json      JSON payload containing OTA info
 * @param json_len  Payload length
 */
void pulsync_ota_handle_trigger(const char *json, size_t json_len);

/**
 * Get current OTA state.
 */
pulsync_ota_state_t pulsync_ota_get_state(void);

/**
 * Register progress callback (called periodically during download).
 */
void pulsync_ota_on_progress(pulsync_ota_progress_cb_t cb);

/**
 * Register completion callback.
 */
void pulsync_ota_on_complete(pulsync_ota_complete_cb_t cb);

/**
 * Manually trigger a reboot after successful OTA.
 * If auto-reboot is disabled, call this to apply the update.
 */
void pulsync_ota_reboot(void);

/**
 * Mark current firmware as valid (confirms OTA partition).
 * Called automatically in pulsync_ota_init(). Can also be called
 * manually after the app verifies it's working correctly.
 */
void pulsync_ota_confirm(void);

/**
 * Rollback to previous firmware (if available).
 */
void pulsync_ota_rollback(void);

#ifdef __cplusplus
}
#endif
