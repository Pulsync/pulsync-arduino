/**
 * Pulsync — Non-blocking task scheduler + real-time cloud sync for ESP32
 *
 * Main include file. Include only this header in your sketch/project.
 *
 * Usage:
 *   #include <Pulsync.h>
 *
 *   void setup() {
 *     Pulsync.setWiFi("SSID", "password");
 *     Pulsync.begin("PUL-XXXXXX");
 *     Pulsync.setInterval([]() { ... }, 5000);
 *   }
 *
 *   void loop() {
 *     Pulsync.loop();
 *   }
 *
 * https://github.com/pulsync/pulsync-arduino
 * License: MIT
 */

#pragma once

#include <Arduino.h>
#include <functional>
#include <cstdint>

/* C core modules */
extern "C" {
#include "core/pulsync_scheduler.h"
#include "core/pulsync_nvs.h"
#include "core/pulsync_wifi.h"
#include "core/pulsync_wifi_store.h"
#include "core/pulsync_transport.h"
#include "core/pulsync_enroll.h"
#include "core/pulsync_heartbeat.h"
#include "core/pulsync_ota.h"
#include "core/pulsync_config.h"
#include "core/pulsync_portal.h"
#include "core/pulsync_version.h"
}

/* Built-in server candidates for zero-config discovery. Override via -D flags.
 * Cloud is a TLS domain; local is an mDNS name resolved on the LAN. */
#ifndef PULSYNC_CLOUD_SERVER
#define PULSYNC_CLOUD_SERVER  "pulsync.in"
#endif
#ifndef PULSYNC_LOCAL_SERVER
#define PULSYNC_LOCAL_SERVER  "pulsync.local"
#endif

/* Discovery probe timeout (short so a dead candidate fails fast) and the
 * retry cadence once both candidates fail (device idle until a server shows). */
#ifndef PULSYNC_DISCOVERY_TIMEOUT_MS
#define PULSYNC_DISCOVERY_TIMEOUT_MS  5000
#endif
#ifndef PULSYNC_NO_SERVER_RETRY_MS
#define PULSYNC_NO_SERVER_RETRY_MS    15000
#endif

class PulsyncClass {
public:
    PulsyncClass();

    /* ===== Initialization ===== */

    /**
     * Set WiFi credentials (up to 3 profiles).
     * Call before begin(). Can be called multiple times for multiple networks.
     */
    void setWiFi(const char *ssid, const char *password = nullptr);

    /**
     * Set server URL. Heuristic: IP → HTTP, domain → HTTPS.
     * Optional — defaults to "pulsync.in" (hosted). Device paths are
     * "/api/device/..." so enroll resolves to https://pulsync.in/api/device/enroll.
     */
    void setServer(const char *server);

    /**
     * Set the firmware version this build reports (e.g. "1.2.3" or
     * "1.2.3-dev4"). Call BEFORE begin().
     *
     * Prefer the compile flag -D PULSYNC_FW_VERSION=\"1.2.3\" on PlatformIO;
     * this method is the Arduino-IDE-friendly alternative. If both are set,
     * this override wins.
     *
     * IMPORTANT: the version you report MUST match the .bin you upload for OTA
     * — bump it every time you cut a build you intend to deploy. Ordering is
     * numeric: 1.2.3 < 1.2.3-dev1 < 1.2.3-dev2 (only the numbers matter; the
     * label text is a note).
     */
    void setVersion(const char *version);

    /**
     * Initialize and start Pulsync.
     * @param pairing_code  Pairing code from the portal (e.g., "PUL-A7K9X2")
     * @return true if NVS initialized and startup succeeded
     */
    bool begin(const char *pairing_code = nullptr);

    /**
     * Main loop — call from Arduino loop().
     * Processes scheduler callbacks. Never blocks.
     */
    void loop();

    /* ===== Scheduler ===== */

    /** Schedule a repeating callback every interval_ms. */
    uint16_t setInterval(std::function<void()> cb, uint32_t interval_ms);

    /** Schedule a one-shot callback after delay_ms. */
    uint16_t setTimeout(std::function<void()> cb, uint32_t delay_ms);

    /** Cancel a scheduled task (timeout or interval). */
    bool clearTimeout(uint16_t id);
    bool clearInterval(uint16_t id);

    /* ===== Data Sending ===== */

    /** Send a single key-value pair to the server. */
    void send(const char *key, float value);
    void send(const char *key, int value);
    void send(const char *key, bool value);
    void send(const char *key, const char *value);

    /** Grouped payload (multiple key-values in one message). */
    void beginPayload();
    void add(const char *key, float value);
    void add(const char *key, int value);
    void add(const char *key, bool value);
    void add(const char *key, const char *value);
    void endPayload();

    /* ===== Commands (receiving) ===== */

    /**
     * Simple typed handlers — server sends {"command":"name","payload":{"value":X}}
     * The library extracts "value" and passes it directly.
     */
    void onInt(const char *command, std::function<void(int)> handler);
    void onFloat(const char *command, std::function<void(float)> handler);
    void onBool(const char *command, std::function<void(bool)> handler);
    void onString(const char *command, std::function<void(const char *)> handler);

    /** No-value handler (just triggers, e.g., "reboot") */
    void on(const char *command, std::function<void()> handler);

    /** Full payload handler for complex commands */
    class Payload {
    public:
        Payload(const char *json, size_t len);
        int getInt(const char *key, int def = 0);
        float getFloat(const char *key, float def = 0.0f);
        bool getBool(const char *key, bool def = false);
        const char *getString(const char *key, const char *def = "");
        bool has(const char *key);
        const char *raw() const { return _json; }
    private:
        const char *_json;
        size_t _len;
        char _str_buf[64];
    };
    void on(const char *command, std::function<void(Payload &)> handler);

    /* ===== Remote Config ===== */

    /** Register callback for config changes (typed). */
    void onConfig(const char *key, std::function<void(int)> cb, int default_val = 0);
    void onConfig(const char *key, std::function<void(float)> cb, float default_val = 0.0f);
    void onConfig(const char *key, std::function<void(bool)> cb, bool default_val = false);
    void onConfig(const char *key, std::function<void(const char *)> cb, const char *default_val = "");

    /** Get current config value. */
    int config(const char *key, int default_val);
    float config(const char *key, float default_val);
    bool config(const char *key, bool default_val);

    /* ===== Configuration ===== */

    /** Set heartbeat interval (minimum 30s). */
    void setHeartbeatInterval(uint32_t interval_ms);

    /**
     * Set the factory reset GPIO pin.
     * Hold for 5 seconds to erase NVS and restart captive portal.
     * Default: GPIO0 (BOOT button on most ESP32 dev boards).
     */
    void setResetPin(int pin);

    /* ===== Status ===== */

    /** Check if connected to server (any transport). */
    bool isConnected();

    /** Check if WiFi is connected. */
    bool isWiFiConnected();

    /** Get current transport type name ("mqtt" when connected, "none" otherwise). */
    const char *getTransportName();

    /** Get device IP address. */
    const char *getIP();

    /**
     * Number of messages waiting in the offline queue.
     * 0 means the link is healthy. Use this for backpressure: when it grows,
     * the connection is down and you can skip non-critical sends.
     */
    int queuedCount();

    /** Offline queue capacity (max messages buffered while disconnected). */
    int queueCapacity();

private:
    bool _begun;
    int _wifi_count;
    /* Staged hardcoded WiFi creds from setWiFi(), used only to seed NVS on the
     * very first boot. NVS is the source of truth once seeded. */
    struct StagedWiFi {
        char ssid[PULSYNC_WIFI_STORE_SSID_MAXLEN];
        char pass[PULSYNC_WIFI_STORE_PASS_MAXLEN];
    };
    StagedWiFi _staged_wifi[PULSYNC_WIFI_MAX_CREDS];
    char _server[128];
    bool _server_explicit;  /* true if setServer() was called by user code */
    char _pairing_code[16];
    bool _payload_active;
    char _payload_buf[512];
    size_t _payload_len;
    int _reset_pin;
    uint32_t _reset_press_start;
    bool _hb_started;  /* heartbeat started once transport first connected */

    /* ----- Server discovery (cloud → local failover) -----
     * Runs only when the user gave no explicit server (setServer / NVS). Tries
     * the cloud candidate first, then local (mDNS). The winning server host is
     * persisted so an enrolled device sticks to it and skips discovery. */
    enum DiscoveryState {
        DISC_OVERRIDE = 0,  /* explicit server set — no discovery */
        DISC_TRY_CLOUD,
        DISC_TRY_LOCAL,
        DISC_RESOLVED,
        DISC_NO_SERVER
    };
    DiscoveryState _disc_state;
    uint32_t _disc_next_attempt;   /* millis timestamp for next probe/retry */
    bool _disc_no_server_logged;   /* so we print the NO_SERVER banner once per cycle */
    void _applyServerCandidate(const char *host);  /* point transport at a host */
    bool _probeAndEnroll();        /* attempt enroll against current target; true on success */
    void _runDiscovery();          /* state machine, called from loop() */

    void _initModules();

public:
    /* These are called from C trampolines — must be accessible */
    void _handleRx(const char *topic, const char *payload, size_t len);
    void _handleHeartbeatResponse(const pulsync_heartbeat_response_t *resp);
    void _handlePortalCredentials(const char *ssid, const char *password,
                                   const char *server, const char *code,
                                   const char *device_name);
    void _handlePortalServer(const char *server);
    void _handlePortalRepair(const char *pairing_code);

private:

    /* Command handlers */
    enum CmdType { CMD_INT, CMD_FLOAT, CMD_BOOL, CMD_STR, CMD_VOID, CMD_PAYLOAD };
    struct CmdHandler {
        char command[32];
        CmdType type;
        std::function<void(int)> cb_int;
        std::function<void(float)> cb_float;
        std::function<void(bool)> cb_bool;
        std::function<void(const char*)> cb_str;
        std::function<void()> cb_void;
        std::function<void(Payload&)> cb_payload;
    };
    static const int MAX_CMD_HANDLERS = 16;
    CmdHandler _cmd_handlers[MAX_CMD_HANDLERS];
    int _cmd_handler_count;

    /* Config callbacks stored as std::function (bridge to C callbacks) */
    struct ConfigCbInt { std::function<void(int)> cb; };
    struct ConfigCbFloat { std::function<void(float)> cb; };
    struct ConfigCbBool { std::function<void(bool)> cb; };
    struct ConfigCbStr { std::function<void(const char*)> cb; };

    static const int MAX_CONFIG_CBS = 16;
    static ConfigCbInt _config_int_cbs[MAX_CONFIG_CBS];
    static ConfigCbFloat _config_float_cbs[MAX_CONFIG_CBS];
    static ConfigCbBool _config_bool_cbs[MAX_CONFIG_CBS];
    static ConfigCbStr _config_str_cbs[MAX_CONFIG_CBS];
    static int _config_int_count;
    static int _config_float_count;
    static int _config_bool_count;
    static int _config_str_count;

    /* Scheduler: store std::function callbacks */
    struct SchedCb {
        std::function<void()> cb;
        bool in_use;
    };
    static const int MAX_SCHED_CBS = PULSYNC_MAX_TASKS;
    static SchedCb _sched_cbs[MAX_SCHED_CBS];

    static void _schedTrampoline(void *arg);
};

/* Global singleton instance */
extern PulsyncClass Pulsync;
