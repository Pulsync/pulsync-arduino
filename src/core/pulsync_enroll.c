/**
 * pulsync_enroll — Device enrollment implementation
 *
 * Uses transport layer to POST pairing code to /api/device/enroll.
 * Parses JSON response for token. Stores token in NVS.
 */

#include "pulsync_enroll.h"
#include "pulsync_nvs.h"
#include "pulsync_transport.h"
#include "pulsync_version.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "pulsync_enroll";

/* ---------- State ---------- */

static char s_pairing_code[PULSYNC_PAIRING_CODE_MAXLEN] = {0};
static char s_device_token[PULSYNC_TOKEN_MAXLEN] = {0};
static pulsync_enroll_state_t s_state = PULSYNC_ENROLL_IDLE;
static pulsync_enroll_cb_t s_cb = NULL;

/* ---------- Simple JSON parsing (no external deps) ---------- */

/**
 * Extract a string value for a given key from a JSON payload.
 * Minimal parser — only handles flat objects with string values.
 * Returns length of extracted value, 0 if not found.
 */
static size_t json_extract_string(const char *json, size_t json_len,
                                  const char *key, char *out, size_t out_maxlen) {
    if (!json || !key || !out || out_maxlen == 0) return 0;

    /* Search for "key": */
    size_t key_len = strlen(key);
    const char *pos = json;
    const char *end = json + json_len;

    while (pos < end) {
        /* Find the key */
        const char *found = strstr(pos, key);
        if (!found || found >= end) return 0;

        /* Verify it's properly quoted: "key" */
        if (found > json && *(found - 1) == '"') {
            const char *after_key = found + key_len;
            if (after_key < end && *after_key == '"') {
                /* Skip to colon and opening quote of value */
                after_key++;  /* skip closing " of key */
                while (after_key < end && (*after_key == ':' || *after_key == ' ')) after_key++;
                if (after_key < end && *after_key == '"') {
                    after_key++;  /* skip opening " of value */
                    const char *value_start = after_key;
                    /* Find closing quote */
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

/* ---------- Public API ---------- */

void pulsync_enroll_init(const char *pairing_code) {
    if (pairing_code) {
        strncpy(s_pairing_code, pairing_code, PULSYNC_PAIRING_CODE_MAXLEN - 1);
        s_pairing_code[PULSYNC_PAIRING_CODE_MAXLEN - 1] = '\0';
    }
    s_state = PULSYNC_ENROLL_IDLE;
    s_device_token[0] = '\0';
}

bool pulsync_enroll_check(void) {
    /* Try to load token from NVS */
    if (pulsync_nvs_get_str(PULSYNC_NVS_KEY_TOKEN, s_device_token, PULSYNC_TOKEN_MAXLEN)) {
        if (s_device_token[0] != '\0') {
            ESP_LOGI(TAG, "Device already enrolled (token in NVS)");
            s_state = PULSYNC_ENROLL_DONE;

            /* Push token to transport layer */
            pulsync_transport_set_token(s_device_token);
            return true;
        }
    }

    ESP_LOGI(TAG, "No token in NVS — enrollment needed");
    s_state = PULSYNC_ENROLL_NEEDED;
    return false;
}

void pulsync_enroll_start(void) {
    if (s_state == PULSYNC_ENROLL_DONE) {
        ESP_LOGI(TAG, "Already enrolled, skipping");
        return;
    }

    if (s_pairing_code[0] == '\0') {
        ESP_LOGE(TAG, "No pairing code set");
        s_state = PULSYNC_ENROLL_FAILED;
        if (s_cb) s_cb(false, NULL);
        return;
    }

    /* Get MAC address */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Get device name from NVS (if set via portal) */
    char dev_name[33] = {0};
    pulsync_nvs_get_str("dev_name", dev_name, sizeof(dev_name));

    /* Build enrollment JSON payload */
    char payload[256];
    int len;
    if (dev_name[0] != '\0') {
        len = snprintf(payload, sizeof(payload),
                       "{\"pairing_code\":\"%s\",\"fw_version\":\"%s\",\"mac_address\":\"%s\",\"device_name\":\"%s\"}",
                       s_pairing_code,
                       pulsync_version_get(),
                       mac_str, dev_name);
    } else {
        len = snprintf(payload, sizeof(payload),
                       "{\"pairing_code\":\"%s\",\"fw_version\":\"%s\",\"mac_address\":\"%s\"}",
                       s_pairing_code,
                       pulsync_version_get(),
                       mac_str);
    }

    ESP_LOGI(TAG, "Enrolling: code=%s mac=%s", s_pairing_code, mac_str);
    s_state = PULSYNC_ENROLL_IN_PROGRESS;

    /* Send enrollment via HTTP POST */
    char response[256] = {0};
    if (!pulsync_transport_http_post("/api/device/enroll", payload, (size_t)len, response, sizeof(response))) {
        ESP_LOGE(TAG, "Enrollment HTTP request failed");
        s_state = PULSYNC_ENROLL_FAILED;
        if (s_cb) s_cb(false, NULL);
        return;
    }

    /* Process response directly */
    pulsync_enroll_handle_response(response, strlen(response));
}

void pulsync_enroll_handle_response(const char *payload, size_t len) {
    if (!payload || len == 0) {
        ESP_LOGE(TAG, "Empty enrollment response");
        s_state = PULSYNC_ENROLL_FAILED;
        if (s_cb) s_cb(false, NULL);
        return;
    }

    /* Check for error field */
    char error[64] = {0};
    if (json_extract_string(payload, len, "error", error, sizeof(error)) > 0) {
        ESP_LOGE(TAG, "Enrollment error: %s", error);
        s_state = PULSYNC_ENROLL_FAILED;
        if (s_cb) s_cb(false, NULL);
        return;
    }

    /* Extract token */
    char token[PULSYNC_TOKEN_MAXLEN] = {0};
    if (json_extract_string(payload, len, "token", token, sizeof(token)) == 0) {
        ESP_LOGE(TAG, "No token in enrollment response");
        s_state = PULSYNC_ENROLL_FAILED;
        if (s_cb) s_cb(false, NULL);
        return;
    }

    /* Store token in NVS */
    if (!pulsync_nvs_set_str(PULSYNC_NVS_KEY_TOKEN, token)) {
        ESP_LOGE(TAG, "Failed to store token in NVS");
        s_state = PULSYNC_ENROLL_FAILED;
        if (s_cb) s_cb(false, NULL);
        return;
    }

    /* Update local state and transport */
    strncpy(s_device_token, token, PULSYNC_TOKEN_MAXLEN - 1);
    pulsync_transport_set_token(token);

    /* The server may hand back the MQTT broker endpoint so the device connects
     * to the right place instead of deriving it from the HTTP host. Persist it
     * and point the transport at it. Optional — absent for older/local servers
     * that don't advertise it, in which case we keep deriving. */
    char mqtt_url[128] = {0};
    if (json_extract_string(payload, len, "mqtt_url", mqtt_url, sizeof(mqtt_url)) > 0) {
        pulsync_nvs_set_str(PULSYNC_NVS_KEY_MQTT_URL, mqtt_url);
        pulsync_transport_set_mqtt_url(mqtt_url);
        ESP_LOGI(TAG, "MQTT endpoint from enroll: %s", mqtt_url);
    }

    /* The server issues a config-portal password when the device enrolls and
     * sends only its SHA-256 hash. Persist it so the config menu can gate
     * itself offline (SHA256(entered) == stored hash). Plaintext never touches
     * the device. */
    char cfg_pw_hash[72] = {0};
    if (json_extract_string(payload, len, "config_pw_hash", cfg_pw_hash, sizeof(cfg_pw_hash)) > 0) {
        pulsync_nvs_set_str(PULSYNC_NVS_KEY_CFG_PW_HASH, cfg_pw_hash);
        ESP_LOGI(TAG, "Config-portal password hash stored from enroll");
    }

    s_state = PULSYNC_ENROLL_DONE;
    ESP_LOGI(TAG, "Enrollment successful");

    if (s_cb) s_cb(true, token);
}

pulsync_enroll_state_t pulsync_enroll_get_state(void) {
    return s_state;
}

bool pulsync_enroll_get_token(char *out, size_t maxlen) {
    if (s_state != PULSYNC_ENROLL_DONE || s_device_token[0] == '\0') {
        return false;
    }
    strncpy(out, s_device_token, maxlen - 1);
    out[maxlen - 1] = '\0';
    return true;
}

void pulsync_enroll_on_complete(pulsync_enroll_cb_t cb) {
    s_cb = cb;
}

void pulsync_enroll_reset(void) {
    ESP_LOGW(TAG, "Resetting enrollment (erasing token)");
    pulsync_nvs_erase_key(PULSYNC_NVS_KEY_TOKEN);
    s_device_token[0] = '\0';
    s_state = PULSYNC_ENROLL_NEEDED;
}
