/**
 * pulsync_config — Remote config implementation
 *
 * Stores registered keys with their type, callback, and current value.
 * Values persisted to NVS with "cfg_" prefix on key.
 * On push from server, compares new value to current, fires callback on change.
 */

#include "pulsync_config.h"
#include "pulsync_nvs.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "pulsync_config";

/* ---------- Internal types ---------- */

typedef struct {
    char key[PULSYNC_CONFIG_KEY_MAXLEN];
    pulsync_config_type_t type;
    pulsync_config_value_t value;
    bool has_value;  /* true if value has been set (from NVS or server) */

    /* Type-specific callbacks */
    union {
        pulsync_config_cb_int_t   cb_int;
        pulsync_config_cb_float_t cb_float;
        pulsync_config_cb_bool_t  cb_bool;
        pulsync_config_cb_str_t   cb_str;
    };
    bool has_callback;
} config_entry_t;

/* ---------- State ---------- */

static config_entry_t s_entries[PULSYNC_CONFIG_MAX_ENTRIES];
static int s_entry_count = 0;
static pulsync_config_cb_t s_generic_cb = NULL;

/* ---------- Internal helpers ---------- */

static config_entry_t *find_entry(const char *key) {
    for (int i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static config_entry_t *alloc_entry(const char *key) {
    config_entry_t *e = find_entry(key);
    if (e) return e;

    if (s_entry_count >= PULSYNC_CONFIG_MAX_ENTRIES) {
        ESP_LOGW(TAG, "Config entries full, can't add '%s'", key);
        return NULL;
    }

    e = &s_entries[s_entry_count++];
    memset(e, 0, sizeof(config_entry_t));
    strncpy(e->key, key, PULSYNC_CONFIG_KEY_MAXLEN - 1);
    return e;
}

/** NVS key for config values: "cfg_" + key (truncated to 15 chars for NVS limit) */
static void make_nvs_key(const char *key, char *nvs_key, size_t maxlen) {
    snprintf(nvs_key, maxlen, "cfg_%.11s", key);  /* 4 + 11 = 15 max NVS key length */
}

static void load_from_nvs(config_entry_t *e) {
    char nvs_key[16];
    make_nvs_key(e->key, nvs_key, sizeof(nvs_key));

    switch (e->type) {
        case PULSYNC_CONFIG_TYPE_INT: {
            int32_t val;
            if (pulsync_nvs_get_i32(nvs_key, &val)) {
                e->value.i = val;
                e->has_value = true;
            }
            break;
        }
        case PULSYNC_CONFIG_TYPE_FLOAT: {
            /* Store float as int32 (multiply by 1000) for NVS compatibility */
            int32_t raw;
            if (pulsync_nvs_get_i32(nvs_key, &raw)) {
                e->value.f = (float)raw / 1000.0f;
                e->has_value = true;
            }
            break;
        }
        case PULSYNC_CONFIG_TYPE_BOOL: {
            int32_t val;
            if (pulsync_nvs_get_i32(nvs_key, &val)) {
                e->value.b = (val != 0);
                e->has_value = true;
            }
            break;
        }
        case PULSYNC_CONFIG_TYPE_STRING: {
            if (pulsync_nvs_get_str(nvs_key, e->value.s, PULSYNC_CONFIG_VAL_MAXLEN)) {
                e->has_value = true;
            }
            break;
        }
    }
}

static void save_to_nvs(config_entry_t *e) {
    char nvs_key[16];
    make_nvs_key(e->key, nvs_key, sizeof(nvs_key));

    switch (e->type) {
        case PULSYNC_CONFIG_TYPE_INT:
            pulsync_nvs_set_i32(nvs_key, e->value.i);
            break;
        case PULSYNC_CONFIG_TYPE_FLOAT:
            pulsync_nvs_set_i32(nvs_key, (int32_t)(e->value.f * 1000.0f));
            break;
        case PULSYNC_CONFIG_TYPE_BOOL:
            pulsync_nvs_set_i32(nvs_key, e->value.b ? 1 : 0);
            break;
        case PULSYNC_CONFIG_TYPE_STRING:
            pulsync_nvs_set_str(nvs_key, e->value.s);
            break;
    }
}

static void fire_callback(config_entry_t *e) {
    if (!e->has_callback) return;

    switch (e->type) {
        case PULSYNC_CONFIG_TYPE_INT:
            if (e->cb_int) e->cb_int(e->value.i);
            break;
        case PULSYNC_CONFIG_TYPE_FLOAT:
            if (e->cb_float) e->cb_float(e->value.f);
            break;
        case PULSYNC_CONFIG_TYPE_BOOL:
            if (e->cb_bool) e->cb_bool(e->value.b);
            break;
        case PULSYNC_CONFIG_TYPE_STRING:
            if (e->cb_str) e->cb_str(e->value.s);
            break;
    }
}

/* ---------- JSON config parser ---------- */

/**
 * Parse flat config JSON: {"key1":"val1","key2":"val2",...}
 * For each key that matches a registered entry, update and fire callback.
 */
static void parse_config_json(const char *json, size_t len) {
    const char *pos = json;
    const char *end = json + len;

    /* Skip to first { */
    while (pos < end && *pos != '{') pos++;
    if (pos >= end) return;
    pos++;  /* skip { */

    while (pos < end) {
        /* Skip whitespace and commas */
        while (pos < end && (*pos == ' ' || *pos == ',' || *pos == '\n' || *pos == '\r' || *pos == '\t')) pos++;
        if (pos >= end || *pos == '}') break;

        /* Parse key */
        if (*pos != '"') break;
        pos++;
        const char *key_start = pos;
        while (pos < end && *pos != '"') pos++;
        size_t key_len = (size_t)(pos - key_start);
        if (pos >= end) break;
        pos++;  /* skip closing " */

        char key[PULSYNC_CONFIG_KEY_MAXLEN] = {0};
        if (key_len >= PULSYNC_CONFIG_KEY_MAXLEN) key_len = PULSYNC_CONFIG_KEY_MAXLEN - 1;
        memcpy(key, key_start, key_len);

        /* Skip colon */
        while (pos < end && (*pos == ':' || *pos == ' ')) pos++;

        /* Parse value (string, number, bool) */
        char val_str[PULSYNC_CONFIG_VAL_MAXLEN] = {0};

        if (*pos == '"') {
            /* String value */
            pos++;
            const char *val_start = pos;
            while (pos < end && *pos != '"') pos++;
            size_t val_len = (size_t)(pos - val_start);
            if (val_len >= PULSYNC_CONFIG_VAL_MAXLEN) val_len = PULSYNC_CONFIG_VAL_MAXLEN - 1;
            memcpy(val_str, val_start, val_len);
            if (pos < end) pos++;  /* skip closing " */
        } else {
            /* Number or bool */
            const char *val_start = pos;
            while (pos < end && *pos != ',' && *pos != '}' && *pos != ' ') pos++;
            size_t val_len = (size_t)(pos - val_start);
            if (val_len >= PULSYNC_CONFIG_VAL_MAXLEN) val_len = PULSYNC_CONFIG_VAL_MAXLEN - 1;
            memcpy(val_str, val_start, val_len);
        }

        /* Find matching entry and update */
        config_entry_t *e = find_entry(key);
        if (e) {
            bool changed = false;

            switch (e->type) {
                case PULSYNC_CONFIG_TYPE_INT: {
                    int32_t new_val = (int32_t)atoi(val_str);
                    if (!e->has_value || e->value.i != new_val) {
                        e->value.i = new_val;
                        changed = true;
                    }
                    break;
                }
                case PULSYNC_CONFIG_TYPE_FLOAT: {
                    float new_val = (float)atof(val_str);
                    if (!e->has_value || e->value.f != new_val) {
                        e->value.f = new_val;
                        changed = true;
                    }
                    break;
                }
                case PULSYNC_CONFIG_TYPE_BOOL: {
                    bool new_val = (strcmp(val_str, "true") == 0 || strcmp(val_str, "1") == 0);
                    if (!e->has_value || e->value.b != new_val) {
                        e->value.b = new_val;
                        changed = true;
                    }
                    break;
                }
                case PULSYNC_CONFIG_TYPE_STRING: {
                    if (!e->has_value || strcmp(e->value.s, val_str) != 0) {
                        strncpy(e->value.s, val_str, PULSYNC_CONFIG_VAL_MAXLEN - 1);
                        changed = true;
                    }
                    break;
                }
            }

            if (changed) {
                e->has_value = true;
                save_to_nvs(e);
                fire_callback(e);
                ESP_LOGI(TAG, "Config '%s' updated: %s", key, val_str);

                if (s_generic_cb) {
                    s_generic_cb(key, val_str);
                }
            }
        } else {
            ESP_LOGD(TAG, "Config key '%s' not registered, ignoring", key);
        }
    }
}

/* ---------- Public API ---------- */

void pulsync_config_init(void) {
    memset(s_entries, 0, sizeof(s_entries));
    s_entry_count = 0;
    ESP_LOGI(TAG, "Config module initialized");
}

void pulsync_config_on_int(const char *key, pulsync_config_cb_int_t cb, int32_t default_val) {
    config_entry_t *e = alloc_entry(key);
    if (!e) return;

    e->type = PULSYNC_CONFIG_TYPE_INT;
    e->value.i = default_val;
    e->cb_int = cb;
    e->has_callback = (cb != NULL);

    /* Try to load from NVS (overrides default if present) */
    load_from_nvs(e);
    if (!e->has_value) {
        e->value.i = default_val;
        e->has_value = true;
    }
}

void pulsync_config_on_float(const char *key, pulsync_config_cb_float_t cb, float default_val) {
    config_entry_t *e = alloc_entry(key);
    if (!e) return;

    e->type = PULSYNC_CONFIG_TYPE_FLOAT;
    e->value.f = default_val;
    e->cb_float = cb;
    e->has_callback = (cb != NULL);

    load_from_nvs(e);
    if (!e->has_value) {
        e->value.f = default_val;
        e->has_value = true;
    }
}

void pulsync_config_on_bool(const char *key, pulsync_config_cb_bool_t cb, bool default_val) {
    config_entry_t *e = alloc_entry(key);
    if (!e) return;

    e->type = PULSYNC_CONFIG_TYPE_BOOL;
    e->value.b = default_val;
    e->cb_bool = cb;
    e->has_callback = (cb != NULL);

    load_from_nvs(e);
    if (!e->has_value) {
        e->value.b = default_val;
        e->has_value = true;
    }
}

void pulsync_config_on_string(const char *key, pulsync_config_cb_str_t cb, const char *default_val) {
    config_entry_t *e = alloc_entry(key);
    if (!e) return;

    e->type = PULSYNC_CONFIG_TYPE_STRING;
    if (default_val) {
        strncpy(e->value.s, default_val, PULSYNC_CONFIG_VAL_MAXLEN - 1);
    }
    e->cb_str = cb;
    e->has_callback = (cb != NULL);

    load_from_nvs(e);
    if (!e->has_value && default_val) {
        strncpy(e->value.s, default_val, PULSYNC_CONFIG_VAL_MAXLEN - 1);
        e->has_value = true;
    }
}

int32_t pulsync_config_get_int(const char *key, int32_t default_val) {
    config_entry_t *e = find_entry(key);
    if (e && e->has_value) return e->value.i;
    return default_val;
}

float pulsync_config_get_float(const char *key, float default_val) {
    config_entry_t *e = find_entry(key);
    if (e && e->has_value) return e->value.f;
    return default_val;
}

bool pulsync_config_get_bool(const char *key, bool default_val) {
    config_entry_t *e = find_entry(key);
    if (e && e->has_value) return e->value.b;
    return default_val;
}

bool pulsync_config_get_string(const char *key, char *out, size_t maxlen, const char *default_val) {
    config_entry_t *e = find_entry(key);
    if (e && e->has_value) {
        strncpy(out, e->value.s, maxlen - 1);
        out[maxlen - 1] = '\0';
        return true;
    }
    if (default_val) {
        strncpy(out, default_val, maxlen - 1);
        out[maxlen - 1] = '\0';
    }
    return false;
}

void pulsync_config_handle_push(const char *json, size_t json_len) {
    if (!json || json_len == 0) return;
    ESP_LOGD(TAG, "Processing config push (%u bytes)", (unsigned)json_len);
    parse_config_json(json, json_len);
}

void pulsync_config_on_change(pulsync_config_cb_t cb) {
    s_generic_cb = cb;
}
