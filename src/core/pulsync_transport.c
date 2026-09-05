/**
 * pulsync_transport — MQTT + HTTP transport implementation
 *
 * MQTT: persistent connection via esp_mqtt_client (handles reconnect internally)
 * HTTP: on-demand via reusable esp_http_client (enrollment + OTA)
 * Offline queue: ring buffer stores messages when MQTT is down
 */

#include "pulsync_transport.h"
#include "pulsync_wifi.h"
#include "pulsync_scheduler.h"

#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "mqtt_client.h"
#include "mdns.h"
#include "esp_netif.h"

/* Arduino-ESP32 cert bundle */
#ifdef ARDUINO
esp_err_t arduino_esp_crt_bundle_attach(void *conf);
#define PULSYNC_CRT_BUNDLE_ATTACH arduino_esp_crt_bundle_attach
#else
#include "esp_crt_bundle.h"
#define PULSYNC_CRT_BUNDLE_ATTACH esp_crt_bundle_attach
#endif

static const char *TAG = "pulsync_transport";

/* ---------- State ---------- */

static pulsync_transport_config_t s_config;
/* When the configured server host is an mDNS name ("<label>.local"), we keep
 * the bare label here and resolve it to an IP at connect time (see
 * resolve_local_host). s_config.server_host then holds the resolved dotted IP
 * that MQTT/HTTP actually dial. Empty when the host is a plain IP/domain. */
static char s_mdns_label[64] = {0};
static bool s_mdns_inited = false;

/* ---- MQTT target (may differ from the HTTP server host) ----
 * By default the MQTT broker is assumed to be the same host as HTTP (with the
 * TLS-derived port). The server can override this via the enroll response /
 * heartbeat (pulsync_transport_set_mqtt_url), pointing the device at a broker
 * that lives elsewhere. s_mqtt_host holds the dial target (resolved IP if the
 * configured host was ".local"); s_mqtt_mdns_label holds the bare mDNS label
 * when applicable; s_mqtt_override is true once the server has set it. */
static char s_mqtt_host[64] = {0};
static uint16_t s_mqtt_port = 0;
static bool s_mqtt_tls = false;
static char s_mqtt_mdns_label[64] = {0};
static bool s_mqtt_override = false;
static pulsync_transport_state_t s_state = PULSYNC_TRANSPORT_DISCONNECTED;
static pulsync_transport_rx_cb_t s_rx_cb = NULL;
static pulsync_transport_state_cb_t s_state_cb = NULL;
static SemaphoreHandle_t s_mutex = NULL;

/* MQTT client */
static esp_mqtt_client_handle_t s_mqtt = NULL;
static bool s_mqtt_started = false;

/* HTTP client (reusable) */
static esp_http_client_handle_t s_http = NULL;
static int s_http_timeout_ms = 10000;

/* Offline queue */
typedef struct {
    char topic[32];
    char payload[256];
    uint16_t len;
    bool in_use;
} offline_msg_t;

static offline_msg_t s_queue[PULSYNC_OFFLINE_QUEUE_SIZE];
static int s_queue_head = 0;

/* HTTP response buffer */
static char s_http_resp[512];
static size_t s_http_resp_len = 0;

/* ---------- Internal helpers ---------- */

static void set_state(pulsync_transport_state_t new_state) {
    if (s_state == new_state) return;
    s_state = new_state;
    ESP_LOGI(TAG, "State → %s",
             new_state == PULSYNC_TRANSPORT_CONNECTED ? "CONNECTED" :
             new_state == PULSYNC_TRANSPORT_CONNECTING ? "CONNECTING" : "DISCONNECTED");
    if (s_state_cb) s_state_cb(new_state);
}

static void get_token(char *buf, size_t len) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(buf, s_config.device_token, len - 1);
    buf[len - 1] = '\0';
    if (s_mutex) xSemaphoreGive(s_mutex);
}

/* ---------- Offline queue ---------- */

static void queue_message(const char *subtopic, const char *payload, size_t len) {
    offline_msg_t *slot = &s_queue[s_queue_head];
    strncpy(slot->topic, subtopic, sizeof(slot->topic) - 1);
    size_t copy = len < sizeof(slot->payload) - 1 ? len : sizeof(slot->payload) - 1;
    memcpy(slot->payload, payload, copy);
    slot->payload[copy] = '\0';
    slot->len = (uint16_t)copy;
    slot->in_use = true;
    s_queue_head = (s_queue_head + 1) % PULSYNC_OFFLINE_QUEUE_SIZE;
}

static int queue_depth(void) {
    int n = 0;
    for (int i = 0; i < PULSYNC_OFFLINE_QUEUE_SIZE; i++) {
        if (s_queue[i].in_use) n++;
    }
    return n;
}

static void flush_queue(void) {
    char token[PULSYNC_TOKEN_MAXLEN];
    get_token(token, sizeof(token));
    if (token[0] == '\0') return;

    int sent = 0;
    for (int i = 0; i < PULSYNC_OFFLINE_QUEUE_SIZE; i++) {
        if (!s_queue[i].in_use) continue;

        char topic[160];
        snprintf(topic, sizeof(topic), "pulsync/%s/upload/%s", token, s_queue[i].topic);
        int msg_id = esp_mqtt_client_publish(s_mqtt, topic, s_queue[i].payload, s_queue[i].len, 1, 0);
        if (msg_id >= 0) {
            s_queue[i].in_use = false;
            sent++;
        }
    }
    if (sent > 0) {
        ESP_LOGI(TAG, "Flushed %d queued messages", sent);
    }
}

/* ---------- MQTT event handler ---------- */

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "MQTT connected");
            set_state(PULSYNC_TRANSPORT_CONNECTED);

            /* Subscribe to download topics */
            char token[PULSYNC_TOKEN_MAXLEN];
            get_token(token, sizeof(token));
            if (token[0] != '\0') {
                char topic[160];
                snprintf(topic, sizeof(topic), "pulsync/%s/download/+", token);
                esp_mqtt_client_subscribe(s_mqtt, topic, 1);
                ESP_LOGI(TAG, "Subscribed: %s", topic);
            }

            /* Flush offline queue */
            flush_queue();
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected (auto-reconnect active)");
            set_state(PULSYNC_TRANSPORT_DISCONNECTED);
            break;

        case MQTT_EVENT_DATA: {
            if (event->data_len > 0 && s_rx_cb) {
                /* Extract subtopic from full topic */
                char topic_buf[160];
                int tlen = event->topic_len < (int)sizeof(topic_buf) - 1 ? event->topic_len : (int)sizeof(topic_buf) - 1;
                memcpy(topic_buf, event->topic, tlen);
                topic_buf[tlen] = '\0';

                s_rx_cb(topic_buf, event->data, event->data_len);
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;

        default:
            break;
    }
}

/* ---------- HTTP event handler ---------- */

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0 && s_http_resp_len + evt->data_len < sizeof(s_http_resp) - 1) {
                memcpy(s_http_resp + s_http_resp_len, evt->data, evt->data_len);
                s_http_resp_len += evt->data_len;
                s_http_resp[s_http_resp_len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

/* ---------- URL parsing ---------- */

void pulsync_transport_parse_url(const char *url, char *host, size_t host_len,
                                  uint16_t *http_port, uint16_t *mqtt_port, bool *use_tls) {
    if (!url || !host) return;

    const char *p = url;
    bool explicit_tls = false;
    bool explicit_scheme = false;

    /* Strip scheme */
    if (strncmp(p, "https://", 8) == 0) { *use_tls = true; p += 8; explicit_scheme = true; }
    else if (strncmp(p, "http://", 7) == 0) { *use_tls = false; p += 7; explicit_scheme = true; }

    /* Extract host:port */
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');

    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= host_len) hlen = host_len - 1;
        strncpy(host, p, hlen);
        host[hlen] = '\0';
        *http_port = (uint16_t)atoi(colon + 1);
    } else {
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= host_len) hlen = host_len - 1;
        strncpy(host, p, hlen);
        host[hlen] = '\0';
        *http_port = 0;
    }

    /* Heuristic: IP or .local → no TLS */
    if (!explicit_scheme) {
        bool is_ip = isdigit((unsigned char)host[0]);
        bool is_local = (strstr(host, ".local") != NULL);
        *use_tls = !(is_ip || is_local);
    }

    /* Default ports */
    if (*http_port == 0) *http_port = *use_tls ? 443 : 3456;
    *mqtt_port = *use_tls ? 8883 : 1883;
}

/* ---------- Public API ---------- */

bool pulsync_transport_init(const pulsync_transport_config_t *config) {
    if (!config) return false;
    memcpy(&s_config, config, sizeof(pulsync_transport_config_t));

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return false;

    /* Clear offline queue */
    memset(s_queue, 0, sizeof(s_queue));

    /* If the server host is an mDNS name ("<label>.local"), remember the bare
     * label so we can resolve it to an IP once WiFi is up. lwIP's built-in
     * ".local" DNS path is unreliable here (getaddrinfo returns EAI_FAIL), so
     * we resolve explicitly via the mDNS component at connect time. */
    s_mdns_label[0] = '\0';
    {
        size_t hlen = strlen(s_config.server_host);
        const char suffix[] = ".local";
        size_t slen = sizeof(suffix) - 1;
        if (hlen > slen && strcasecmp(s_config.server_host + hlen - slen, suffix) == 0) {
            size_t label_len = hlen - slen;
            if (label_len >= sizeof(s_mdns_label)) label_len = sizeof(s_mdns_label) - 1;
            memcpy(s_mdns_label, s_config.server_host, label_len);
            s_mdns_label[label_len] = '\0';
            ESP_LOGI(TAG, "Server host is mDNS name '%s.local' — will resolve via mDNS", s_mdns_label);
        }
    }

    ESP_LOGI(TAG, "Init: %s:%u (MQTT:%u, TLS:%s)",
             s_config.server_host, s_config.http_port, s_config.mqtt_port,
             s_config.use_tls ? "yes" : "no");
    return true;
}

/* Resolve the configured mDNS ".local" host to a dotted IP, writing it into
 * s_config.server_host so MQTT/HTTP dial the IP directly. No-op when the host
 * isn't an mDNS name. Safe to call repeatedly (re-resolves each time, so a
 * server that moved to a new DHCP IP is picked up on reconnect). Returns true
 * if the host is usable (resolved, or not an mDNS name to begin with). */
static bool resolve_local_host(void) {
    if (s_mdns_label[0] == '\0') return true;  /* not an mDNS name */

    if (!s_mdns_inited) {
        esp_err_t ie = mdns_init();
        if (ie != ESP_OK) {
            ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(ie));
            return false;
        }
        s_mdns_inited = true;
    }

    esp_ip4_addr_t addr = {0};
    /* mDNS expects the bare label (no ".local"). */
    esp_err_t err = mdns_query_a(s_mdns_label, 3000, &addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS resolve of '%s.local' failed: %s", s_mdns_label, esp_err_to_name(err));
        return false;
    }

    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&addr));
    strncpy(s_config.server_host, ip_str, sizeof(s_config.server_host) - 1);
    s_config.server_host[sizeof(s_config.server_host) - 1] = '\0';
    ESP_LOGI(TAG, "Resolved %s.local → %s", s_mdns_label, s_config.server_host);
    return true;
}

/* Compute the effective MQTT dial target (host/port/tls). When the server has
 * not overridden it, fall back to the HTTP server host + TLS-derived port. The
 * host written to *out_host is the resolved IP when the target is an mDNS name.
 * Returns true if the target is usable (resolved or plain host). */
static bool resolve_mqtt_target(char *out_host, size_t out_host_len,
                                uint16_t *out_port, bool *out_tls) {
    if (!s_mqtt_override) {
        /* Derive from HTTP target: same host (already resolved by
         * resolve_local_host if it was .local), TLS-based MQTT port. */
        strncpy(out_host, s_config.server_host, out_host_len - 1);
        out_host[out_host_len - 1] = '\0';
        *out_port = s_config.mqtt_port;
        *out_tls = s_config.use_tls;
        return out_host[0] != '\0';
    }

    *out_port = s_mqtt_port;
    *out_tls = s_mqtt_tls;

    if (s_mqtt_mdns_label[0] != '\0') {
        /* Resolve the MQTT ".local" host via mDNS. */
        if (!s_mdns_inited) {
            if (mdns_init() != ESP_OK) return false;
            s_mdns_inited = true;
        }
        esp_ip4_addr_t addr = {0};
        esp_err_t err = mdns_query_a(s_mqtt_mdns_label, 3000, &addr);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mDNS resolve of MQTT host '%s.local' failed: %s",
                     s_mqtt_mdns_label, esp_err_to_name(err));
            return false;
        }
        snprintf(out_host, out_host_len, IPSTR, IP2STR(&addr));
        return true;
    }

    strncpy(out_host, s_mqtt_host, out_host_len - 1);
    out_host[out_host_len - 1] = '\0';
    return out_host[0] != '\0';
}

void pulsync_transport_start(void) {
    if (s_mqtt_started) return;

    /* Resolve an mDNS ".local" server host to an IP before dialing. If it
     * fails now (WiFi just came up, server not yet announced), we return
     * without starting; the caller's connect/retry loop will call us again. */
    if (!resolve_local_host()) {
        ESP_LOGW(TAG, "Server host not resolvable yet; will retry.");
        return;
    }

    /* Resolve the MQTT dial target (may be a server-provided broker distinct
     * from the HTTP host). If it's a .local name that isn't announced yet, bail
     * and let the retry loop call us again. */
    char mqtt_host[64] = {0};
    uint16_t mqtt_port = 0;
    bool mqtt_tls = false;
    if (!resolve_mqtt_target(mqtt_host, sizeof(mqtt_host), &mqtt_port, &mqtt_tls)) {
        ESP_LOGW(TAG, "MQTT target not resolvable yet; will retry.");
        return;
    }

    char token[PULSYNC_TOKEN_MAXLEN];
    get_token(token, sizeof(token));

    /* Build MQTT URI */
    char uri[128];
    snprintf(uri, sizeof(uri), "%s://%s:%u",
             mqtt_tls ? "mqtts" : "mqtt", mqtt_host, mqtt_port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = uri,
        .username = "device",
        .password = token,
        .keepalive = 30,
        .disable_auto_reconnect = false,  /* esp_mqtt handles reconnect */
    };

    if (mqtt_tls) {
        mqtt_cfg.crt_bundle_attach = PULSYNC_CRT_BUNDLE_ATTACH;
    }

    s_mqtt = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return;
    }

    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    esp_err_t err = esp_mqtt_client_start(s_mqtt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
        return;
    }

    s_mqtt_started = true;
    set_state(PULSYNC_TRANSPORT_CONNECTING);
    ESP_LOGI(TAG, "MQTT connecting to %s", uri);
}

void pulsync_transport_stop(void) {
    if (s_mqtt) {
        esp_mqtt_client_stop(s_mqtt);
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
    }
    s_mqtt_started = false;
    set_state(PULSYNC_TRANSPORT_DISCONNECTED);
}

bool pulsync_transport_publish(const char *subtopic, const char *payload, size_t len) {
    if (!subtopic || !payload) return false;

    /* If not connected, queue for later */
    if (s_state != PULSYNC_TRANSPORT_CONNECTED || !s_mqtt) {
        queue_message(subtopic, payload, len);
        return true;  /* queued successfully */
    }

    char token[PULSYNC_TOKEN_MAXLEN];
    get_token(token, sizeof(token));
    if (token[0] == '\0') {
        queue_message(subtopic, payload, len);
        return true;
    }

    char topic[160];
    snprintf(topic, sizeof(topic), "pulsync/%s/upload/%s", token, subtopic);

    int msg_id = esp_mqtt_client_publish(s_mqtt, topic, payload, (int)len, 1, 0);
    if (msg_id < 0) {
        /* Publish failed — queue it */
        queue_message(subtopic, payload, len);
        ESP_LOGW(TAG, "Publish failed, queued");
    }
    return true;
}

bool pulsync_transport_is_connected(void) {
    return s_state == PULSYNC_TRANSPORT_CONNECTED;
}

pulsync_transport_state_t pulsync_transport_get_state(void) {
    return s_state;
}

int pulsync_transport_queue_depth(void) {
    return queue_depth();
}

int pulsync_transport_queue_capacity(void) {
    return PULSYNC_OFFLINE_QUEUE_SIZE;
}

/* ---------- HTTP ---------- */

bool pulsync_transport_http_post(const char *path, const char *payload, size_t len,
                                  char *response, size_t resp_max) {
    /* Resolve an mDNS ".local" host to an IP first (enrollment/OTA may run
     * before the MQTT path has resolved it). No-op for plain IP/domain hosts. */
    if (!resolve_local_host()) {
        ESP_LOGW(TAG, "HTTP: server host not resolvable; aborting request.");
        return false;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s://%s:%u%s",
             s_config.use_tls ? "https" : "http",
             s_config.server_host, s_config.http_port, path);

    char token[PULSYNC_TOKEN_MAXLEN];
    get_token(token, sizeof(token));

    /* Clear response buffer */
    s_http_resp_len = 0;
    s_http_resp[0] = '\0';

    /* Create or reuse HTTP client */
    if (!s_http) {
        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = s_http_timeout_ms,
            .event_handler = http_event_handler,
            .keep_alive_enable = true,
            .crt_bundle_attach = s_config.use_tls ? PULSYNC_CRT_BUNDLE_ATTACH : NULL,
        };
        s_http = esp_http_client_init(&cfg);
        if (!s_http) return false;
    } else {
        /* Reused client: keep its timeout in sync with the configured value. */
        esp_http_client_set_timeout_ms(s_http, s_http_timeout_ms);
    }

    esp_http_client_set_url(s_http, url);
    esp_http_client_set_method(s_http, HTTP_METHOD_POST);
    esp_http_client_set_header(s_http, "Content-Type", "application/json");
    if (token[0] != '\0') {
        esp_http_client_set_header(s_http, "X-Device-Token", token);
    }
    esp_http_client_set_post_field(s_http, payload, (int)len);

    esp_err_t err = esp_http_client_perform(s_http);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP failed: %s", esp_err_to_name(err));
        /* Destroy and recreate on next call */
        esp_http_client_cleanup(s_http);
        s_http = NULL;
        return false;
    }

    int status = esp_http_client_get_status_code(s_http);
    if (status >= 200 && status < 300) {
        /* Copy response */
        if (response && resp_max > 0 && s_http_resp_len > 0) {
            size_t copy = s_http_resp_len < resp_max - 1 ? s_http_resp_len : resp_max - 1;
            memcpy(response, s_http_resp, copy);
            response[copy] = '\0';
        }
        return true;
    }

    ESP_LOGW(TAG, "HTTP %d from %s", status, path);
    return false;
}

/* ---------- Callbacks ---------- */

void pulsync_transport_on_receive(pulsync_transport_rx_cb_t cb) {
    s_rx_cb = cb;
}

void pulsync_transport_on_state_change(pulsync_transport_state_cb_t cb) {
    s_state_cb = cb;
}

/* ---------- Token / Config ---------- */

void pulsync_transport_set_token(const char *token) {
    if (!token || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_config.device_token, token, PULSYNC_TOKEN_MAXLEN - 1);
    s_config.device_token[PULSYNC_TOKEN_MAXLEN - 1] = '\0';
    xSemaphoreGive(s_mutex);
}

void pulsync_transport_set_server(const char *host, uint16_t http_port, uint16_t mqtt_port, bool use_tls) {
    if (!host || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool tls_changed = (s_config.use_tls != use_tls);
    bool host_changed = (strcmp(s_config.server_host, host) != 0);
    strncpy(s_config.server_host, host, sizeof(s_config.server_host) - 1);
    s_config.server_host[sizeof(s_config.server_host) - 1] = '\0';
    s_config.http_port = http_port;
    s_config.mqtt_port = mqtt_port;
    s_config.use_tls = use_tls;

    /* The cached HTTP client bakes in the TLS cert-bundle attach at init, so a
     * switch between an https (cloud) and http (local) target — as happens
     * during server discovery — needs a fresh client. Also re-detect an mDNS
     * ".local" host so resolution targets the new host. */
    if (tls_changed && s_http) {
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    if (host_changed) {
        s_mdns_label[0] = '\0';
        size_t hlen = strlen(s_config.server_host);
        const char suffix[] = ".local";
        size_t slen = sizeof(suffix) - 1;
        if (hlen > slen && strcasecmp(s_config.server_host + hlen - slen, suffix) == 0) {
            size_t label_len = hlen - slen;
            if (label_len >= sizeof(s_mdns_label)) label_len = sizeof(s_mdns_label) - 1;
            memcpy(s_mdns_label, s_config.server_host, label_len);
            s_mdns_label[label_len] = '\0';
        }
    }
    xSemaphoreGive(s_mutex);
}

void pulsync_transport_set_http_timeout(int timeout_ms) {
    if (timeout_ms < 500) timeout_ms = 500;
    s_http_timeout_ms = timeout_ms;
}

/* Parse "mqtt(s)://host:port" into host/port/tls. Missing scheme → mqtt (no
 * TLS); missing port → 8883 if tls else 1883. Returns false on empty input. */
static bool parse_mqtt_url(const char *url, char *host, size_t host_len,
                           uint16_t *port, bool *tls) {
    if (!url || url[0] == '\0') return false;

    bool use_tls = false;
    const char *h = url;
    if (strncmp(url, "mqtts://", 8) == 0) { use_tls = true;  h = url + 8; }
    else if (strncmp(url, "mqtt://", 7) == 0) { use_tls = false; h = url + 7; }
    else if (strncmp(url, "ssl://", 6) == 0) { use_tls = true;  h = url + 6; }
    else if (strncmp(url, "tcp://", 6) == 0) { use_tls = false; h = url + 6; }

    /* Copy host up to ':' or '/' */
    size_t i = 0;
    while (h[i] && h[i] != ':' && h[i] != '/' && i < host_len - 1) {
        host[i] = h[i];
        i++;
    }
    host[i] = '\0';
    if (host[0] == '\0') return false;

    uint16_t p = use_tls ? 8883 : 1883;
    const char *colon = strchr(h, ':');
    if (colon) {
        int parsed = atoi(colon + 1);
        if (parsed > 0 && parsed <= 65535) p = (uint16_t)parsed;
    }

    *port = p;
    *tls = use_tls;
    return true;
}

bool pulsync_transport_set_mqtt_url(const char *url) {
    /* Snapshot current effective target for change detection. */
    char prev[128] = {0};
    pulsync_transport_get_mqtt_url(prev, sizeof(prev));

    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!url || url[0] == '\0') {
        /* Clear override → fall back to deriving from HTTP host. */
        s_mqtt_override = false;
        s_mqtt_host[0] = '\0';
        s_mqtt_mdns_label[0] = '\0';
        s_mqtt_port = 0;
        s_mqtt_tls = false;
        xSemaphoreGive(s_mutex);
    } else {
        char host[64] = {0};
        uint16_t port = 0;
        bool tls = false;
        if (!parse_mqtt_url(url, host, sizeof(host), &port, &tls)) {
            xSemaphoreGive(s_mutex);
            return false;
        }
        s_mqtt_override = true;
        s_mqtt_port = port;
        s_mqtt_tls = tls;
        /* Detect an mDNS ".local" broker host. */
        s_mqtt_mdns_label[0] = '\0';
        size_t hlen = strlen(host);
        const char suffix[] = ".local";
        size_t slen = sizeof(suffix) - 1;
        if (hlen > slen && strcasecmp(host + hlen - slen, suffix) == 0) {
            size_t label_len = hlen - slen;
            if (label_len >= sizeof(s_mqtt_mdns_label)) label_len = sizeof(s_mqtt_mdns_label) - 1;
            memcpy(s_mqtt_mdns_label, host, label_len);
            s_mqtt_mdns_label[label_len] = '\0';
            s_mqtt_host[0] = '\0';
        } else {
            strncpy(s_mqtt_host, host, sizeof(s_mqtt_host) - 1);
            s_mqtt_host[sizeof(s_mqtt_host) - 1] = '\0';
        }
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "MQTT target set: %s:%u (TLS:%s)%s",
                 s_mqtt_mdns_label[0] ? s_mqtt_mdns_label : s_mqtt_host, port,
                 tls ? "yes" : "no", s_mqtt_mdns_label[0] ? " [mDNS]" : "");
    }

    /* Report whether the configured target changed (compare pre-resolution
     * scheme/host/port strings, not resolved IPs). */
    char now[128] = {0};
    pulsync_transport_get_mqtt_url(now, sizeof(now));
    return strcmp(prev, now) != 0;
}

void pulsync_transport_get_mqtt_url(char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (s_mqtt_override) {
        const char *host = s_mqtt_mdns_label[0] ? s_mqtt_mdns_label : s_mqtt_host;
        char full_host[80];
        if (s_mqtt_mdns_label[0])
            snprintf(full_host, sizeof(full_host), "%s.local", host);
        else
            snprintf(full_host, sizeof(full_host), "%s", host);
        snprintf(out, out_len, "%s://%s:%u",
                 s_mqtt_tls ? "mqtts" : "mqtt", full_host, s_mqtt_port);
    } else {
        /* Derived target (pre-resolution host). */
        snprintf(out, out_len, "%s://%s:%u",
                 s_config.use_tls ? "mqtts" : "mqtt",
                 s_config.server_host, s_config.mqtt_port);
    }
}
