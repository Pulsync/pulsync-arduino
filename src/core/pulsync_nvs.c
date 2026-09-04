/**
 * pulsync_nvs — NVS management implementation
 *
 * Uses ESP-IDF NVS APIs directly (available in both ESP-IDF and Arduino-ESP32).
 * Arduino-ESP32 ships the full ESP-IDF NVS component.
 */

#include "pulsync_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "pulsync_nvs";

static nvs_handle_t s_nvs_handle = 0;
static bool s_initialized = false;

bool pulsync_nvs_init(void) {
    if (s_initialized) return true;

    esp_err_t err = nvs_flash_init();

    /* If NVS partition is corrupted or truncated, erase and retry */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrupted, erasing and re-initializing");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
            return false;
        }
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Open our namespace */
    err = nvs_open(PULSYNC_NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open namespace '%s' failed: %s",
                 PULSYNC_NVS_NAMESPACE, esp_err_to_name(err));
        return false;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "NVS initialized (namespace: %s)", PULSYNC_NVS_NAMESPACE);
    return true;
}

bool pulsync_nvs_get_str(const char *key, char *out, size_t maxlen) {
    if (!s_initialized || !key || !out || maxlen == 0) return false;

    size_t required = maxlen;
    esp_err_t err = nvs_get_str(s_nvs_handle, key, out, &required);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS get_str '%s' failed: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool pulsync_nvs_set_str(const char *key, const char *value) {
    if (!s_initialized || !key || !value) return false;

    esp_err_t err = nvs_set_str(s_nvs_handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set_str '%s' failed: %s", key, esp_err_to_name(err));
        return false;
    }

    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool pulsync_nvs_get_i32(const char *key, int32_t *out) {
    if (!s_initialized || !key || !out) return false;

    esp_err_t err = nvs_get_i32(s_nvs_handle, key, out);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS get_i32 '%s' failed: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool pulsync_nvs_set_i32(const char *key, int32_t value) {
    if (!s_initialized || !key) return false;

    esp_err_t err = nvs_set_i32(s_nvs_handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set_i32 '%s' failed: %s", key, esp_err_to_name(err));
        return false;
    }

    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool pulsync_nvs_erase_key(const char *key) {
    if (!s_initialized || !key) return false;

    esp_err_t err = nvs_erase_key(s_nvs_handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;  /* already gone */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS erase_key '%s' failed: %s", key, esp_err_to_name(err));
        return false;
    }

    nvs_commit(s_nvs_handle);
    return true;
}

bool pulsync_nvs_erase_all(void) {
    if (!s_initialized) return false;

    esp_err_t err = nvs_erase_all(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS erase_all failed: %s", esp_err_to_name(err));
        return false;
    }

    nvs_commit(s_nvs_handle);
    ESP_LOGW(TAG, "All NVS data erased (factory reset)");
    return true;
}

bool pulsync_nvs_is_ready(void) {
    return s_initialized;
}
