/**
 * pulsync_wifi — WiFi manager implementation
 *
 * Uses ESP-IDF WiFi driver directly. Works on both ESP-IDF and Arduino-ESP32
 * since Arduino-ESP32 exposes the full ESP-IDF WiFi API.
 */

#include "pulsync_wifi.h"
#include "pulsync_scheduler.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG = "pulsync_wifi";

/* ---------- Configuration ---------- */

#define BACKOFF_INITIAL_MS   1000
#define BACKOFF_MAX_MS       60000
#define BACKOFF_MULTIPLIER   2
#define MAX_PROFILE_RETRIES  2   /* retries per profile before moving to next */

/* ---------- State ---------- */

static pulsync_wifi_profile_t s_profiles[PULSYNC_WIFI_MAX_PROFILES];
static int s_profile_count = 0;

static pulsync_wifi_state_t s_state = PULSYNC_WIFI_DISCONNECTED;
static pulsync_wifi_event_cb_t s_state_cb = NULL;
static pulsync_wifi_portal_cb_t s_portal_cb = NULL;

static int s_current_profile = 0;
static int s_profile_retries = 0;
static bool s_should_reconnect = false;
static uint32_t s_backoff_ms = BACKOFF_INITIAL_MS;
static uint32_t s_reconnect_at = 0;
static bool s_wifi_started = false;
static bool s_connecting = false;

static char s_ip_str[16] = {0};
static esp_netif_t *s_sta_netif = NULL;

/* ---------- Internal helpers ---------- */

static void set_state(pulsync_wifi_state_t new_state) {
    if (s_state == new_state) return;
    s_state = new_state;
    ESP_LOGI(TAG, "State → %d", (int)new_state);
    if (s_state_cb) s_state_cb(new_state);
}

/* Forward declarations */
static void schedule_reconnect(void);
static void try_connect_profile(int idx);

static void try_connect_profile(int idx) {
    if (idx < 0 || idx >= s_profile_count) return;

    pulsync_wifi_profile_t *p = &s_profiles[idx];
    if (!p->in_use) return;

    ESP_LOGI(TAG, "Connecting to '%s' (profile %d)", p->ssid, idx);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, p->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, p->password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = (p->password[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    s_connecting = true;
    set_state(PULSYNC_WIFI_CONNECTING);
}

static void try_next_profile(void) {
    s_profile_retries = 0;
    s_current_profile++;

    if (s_current_profile >= s_profile_count) {
        /* All profiles exhausted — reset to 0 and retry with longer backoff */
        ESP_LOGW(TAG, "All WiFi profiles failed, retrying from profile 0...");
        s_current_profile = 0;
        s_backoff_ms = BACKOFF_MAX_MS;  /* Use max backoff before retrying */
        schedule_reconnect();

        /* Also notify portal callback (portal can start alongside retry) */
        if (s_portal_cb) {
            s_portal_cb();
        }
        return;
    }

    try_connect_profile(s_current_profile);
}

static void schedule_reconnect(void) {
    s_reconnect_at = pulsync_millis() + s_backoff_ms;
    s_should_reconnect = true;
    ESP_LOGI(TAG, "Reconnect in %lu ms", (unsigned long)s_backoff_ms);

    /* Exponential backoff */
    s_backoff_ms *= BACKOFF_MULTIPLIER;
    if (s_backoff_ms > BACKOFF_MAX_MS) s_backoff_ms = BACKOFF_MAX_MS;
}

/* ---------- Event handler ---------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started");
                /* Connection is triggered by pulsync_wifi_start() — don't connect here */
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *evt =
                    (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "Disconnected (reason: %d)", evt->reason);

                s_connecting = false;
                s_ip_str[0] = '\0';

                if (s_state == PULSYNC_WIFI_CONNECTED) {
                    /* Was connected, lost connection — reconnect with backoff */
                    set_state(PULSYNC_WIFI_DISCONNECTED);
                    s_current_profile = 0;
                    schedule_reconnect();
                } else {
                    /* Never connected on this profile — try next or retry */
                    s_profile_retries++;
                    if (s_profile_retries >= MAX_PROFILE_RETRIES) {
                        try_next_profile();
                    } else {
                        schedule_reconnect();
                    }
                }
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "Connected, IP: %s", s_ip_str);

        s_connecting = false;
        s_should_reconnect = false;
        s_backoff_ms = BACKOFF_INITIAL_MS;
        set_state(PULSYNC_WIFI_CONNECTED);
    }
}

/* ---------- Public API ---------- */

bool pulsync_wifi_init(void) {
    if (s_sta_netif != NULL) return true;  /* already initialized */

    esp_err_t err;

    /* Initialize TCP/IP stack and event loop (safe to call multiple times) */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return false;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "Failed to create STA netif");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Register event handlers */
    esp_event_handler_instance_t wifi_inst;
    esp_event_handler_instance_t ip_inst;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &wifi_inst);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &ip_inst);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  /* We manage profiles ourselves */

    ESP_LOGI(TAG, "WiFi initialized");
    return true;
}

bool pulsync_wifi_add_profile(int slot, const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') return false;

    int idx;
    if (slot >= 0 && slot < PULSYNC_WIFI_MAX_PROFILES) {
        idx = slot;
    } else {
        /* Find next available slot */
        idx = -1;
        for (int i = 0; i < PULSYNC_WIFI_MAX_PROFILES; i++) {
            if (!s_profiles[i].in_use) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            ESP_LOGW(TAG, "All WiFi profile slots full");
            return false;
        }
    }

    strncpy(s_profiles[idx].ssid, ssid, PULSYNC_WIFI_SSID_MAXLEN - 1);
    s_profiles[idx].ssid[PULSYNC_WIFI_SSID_MAXLEN - 1] = '\0';

    if (password) {
        strncpy(s_profiles[idx].password, password, PULSYNC_WIFI_PASS_MAXLEN - 1);
        s_profiles[idx].password[PULSYNC_WIFI_PASS_MAXLEN - 1] = '\0';
    } else {
        s_profiles[idx].password[0] = '\0';
    }

    s_profiles[idx].in_use = true;

    if (idx >= s_profile_count) {
        s_profile_count = idx + 1;
    }

    ESP_LOGI(TAG, "Profile %d: '%s'", idx, s_profiles[idx].ssid);
    return true;
}

void pulsync_wifi_clear_profiles(void) {
    memset(s_profiles, 0, sizeof(s_profiles));
    s_profile_count = 0;
    s_current_profile = 0;
    s_profile_retries = 0;
    ESP_LOGI(TAG, "Cleared all WiFi profiles");
}

void pulsync_wifi_start(void) {
    /* Always ensure WiFi driver is started */
    if (!s_wifi_started) {
        esp_wifi_start();
        s_wifi_started = true;
    }

    if (s_profile_count == 0) {
        ESP_LOGW(TAG, "No WiFi profiles configured — requesting portal");
        if (s_portal_cb) s_portal_cb();
        return;
    }

    s_current_profile = 0;
    s_profile_retries = 0;
    s_backoff_ms = BACKOFF_INITIAL_MS;
    try_connect_profile(s_current_profile);
}

void pulsync_wifi_disconnect(void) {
    s_should_reconnect = false;
    esp_wifi_disconnect();
    set_state(PULSYNC_WIFI_DISCONNECTED);
}

pulsync_wifi_state_t pulsync_wifi_get_state(void) {
    return s_state;
}

bool pulsync_wifi_is_connected(void) {
    return s_state == PULSYNC_WIFI_CONNECTED;
}

const char *pulsync_wifi_get_ip(void) {
    return s_ip_str;
}

void pulsync_wifi_on_state_change(pulsync_wifi_event_cb_t cb) {
    s_state_cb = cb;
}

void pulsync_wifi_on_portal_needed(pulsync_wifi_portal_cb_t cb) {
    s_portal_cb = cb;
}

void pulsync_wifi_tick(void) {
    if (!s_should_reconnect) return;
    if (!s_wifi_started) return;

    uint32_t now = pulsync_millis();
    if ((int32_t)(now - s_reconnect_at) >= 0) {
        s_should_reconnect = false;
        ESP_LOGI(TAG, "Reconnect timer fired, attempting connection...");
        try_connect_profile(s_current_profile);
    }
}

uint32_t pulsync_wifi_get_backoff_ms(void) {
    return s_backoff_ms;
}
