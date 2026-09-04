/**
 * pulsync_heartbeat — Periodic heartbeat to server (C API)
 *
 * Sends device status at configurable intervals (minimum 30s).
 * Server responds with:
 *   - Config changes (key-value pairs)
 *   - OTA update availability (firmware URL + version)
 *   - Pending commands
 *   - Optional credential rotation (new token)
 *
 * Heartbeat runs on the background transport task via timer.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PULSYNC_HEARTBEAT_MIN_MS     30000   /* 30 seconds minimum */
#define PULSYNC_HEARTBEAT_DEFAULT_MS 30000   /* 30 seconds default */

/** Heartbeat response data passed to callbacks */
typedef struct {
    bool has_config;           /* server pushed config changes */
    bool has_ota;              /* OTA update available */
    bool has_commands;         /* pending commands */
    bool has_new_token;        /* credential rotation */
    const char *raw_json;      /* full response JSON for sub-modules to parse */
    size_t raw_json_len;
} pulsync_heartbeat_response_t;

/** Callback when heartbeat response is received */
typedef void (*pulsync_heartbeat_cb_t)(const pulsync_heartbeat_response_t *response);

/**
 * Initialize heartbeat module.
 * @param interval_ms  Heartbeat interval (clamped to minimum 30s)
 */
void pulsync_heartbeat_init(uint32_t interval_ms);

/**
 * Start sending heartbeats. Requires transport to be initialized.
 */
void pulsync_heartbeat_start(void);

/**
 * Stop heartbeats.
 */
void pulsync_heartbeat_stop(void);

/**
 * Tick function — checks if it's time to send a heartbeat.
 * Call from the main loop or background task.
 */
void pulsync_heartbeat_tick(void);

/**
 * Force an immediate heartbeat (e.g., after reconnect).
 */
void pulsync_heartbeat_send_now(void);

/**
 * Process heartbeat response from server.
 * Called by transport rx handler when response arrives.
 */
void pulsync_heartbeat_handle_response(const char *payload, size_t len);

/**
 * Update heartbeat interval (clamped to minimum 30s).
 */
void pulsync_heartbeat_set_interval(uint32_t interval_ms);

/**
 * Get current heartbeat interval.
 */
uint32_t pulsync_heartbeat_get_interval(void);

/**
 * Register callback for heartbeat responses.
 */
void pulsync_heartbeat_on_response(pulsync_heartbeat_cb_t cb);

/**
 * Get time (ms) since last successful heartbeat.
 */
uint32_t pulsync_heartbeat_time_since_last(void);

#ifdef __cplusplus
}
#endif
