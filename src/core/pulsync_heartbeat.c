/**
 * pulsync_heartbeat — Heartbeat implementation
 *
 * Periodically sends status JSON to server via transport layer.
 * Parses response for config/OTA/commands/credential rotation.
 * Dispatches to respective modules via callback.
 */

#include "pulsync_heartbeat.h"
#include "pulsync_transport.h"
#include "pulsync_scheduler.h"
#include "pulsync_nvs.h"
#include "pulsync_version.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "pulsync_heartbeat";

/* ---------- State ---------- */

static uint32_t s_interval_ms = PULSYNC_HEARTBEAT_DEFAULT_MS;
static uint32_t s_last_sent = 0;
static uint32_t s_last_success = 0;
static bool s_running = false;
static bool s_send_pending = false;
static pulsync_heartbeat_cb_t s_cb = NULL;

/* ---------- Internal helpers ---------- */

static bool json_has_key(const char *json, size_t len, const char *key) {
    if (!json || !key) return false;
    size_t key_len = strlen(key);
    const char *pos = json;
    const char *end = json + len;

    while (pos < end) {
        const char *found = strstr(pos, key);
        if (!found || found >= end) return false;
        /* Check it's a proper key (preceded by ") */
        if (found > json && *(found - 1) == '"') {
            const char *after = found + key_len;
            if (after < end && *after == '"') return true;
        }
        pos = found + 1;
    }
    return false;
}

/**
 * Extract a string value from flat JSON (same as enroll module).
 */
static size_t json_get_string(const char *json, size_t json_len,
                              const char *key, char *out, size_t out_maxlen) {
    if (!json || !key || !out || out_maxlen == 0) return 0;

    size_t key_len = strlen(key);
    const char *pos = json;
    const char *end = json + json_len;

    while (pos < end) {
        const char *found = strstr(pos, key);
        if (!found || found >= end) return 0;

        if (found > json && *(found - 1) == '"') {
            const char *after_key = found + key_len;
            if (after_key < end && *after_key == '"') {
                after_key++;
                while (after_key < end && (*after_key == ':' || *after_key == ' ')) after_key++;
                if (after_key < end && *after_key == '"') {
                    after_key++;
                    const char *value_start = after_key;
                    while (after_key < end && *after_key != '"') after_key++;
                    size_t value_len = (size_t)(after_key - value_start);
                    if (value_len >= out_maxlen) value_len = out_maxlen - 1;
                    memcpy(out, value_start, value_len);
                    out[value_len] = '\0';
                    return value_len;
                }
            }
        }
        pos = found + 1;
    }
    return 0;
}

static void build_heartbeat_payload(char *buf, size_t buflen) {
    uint32_t uptime_s = pulsync_millis() / 1000;
    uint32_t free_heap = esp_get_free_heap_size();

    snprintf(buf, buflen,
             "{\"uptime\":%lu,\"free_heap\":%lu,\"fw_version\":\"%s\",\"rssi\":%d}",
             (unsigned long)uptime_s,
             (unsigned long)free_heap,
             pulsync_version_get(),
             0  /* TODO: get actual RSSI from WiFi module */
    );
}

/* ---------- Public API ---------- */

void pulsync_heartbeat_init(uint32_t interval_ms) {
    if (interval_ms < PULSYNC_HEARTBEAT_MIN_MS) {
        interval_ms = PULSYNC_HEARTBEAT_MIN_MS;
    }
    s_interval_ms = interval_ms;
    s_running = false;
    s_send_pending = false;
    s_last_sent = 0;
    s_last_success = 0;
    ESP_LOGI(TAG, "Heartbeat interval: %lu ms", (unsigned long)s_interval_ms);
}

void pulsync_heartbeat_start(void) {
    s_running = true;
    s_last_sent = pulsync_millis();
    ESP_LOGI(TAG, "Heartbeat started");
}

void pulsync_heartbeat_stop(void) {
    s_running = false;
    ESP_LOGI(TAG, "Heartbeat stopped");
}

void pulsync_heartbeat_tick(void) {
    if (!s_running) return;
    if (!pulsync_transport_is_connected()) return;

    uint32_t now = pulsync_millis();
    if ((int32_t)(now - s_last_sent) < (int32_t)s_interval_ms && !s_send_pending) {
        return;
    }

    /* Time to send heartbeat */
    char payload[256];
    build_heartbeat_payload(payload, sizeof(payload));

    if (pulsync_transport_publish("heartbeat", payload, strlen(payload))) {
        s_last_sent = now;
        s_send_pending = false;
    } else {
        ESP_LOGW(TAG, "Failed to queue heartbeat");
    }
}

void pulsync_heartbeat_send_now(void) {
    s_send_pending = true;
    pulsync_heartbeat_tick();
}

void pulsync_heartbeat_handle_response(const char *payload, size_t len) {
    if (!payload || len == 0) return;

    s_last_success = pulsync_millis();

    /* Parse response flags */
    pulsync_heartbeat_response_t resp = {0};
    resp.raw_json = payload;
    resp.raw_json_len = len;
    resp.has_config = json_has_key(payload, len, "config");
    resp.has_ota = json_has_key(payload, len, "ota");
    resp.has_commands = json_has_key(payload, len, "commands");
    resp.has_new_token = json_has_key(payload, len, "new_token");

    /* Handle credential rotation inline */
    if (resp.has_new_token) {
        char new_token[128] = {0};
        if (json_get_string(payload, len, "new_token", new_token, sizeof(new_token)) > 0) {
            ESP_LOGI(TAG, "Credential rotation: updating token");
            pulsync_nvs_set_str(PULSYNC_NVS_KEY_TOKEN, new_token);
            pulsync_transport_set_token(new_token);
        }
    }

    /* Dispatch to callback (config, OTA, commands handled by higher-level modules) */
    if (s_cb) {
        s_cb(&resp);
    }
}

void pulsync_heartbeat_set_interval(uint32_t interval_ms) {
    if (interval_ms < PULSYNC_HEARTBEAT_MIN_MS) {
        interval_ms = PULSYNC_HEARTBEAT_MIN_MS;
    }
    s_interval_ms = interval_ms;
    ESP_LOGI(TAG, "Interval updated: %lu ms", (unsigned long)s_interval_ms);
}

uint32_t pulsync_heartbeat_get_interval(void) {
    return s_interval_ms;
}

void pulsync_heartbeat_on_response(pulsync_heartbeat_cb_t cb) {
    s_cb = cb;
}

uint32_t pulsync_heartbeat_time_since_last(void) {
    if (s_last_success == 0) return UINT32_MAX;
    return pulsync_millis() - s_last_success;
}
