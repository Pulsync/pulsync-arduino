/**
 * pulsync_config — Remote config store (C API)
 *
 * Generic key-value config pushed from server via heartbeat or WSS.
 * Values are stored in NVS for persistence across reboots.
 * User registers callbacks for specific keys — fired on change.
 *
 * Supports int, float, bool, and string value types.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PULSYNC_CONFIG_MAX_ENTRIES
#define PULSYNC_CONFIG_MAX_ENTRIES  16
#endif

#define PULSYNC_CONFIG_KEY_MAXLEN   24
#define PULSYNC_CONFIG_VAL_MAXLEN   64

/** Config value types */
typedef enum {
    PULSYNC_CONFIG_TYPE_INT = 0,
    PULSYNC_CONFIG_TYPE_FLOAT,
    PULSYNC_CONFIG_TYPE_BOOL,
    PULSYNC_CONFIG_TYPE_STRING
} pulsync_config_type_t;

/** Config value union */
typedef union {
    int32_t  i;
    float    f;
    bool     b;
    char     s[PULSYNC_CONFIG_VAL_MAXLEN];
} pulsync_config_value_t;

/** Callback typedefs for each value type */
typedef void (*pulsync_config_cb_int_t)(int32_t value);
typedef void (*pulsync_config_cb_float_t)(float value);
typedef void (*pulsync_config_cb_bool_t)(bool value);
typedef void (*pulsync_config_cb_str_t)(const char *value);

/** Generic config change callback (key + raw string value) */
typedef void (*pulsync_config_cb_t)(const char *key, const char *value);

/**
 * Initialize config module. Call once at startup.
 * Loads persisted config values from NVS.
 */
void pulsync_config_init(void);

/**
 * Register a callback for an int config key.
 * Callback fires when server pushes a new value for this key.
 * @param key         Config key name
 * @param cb          Callback function
 * @param default_val Default value if key not set by server
 */
void pulsync_config_on_int(const char *key, pulsync_config_cb_int_t cb, int32_t default_val);

/**
 * Register a callback for a float config key.
 */
void pulsync_config_on_float(const char *key, pulsync_config_cb_float_t cb, float default_val);

/**
 * Register a callback for a bool config key.
 */
void pulsync_config_on_bool(const char *key, pulsync_config_cb_bool_t cb, bool default_val);

/**
 * Register a callback for a string config key.
 */
void pulsync_config_on_string(const char *key, pulsync_config_cb_str_t cb, const char *default_val);

/**
 * Get current int config value.
 * @param key          Config key
 * @param default_val  Default if key doesn't exist
 * @return Current value (from NVS or default)
 */
int32_t pulsync_config_get_int(const char *key, int32_t default_val);

/**
 * Get current float config value.
 */
float pulsync_config_get_float(const char *key, float default_val);

/**
 * Get current bool config value.
 */
bool pulsync_config_get_bool(const char *key, bool default_val);

/**
 * Get current string config value.
 * @param key          Config key
 * @param out          Output buffer
 * @param maxlen       Buffer size
 * @param default_val  Default if key doesn't exist (can be NULL)
 * @return true if value exists
 */
bool pulsync_config_get_string(const char *key, char *out, size_t maxlen, const char *default_val);

/**
 * Handle config push from server (called from heartbeat/WSS handler).
 * Parses JSON config object, updates values, fires callbacks.
 * @param json      JSON string containing config key-value pairs
 * @param json_len  Length of JSON string
 */
void pulsync_config_handle_push(const char *json, size_t json_len);

/**
 * Register a generic callback for any config change.
 */
void pulsync_config_on_change(pulsync_config_cb_t cb);

#ifdef __cplusplus
}
#endif
