/**
 * pulsync_transport — Device communication layer (C API)
 *
 * Two transport modes:
 *   MQTT — persistent connection for data, heartbeat, config, commands
 *   HTTP — on-demand for enrollment and OTA downloads
 *
 * MQTT features:
 *   - Auto-reconnect with exponential backoff (1s → 30s max)
 *   - Offline message queue (in-RAM ring buffer, sends on reconnect)
 *   - QoS 1 for all publishes (at-least-once delivery)
 *   - Subscribes to download/config and download/commands
 *
 * HTTP features:
 *   - Enrollment (POST /api/device/enroll)
 *   - OTA download (GET firmware URL)
 *   - Reuses single HTTP client (no memory leak)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Configuration ---------- */

#define PULSYNC_SERVER_URL_MAXLEN  128
#define PULSYNC_TOKEN_MAXLEN      128
#define PULSYNC_OFFLINE_QUEUE_SIZE 20   /* messages buffered when MQTT is down */

/** Transport state */
typedef enum {
    PULSYNC_TRANSPORT_DISCONNECTED = 0,
    PULSYNC_TRANSPORT_CONNECTING,
    PULSYNC_TRANSPORT_CONNECTED
} pulsync_transport_state_t;

/** Transport config — set before init */
typedef struct {
    char server_host[64];         /* e.g., "10.212.89.160" or "api.pulsync.in" */
    uint16_t http_port;           /* HTTP port (default 3001 local, 443 hosted) */
    uint16_t mqtt_port;           /* MQTT port (default 1883 local, 8883 hosted) */
    char device_token[PULSYNC_TOKEN_MAXLEN];
    bool use_tls;                 /* TLS for both MQTT and HTTP */
} pulsync_transport_config_t;

/** Callback for received messages (commands, config from server) */
typedef void (*pulsync_transport_rx_cb_t)(const char *topic, const char *payload, size_t len);

/** Callback for transport state changes */
typedef void (*pulsync_transport_state_cb_t)(pulsync_transport_state_t state);

/* ---------- Initialization ---------- */

/**
 * Initialize transport. Call once after WiFi init.
 */
bool pulsync_transport_init(const pulsync_transport_config_t *config);

/**
 * Start MQTT connection. Non-blocking — connects in background.
 */
void pulsync_transport_start(void);

/**
 * Stop transport — disconnects MQTT.
 */
void pulsync_transport_stop(void);

/* ---------- MQTT (data, heartbeat, config, commands) ---------- */

/**
 * Publish a message via MQTT.
 * If disconnected, queues in offline buffer (up to PULSYNC_OFFLINE_QUEUE_SIZE).
 * @param subtopic  "data" or "heartbeat"
 * @param payload   JSON string
 * @param len       Payload length
 * @return true if sent or queued
 */
bool pulsync_transport_publish(const char *subtopic, const char *payload, size_t len);

/**
 * Check if MQTT is connected.
 */
bool pulsync_transport_is_connected(void);

/**
 * Get current transport state.
 */
pulsync_transport_state_t pulsync_transport_get_state(void);

/**
 * Number of messages currently held in the offline queue.
 * Use for backpressure: if this approaches capacity, the link is down
 * and non-critical sends can be skipped by the caller.
 */
int pulsync_transport_queue_depth(void);

/**
 * Maximum offline queue capacity (PULSYNC_OFFLINE_QUEUE_SIZE).
 */
int pulsync_transport_queue_capacity(void);

/* ---------- HTTP (enrollment, OTA) ---------- */

/**
 * Send an HTTP POST request (for enrollment).
 * Blocking — waits for response.
 * @param path       URL path (e.g., "/api/device/enroll")
 * @param payload    JSON body
 * @param len        Body length
 * @param response   Buffer to store response body
 * @param resp_max   Response buffer size
 * @return true if request succeeded (2xx status)
 */
bool pulsync_transport_http_post(const char *path, const char *payload, size_t len,
                                  char *response, size_t resp_max);

/**
 * Set the HTTP request timeout in milliseconds (applies to subsequent
 * enrollment/probe requests). Discovery uses a short timeout so a dead server
 * fails fast; normal operation can use a longer one. Default 10000ms.
 */
void pulsync_transport_set_http_timeout(int timeout_ms);

/* ---------- Callbacks ---------- */

/**
 * Register callback for incoming messages (config, commands).
 */
void pulsync_transport_on_receive(pulsync_transport_rx_cb_t cb);

/**
 * Register callback for state changes.
 */
void pulsync_transport_on_state_change(pulsync_transport_state_cb_t cb);

/* ---------- Token management ---------- */

/**
 * Update device token (after enrollment or rotation).
 */
void pulsync_transport_set_token(const char *token);

/**
 * Update server configuration (HTTP target). MQTT target defaults to the same
 * host unless overridden via pulsync_transport_set_mqtt_url().
 */
void pulsync_transport_set_server(const char *host, uint16_t http_port, uint16_t mqtt_port, bool use_tls);

/**
 * Override the MQTT broker target from a full URL, e.g.
 *   "mqtt://192.168.0.103:1883"  or  "mqtts://mqtt.pulsync.in:8883"
 * The server hands this back in the enroll response / heartbeat so the device
 * doesn't derive the broker location from the HTTP host. A ".local" host is
 * resolved via mDNS at connect time. Passing NULL/empty clears the override
 * (falls back to deriving from the HTTP server host).
 *
 * If the new target differs from the current one and MQTT is running, the
 * caller should stop/start the transport to reconnect to the new broker.
 * @return true if the parsed target differs from the previous MQTT target.
 */
bool pulsync_transport_set_mqtt_url(const char *url);

/**
 * Get the MQTT target currently in use as a URL string (scheme://host:port).
 * Writes into out. Useful for change-detection / diagnostics.
 */
void pulsync_transport_get_mqtt_url(char *out, size_t out_len);

/**
 * Parse server URL string into host/port/tls.
 * Heuristic: IP → no TLS, domain → TLS. Explicit scheme overrides.
 */
void pulsync_transport_parse_url(const char *url, char *host, size_t host_len,
                                  uint16_t *http_port, uint16_t *mqtt_port, bool *use_tls);

#ifdef __cplusplus
}
#endif
