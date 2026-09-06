/**
 * PulsyncClass — C++ wrapper implementation
 *
 * Wraps the C core modules into a singleton class with Arduino-friendly API.
 * Bridges std::function callbacks to C function pointers via static trampolines.
 */

#include "Pulsync.h"
#include <cstring>
#include <cstdio>

/* ---------- Global instance ---------- */

PulsyncClass Pulsync;

/* ---------- Static member definitions ---------- */

PulsyncClass::SchedCb PulsyncClass::_sched_cbs[MAX_SCHED_CBS] = {};
PulsyncClass::ConfigCbInt PulsyncClass::_config_int_cbs[MAX_CONFIG_CBS] = {};
PulsyncClass::ConfigCbFloat PulsyncClass::_config_float_cbs[MAX_CONFIG_CBS] = {};
PulsyncClass::ConfigCbBool PulsyncClass::_config_bool_cbs[MAX_CONFIG_CBS] = {};
PulsyncClass::ConfigCbStr PulsyncClass::_config_str_cbs[MAX_CONFIG_CBS] = {};
int PulsyncClass::_config_int_count = 0;
int PulsyncClass::_config_float_count = 0;
int PulsyncClass::_config_bool_count = 0;
int PulsyncClass::_config_str_count = 0;

/* ---------- Singleton pointer for C callbacks ---------- */

static PulsyncClass *s_instance = nullptr;

/* ---------- C callback trampolines ---------- */

static void rx_trampoline(const char *topic, const char *payload, size_t len) {
    if (s_instance) s_instance->_handleRx(topic, payload, len);
}

static void heartbeat_trampoline(const pulsync_heartbeat_response_t *resp) {
    if (s_instance) s_instance->_handleHeartbeatResponse(resp);
}

static void portal_creds_trampoline(const char *ssid, const char *password,
                                    const char *server, const char *code,
                                    const char *device_name) {
    if (s_instance) s_instance->_handlePortalCredentials(ssid, password, server, code, device_name);
}

static void factory_reset_trampoline(void) {
    Serial.println("[PULSYNC] Factory reset from config portal — erasing NVS");
    pulsync_nvs_erase_all();
    delay(500);
    ESP.restart();
}

static void portal_needed_trampoline(void) {
    /* Auto-portal (no WiFi connected) shows the ENROLL form. Set the mode
     * explicitly so a prior config session doesn't leave it in CONFIG mode. */
    pulsync_portal_set_mode(PULSYNC_PORTAL_MODE_ENROLL);
    /* Refresh the portal's network list snapshot before opening it. */
    pulsync_wifi_cred_t list[PULSYNC_WIFI_MAX_CREDS];
    int n = pulsync_wifi_store_count();
    for (int i = 0; i < n; i++) pulsync_wifi_store_get(i, &list[i]);
    pulsync_portal_set_wifi_list(list, n);
    /* Apply the password gate here too. A FRESH device is unenrolled with no
     * hash → open (pairing code is the gate). But an ENROLLED device that lost
     * WiFi lands here as well, and its config actions (WiFi/server/re-pair)
     * must still require the password — otherwise anyone on the open AP could
     * hijack it by forcing it offline. The gate keys on enrollment + hash, not
     * portal mode, so this closes that hole. */
    {
        bool enrolled = (pulsync_enroll_get_state() == PULSYNC_ENROLL_DONE);
        char cfg_hash[72] = {0};
        pulsync_nvs_get_str(PULSYNC_NVS_KEY_CFG_PW_HASH, cfg_hash, sizeof(cfg_hash));
        pulsync_portal_set_gate(cfg_hash, enrolled);
    }
    pulsync_portal_start();
}

/* Config menu: WiFi list saved. Replace the store, persist, apply, refresh
 * the portal's snapshot. Does NOT reconnect (that happens on Exit). */
static void portal_wifi_save_trampoline(const pulsync_wifi_cred_t *creds, int count) {
    pulsync_wifi_store_set_all(creds, count);
    pulsync_wifi_store_save();
    pulsync_wifi_store_apply();

    pulsync_wifi_cred_t list[PULSYNC_WIFI_MAX_CREDS];
    int n = pulsync_wifi_store_count();
    for (int i = 0; i < n; i++) pulsync_wifi_store_get(i, &list[i]);
    pulsync_portal_set_wifi_list(list, n);
}

/* Config menu: server address saved. */
static void portal_server_save_trampoline(const char *server) {
    if (s_instance) s_instance->_handlePortalServer(server);
}

/* Config menu: re-pair with a new pairing code. */
static void portal_repair_trampoline(const char *pairing_code) {
    if (s_instance) s_instance->_handlePortalRepair(pairing_code);
}

/* Config menu: Exit pressed — portal already stopped, reconnect now. */
static void portal_exit_trampoline(void) {
    Serial.println("[PULSYNC] Config portal exit — reconnecting WiFi");
    pulsync_wifi_disconnect();
    delay(300);
    pulsync_wifi_start();
}

void PulsyncClass::_schedTrampoline(void *arg) {
    int idx = (int)(intptr_t)arg;
    if (idx >= 0 && idx < MAX_SCHED_CBS && _sched_cbs[idx].in_use) {
        _sched_cbs[idx].cb();
    }
}

/* ---------- Constructor ---------- */

PulsyncClass::PulsyncClass()
    : _begun(false), _wifi_count(0), _server_explicit(false), _payload_active(false),
      _payload_len(0), _cmd_handler_count(0), _reset_pin(0), _reset_press_start(0),
      _hb_started(false), _server_mode(AUTO),
      _disc_state(DISC_TRY_LOCAL), _disc_next_attempt(0),
      _disc_no_server_logged(false) {
    memset(_server, 0, sizeof(_server));
    /* Hosted HTTP host. Device paths are "/api/device/..." so this yields
     * e.g. https://pulsync.in/api/device/enroll. */
    strncpy(_server, "pulsync.in", sizeof(_server) - 1);
    memset(_pairing_code, 0, sizeof(_pairing_code));
    memset(_payload_buf, 0, sizeof(_payload_buf));
    memset(_cmd_handlers, 0, sizeof(_cmd_handlers));
    memset(_staged_wifi, 0, sizeof(_staged_wifi));
    s_instance = this;
}

/* ---------- Initialization ---------- */

void PulsyncClass::setWiFi(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') return;
    /* Stage only — NVS isn't initialized yet (begin() does that). These creds
     * are used to seed NVS on the first boot; after that NVS wins and these
     * are ignored. */
    if (_wifi_count < PULSYNC_WIFI_MAX_CREDS) {
        StagedWiFi &s = _staged_wifi[_wifi_count];
        strncpy(s.ssid, ssid, sizeof(s.ssid) - 1);
        s.ssid[sizeof(s.ssid) - 1] = '\0';
        if (password) {
            strncpy(s.pass, password, sizeof(s.pass) - 1);
            s.pass[sizeof(s.pass) - 1] = '\0';
        } else {
            s.pass[0] = '\0';
        }
        _wifi_count++;
    }
}

void PulsyncClass::setServer(const char *server) {
    if (server) {
        strncpy(_server, server, sizeof(_server) - 1);
        _server[sizeof(_server) - 1] = '\0';
        _server_explicit = true;
    }
}

void PulsyncClass::setServerMode(int mode) {
    /* Hard override to a tier (no discovery, no fallback). setServer(url) still
     * wins over this — precedence is resolved in begin(). */
    if (mode == LOCAL || mode == CLOUD || mode == AUTO) {
        _server_mode = mode;
    }
}

void PulsyncClass::setVersion(const char *version) {
    /* Store a runtime override in the C core. Takes effect for the version
     * reported at enrollment and in every heartbeat, provided it's called
     * before begin(). */
    pulsync_version_set(version);
}

bool PulsyncClass::begin(const char *pairing_code) {
    if (_begun) return true;

    /* Store pairing code */
    if (pairing_code) {
        strncpy(_pairing_code, pairing_code, sizeof(_pairing_code) - 1);
    }

    _initModules();
    _begun = true;
    return true;
}

void PulsyncClass::_initModules() {
    /* Report the effective firmware version up front. It's already resolved
     * (runtime override via setVersion() > PULSYNC_FW_VERSION > "0.0.0"). */
    const char *fw = pulsync_version_get();
    Serial.printf("[PULSYNC] firmware version: %s\n", fw);
    if (strcmp(fw, "0.0.0") == 0) {
        Serial.println("[PULSYNC] WARNING: no firmware version set. OTA update "
                       "comparisons will not work. Set -D PULSYNC_FW_VERSION or "
                       "call Pulsync.setVersion(\"x.y.z\") before begin().");
    }

    /* 1. Scheduler */
    pulsync_scheduler_init();

    /* 2. NVS */
    if (!pulsync_nvs_init()) {
        Serial.println("[Pulsync] ERROR: NVS init failed");
        return;
    }

    /* 3. WiFi */
    pulsync_wifi_init();
    pulsync_wifi_on_portal_needed(portal_needed_trampoline);

    /* WiFi credential list — NVS is the source of truth once seeded.
     * First boot only: seed NVS from the hardcoded setWiFi() creds. On every
     * later boot (or once the portal has written creds), NVS wins and the
     * hardcoded creds are ignored. */
    pulsync_wifi_store_load();
    if (!pulsync_wifi_store_is_seeded()) {
        for (int i = 0; i < _wifi_count; i++) {
            pulsync_wifi_store_seed_add(_staged_wifi[i].ssid, _staged_wifi[i].pass);
        }
        pulsync_wifi_store_commit_seed();
        pulsync_wifi_store_load();  /* reload the just-seeded list */
        Serial.printf("[WIFI] Seeded %d hardcoded network(s) into NVS\n", _wifi_count);
    } else if (_wifi_count > 0) {
        Serial.println("[WIFI] NVS already provisioned — ignoring hardcoded setWiFi()");
    }
    pulsync_wifi_store_apply();
    {
        int n = pulsync_wifi_store_count();
        Serial.printf("[WIFI] %d saved network(s):\n", n);
        for (int i = 0; i < n; i++) {
            pulsync_wifi_cred_t c;
            if (pulsync_wifi_store_get(i, &c)) {
                Serial.printf("       %d. %s\n", i, c.ssid);
            }
        }
    }

    /* 4. Captive portal */
    pulsync_portal_init();
    pulsync_portal_on_credentials(portal_creds_trampoline);
    pulsync_portal_on_factory_reset(factory_reset_trampoline);
    pulsync_portal_on_wifi_save(portal_wifi_save_trampoline);
    pulsync_portal_on_server_save(portal_server_save_trampoline);
    pulsync_portal_on_repair(portal_repair_trampoline);
    pulsync_portal_on_exit(portal_exit_trampoline);

    /* 5. Config */
    pulsync_config_init();

    /* 6. OTA */
    pulsync_ota_init();

    /* 7. Enrollment — check NVS for existing token before transport init */
    /* Load pairing code from NVS if not set via begin() */
    if (_pairing_code[0] == '\0') {
        pulsync_nvs_get_str("pair_code", _pairing_code, sizeof(_pairing_code));
        if (_pairing_code[0] != '\0') {
            Serial.printf("[PULSYNC] Pairing code loaded from NVS: %s\n", _pairing_code);
        }
    }
    /* Resolve the server target by precedence:
     *   1. setServer(url)          — explicit host, highest priority
     *   2. setServerMode(LOCAL|CLOUD) — force a built-in tier (hard override)
     *   3. NVS-provisioned server  — from the portal / previous run
     *   4. AUTO (nothing set)      — discovery, local → cloud
     * (1)–(3) all set server_provisioned = no discovery. */
    bool server_provisioned = false;
    if (_server_explicit) {
        server_provisioned = true;
        Serial.printf("[PULSYNC] Server set explicitly: %s\n", _server);
    } else if (_server_mode == LOCAL) {
        strncpy(_server, PULSYNC_LOCAL_SERVER, sizeof(_server) - 1);
        _server[sizeof(_server) - 1] = '\0';
        server_provisioned = true;
        Serial.println("[PULSYNC] Server mode: LOCAL (" PULSYNC_LOCAL_SERVER ")");
    } else if (_server_mode == CLOUD) {
        strncpy(_server, PULSYNC_CLOUD_SERVER, sizeof(_server) - 1);
        _server[sizeof(_server) - 1] = '\0';
        server_provisioned = true;
        Serial.println("[PULSYNC] Server mode: CLOUD (" PULSYNC_CLOUD_SERVER ")");
    } else {
        char saved_server[128] = {0};
        if (pulsync_nvs_get_str(PULSYNC_NVS_KEY_SERVER, saved_server, sizeof(saved_server))) {
            if (saved_server[0] != '\0') {
                strncpy(_server, saved_server, sizeof(_server) - 1);
                server_provisioned = true;
                Serial.printf("[PULSYNC] Server loaded from NVS: %s\n", _server);
            }
        }
    }

    pulsync_enroll_init(_pairing_code);
    char _loaded_token[PULSYNC_TOKEN_MAXLEN] = {0};
    bool _already_enrolled = pulsync_enroll_check();
    if (_already_enrolled) {
        pulsync_enroll_get_token(_loaded_token, sizeof(_loaded_token));
        Serial.printf("[PULSYNC] Token loaded from NVS\n");
    }

    /* 8. Transport — MQTT primary, HTTP for enrollment/OTA */
    pulsync_transport_config_t tcfg = {};
    pulsync_transport_parse_url(_server, tcfg.server_host, sizeof(tcfg.server_host),
                                &tcfg.http_port, &tcfg.mqtt_port, &tcfg.use_tls);
    strncpy(tcfg.device_token, _loaded_token, PULSYNC_TOKEN_MAXLEN - 1);

    pulsync_transport_init(&tcfg);
    pulsync_transport_on_receive(rx_trampoline);

    /* If already enrolled, restore the server-provided MQTT endpoint from NVS
     * so we reconnect to the right broker without re-enrolling. Absent → the
     * transport derives the broker from the HTTP host (back-compat). */
    if (_already_enrolled) {
        char saved_mqtt[128] = {0};
        if (pulsync_nvs_get_str(PULSYNC_NVS_KEY_MQTT_URL, saved_mqtt, sizeof(saved_mqtt)) &&
            saved_mqtt[0] != '\0') {
            pulsync_transport_set_mqtt_url(saved_mqtt);
            Serial.printf("[PULSYNC] MQTT endpoint from NVS: %s\n", saved_mqtt);
        }
    }

    /* Decide the discovery strategy:
     *  - Explicit setServer()/NVS server → OVERRIDE, never auto-discover.
     *  - Already enrolled → stick to the server the token was issued by
     *    (token_srv), skip discovery (RESOLVED). If that host stops answering,
     *    loop() re-runs discovery + re-enrolls.
     *  - Fresh device → discover local → cloud. */
    if (server_provisioned) {
        _disc_state = DISC_OVERRIDE;
        Serial.println("[PULSYNC] Server fixed — auto-discovery disabled");
    } else if (_already_enrolled) {
        char token_srv[128] = {0};
        if (pulsync_nvs_get_str(PULSYNC_NVS_KEY_TOKEN_SRV, token_srv, sizeof(token_srv)) &&
            token_srv[0] != '\0') {
            _applyServerCandidate(token_srv);
            Serial.printf("[PULSYNC] Enrolled with '%s' — using it (discovery skipped)\n", token_srv);
        } else {
            /* Enrolled but we don't know which server (older token). Default to
             * the current _server and carry on. */
            Serial.println("[PULSYNC] Enrolled (server unknown) — using current target");
        }
        _disc_state = DISC_RESOLVED;
    } else {
        _disc_state = DISC_TRY_LOCAL;
        Serial.println("[PULSYNC] No server set — will discover (local → cloud)");
    }

    /* 9. Heartbeat */
    pulsync_heartbeat_init(PULSYNC_HEARTBEAT_DEFAULT_MS);
    pulsync_heartbeat_on_response(heartbeat_trampoline);

    /* 10. Start WiFi (triggers connection sequence) */
    pulsync_wifi_start();

    /* 13. Configure reset button (GPIO0 / BOOT by default) */
    if (_reset_pin >= 0) {
        pinMode(_reset_pin, INPUT_PULLUP);
    }

    /* 11. Transport + heartbeat started from loop() after enrollment */
}

void PulsyncClass::loop() {
    /* Run scheduler (user callbacks) */
    pulsync_scheduler_tick();

    /* WiFi reconnect tick */
    pulsync_wifi_tick();

    /* Once WiFi is up, run discovery (if needed) then enrollment + MQTT. */
    if (pulsync_wifi_is_connected()) {
        /* Server discovery gates everything. For OVERRIDE it's a no-op that
         * leaves us free to enroll against the fixed target. For auto-discovery
         * it must reach RESOLVED before we proceed. */
        _runDiscovery();

        bool ready = (_disc_state == DISC_OVERRIDE || _disc_state == DISC_RESOLVED);

        if (ready) {
            /* Enrollment via HTTP (blocking, runs once) */
            if (pulsync_enroll_get_state() != PULSYNC_ENROLL_DONE &&
                pulsync_enroll_get_state() != PULSYNC_ENROLL_IN_PROGRESS &&
                pulsync_enroll_get_state() != PULSYNC_ENROLL_FAILED) {
                if (!pulsync_enroll_check()) {
                    Serial.println("[PULSYNC] Enrolling via HTTP...");
                    pulsync_enroll_start();  /* blocking HTTP POST */
                }
            }

            /* Start MQTT after enrollment (token needed for auth) */
            if (pulsync_enroll_get_state() == PULSYNC_ENROLL_DONE) {
                pulsync_transport_start();  /* no-op if already started */

                /* Start the heartbeat loop once, the first time the transport
                 * comes up. Without this s_running stays false and no heartbeat
                 * is ever sent — which also means OTA offers / config pushes
                 * (delivered in the heartbeat response) never reach the device. */
                if (!_hb_started && pulsync_transport_is_connected()) {
                    pulsync_heartbeat_start();
                    _hb_started = true;
                }

                pulsync_heartbeat_tick();

                /* Enrolled-device server validation (discovery path only, not
                 * a user-fixed OVERRIDE server). If heartbeats have not
                 * succeeded for a sustained window, the server we picked has
                 * stopped answering — re-run discovery and re-register. We only
                 * arm this once at least one heartbeat has succeeded, so a slow
                 * first connect doesn't trip it. */
                if (_disc_state == DISC_RESOLVED && _hb_started) {
                    uint32_t since = pulsync_heartbeat_time_since_last();
                    const uint32_t stale_ms = 4 * PULSYNC_HEARTBEAT_DEFAULT_MS; /* ~2 min */
                    if (since != UINT32_MAX && since > stale_ms) {
                        Serial.println("[PULSYNC] Server stopped answering — re-discovering.");
                        pulsync_heartbeat_stop();
                        pulsync_transport_stop();
                        _hb_started = false;
                        pulsync_enroll_reset();   /* force re-enroll on the rediscovered server */
                        pulsync_nvs_erase_key(PULSYNC_NVS_KEY_TOKEN_SRV);
                        _disc_state = DISC_TRY_LOCAL;
                    }
                }
            }
        }
    }

    /* Factory reset button check (hold 5s to reset) */
    if (_reset_pin >= 0) {
        if (digitalRead(_reset_pin) == LOW) {
            if (_reset_press_start == 0) {
                _reset_press_start = millis();
            } else if (millis() - _reset_press_start >= 3000 && !pulsync_portal_is_active()) {
                /* Hold 3s → open config portal (keeps NVS) */
                Serial.println("[PULSYNC] Config button held — opening config portal");
                pulsync_portal_set_mode(PULSYNC_PORTAL_MODE_CONFIG);
                /* Refresh the portal's saved-network snapshot before opening. */
                {
                    pulsync_wifi_cred_t list[PULSYNC_WIFI_MAX_CREDS];
                    int n = pulsync_wifi_store_count();
                    for (int i = 0; i < n; i++) pulsync_wifi_store_get(i, &list[i]);
                    pulsync_portal_set_wifi_list(list, n);
                    Serial.printf("[PULSYNC] Portal snapshot: %d network(s)\n", n);
                }
                /* Gate the config menu behind the server-issued password when
                 * this device is enrolled. Hand the portal the stored hash +
                 * enrolled state; it stays NVS-agnostic. Unenrolled or no hash
                 * → open (pairing code is the gate for a fresh device). */
                {
                    bool enrolled = (pulsync_enroll_get_state() == PULSYNC_ENROLL_DONE);
                    char cfg_hash[72] = {0};
                    pulsync_nvs_get_str(PULSYNC_NVS_KEY_CFG_PW_HASH, cfg_hash, sizeof(cfg_hash));
                    pulsync_portal_set_gate(cfg_hash, enrolled);
                    Serial.printf("[PULSYNC] Config gate: %s\n",
                                  (enrolled && cfg_hash[0]) ? "LOCKED (password required)" : "open");
                }
                bool _ok = pulsync_portal_start();
                Serial.printf("[PULSYNC] Portal start %s. Connect to AP '%s'\n",
                              _ok ? "OK" : "FAILED", pulsync_portal_get_ssid());
                _reset_press_start = 0;
            }
        } else {
            _reset_press_start = 0;
        }
    }
}

/* ---------- Server discovery (cloud → local failover) ---------- */

void PulsyncClass::_applyServerCandidate(const char *host) {
    if (!host || host[0] == '\0') return;
    strncpy(_server, host, sizeof(_server) - 1);
    _server[sizeof(_server) - 1] = '\0';
    /* Parse into host/ports/tls and point the transport at it. Also clear any
     * MQTT override so the broker is re-derived (or replaced by the mqtt_url
     * the chosen server returns on enroll). */
    char h[64]; uint16_t hp, mp; bool tls;
    pulsync_transport_parse_url(_server, h, sizeof(h), &hp, &mp, &tls);
    pulsync_transport_set_server(h, hp, mp, tls);
    pulsync_transport_set_mqtt_url(NULL);  /* re-derive until enroll provides one */
}

bool PulsyncClass::_probeAndEnroll() {
    /* Use a short HTTP timeout so a dead candidate fails fast during discovery. */
    pulsync_transport_set_http_timeout(PULSYNC_DISCOVERY_TIMEOUT_MS);
    pulsync_enroll_start();  /* blocking HTTP POST against the current target */
    bool ok = (pulsync_enroll_get_state() == PULSYNC_ENROLL_DONE);
    /* Restore a normal timeout for regular operation. */
    pulsync_transport_set_http_timeout(10000);
    return ok;
}

void PulsyncClass::_runDiscovery() {
    /* Nothing to do for override or once resolved. */
    if (_disc_state == DISC_OVERRIDE || _disc_state == DISC_RESOLVED) return;

    uint32_t now = millis();

    switch (_disc_state) {
        case DISC_TRY_LOCAL: {
            Serial.println("[DISCOVERY] Trying local (" PULSYNC_LOCAL_SERVER ")...");
            _applyServerCandidate(PULSYNC_LOCAL_SERVER);
            pulsync_enroll_init(_pairing_code);  /* reset enroll state for this target */
            if (_probeAndEnroll()) {
                pulsync_nvs_set_str(PULSYNC_NVS_KEY_TOKEN_SRV, PULSYNC_LOCAL_SERVER);
                Serial.println("[DISCOVERY] Local reachable — enrolled.");
                _disc_state = DISC_RESOLVED;
            } else {
                Serial.println("[DISCOVERY] Local unavailable — trying cloud.");
                _disc_state = DISC_TRY_CLOUD;
            }
            break;
        }

        case DISC_TRY_CLOUD: {
            Serial.println("[DISCOVERY] Trying cloud (" PULSYNC_CLOUD_SERVER ")...");
            _applyServerCandidate(PULSYNC_CLOUD_SERVER);
            pulsync_enroll_init(_pairing_code);
            if (_probeAndEnroll()) {
                pulsync_nvs_set_str(PULSYNC_NVS_KEY_TOKEN_SRV, PULSYNC_CLOUD_SERVER);
                Serial.println("[DISCOVERY] Cloud reachable — enrolled.");
                _disc_state = DISC_RESOLVED;
            } else {
                Serial.println("[DISCOVERY] Cloud unavailable.");
                _disc_state = DISC_NO_SERVER;
                _disc_no_server_logged = false;
                _disc_next_attempt = now + PULSYNC_NO_SERVER_RETRY_MS;
            }
            break;
        }

        case DISC_NO_SERVER: {
            if (!_disc_no_server_logged) {
                Serial.println("========================================");
                Serial.println("[PULSYNC] NO SERVER FOUND (tried local + cloud).");
                Serial.println("[PULSYNC] Device idle — cannot operate without a server.");
                Serial.printf( "[PULSYNC] Retrying in %d s...\n", PULSYNC_NO_SERVER_RETRY_MS / 1000);
                Serial.println("========================================");
                _disc_no_server_logged = true;
            }
            if ((int32_t)(now - _disc_next_attempt) >= 0) {
                _disc_state = DISC_TRY_LOCAL;  /* restart the cycle (local first) */
            }
            break;
        }

        default:
            break;
    }
}

/* ---------- Scheduler ---------- */

uint16_t PulsyncClass::setInterval(std::function<void()> cb, uint32_t interval_ms) {
    /* Find free slot in std::function array */
    int slot = -1;
    for (int i = 0; i < MAX_SCHED_CBS; i++) {
        if (!_sched_cbs[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return 0;

    _sched_cbs[slot].cb = cb;
    _sched_cbs[slot].in_use = true;

    uint16_t id = pulsync_set_interval_arg(_schedTrampoline, interval_ms, (void *)(intptr_t)slot);
    if (id == 0) {
        _sched_cbs[slot].in_use = false;
    }
    return id;
}

uint16_t PulsyncClass::setTimeout(std::function<void()> cb, uint32_t delay_ms) {
    int slot = -1;
    for (int i = 0; i < MAX_SCHED_CBS; i++) {
        if (!_sched_cbs[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return 0;

    _sched_cbs[slot].cb = cb;
    _sched_cbs[slot].in_use = true;

    uint16_t id = pulsync_set_timeout_arg(_schedTrampoline, delay_ms, (void *)(intptr_t)slot);
    if (id == 0) {
        _sched_cbs[slot].in_use = false;
    }
    return id;
}

bool PulsyncClass::clearTimeout(uint16_t id) {
    /* Note: we don't free the slot here since we don't track id→slot mapping.
       The slot's std::function will be overwritten on next allocation.
       For timeouts that fire, the slot should be freed in the trampoline. */
    return pulsync_clear_task(id);
}

bool PulsyncClass::clearInterval(uint16_t id) {
    return pulsync_clear_task(id);
}

/* ---------- Data Sending ---------- */

void PulsyncClass::send(const char *key, float value) {
    if (pulsync_enroll_get_state() != PULSYNC_ENROLL_DONE) return;
    char payload[128];
    int len = snprintf(payload, sizeof(payload), "{\"%s\":%.4f}", key, (double)value);
    pulsync_transport_publish("data", payload, (size_t)len);
}

void PulsyncClass::send(const char *key, int value) {
    char payload[128];
    int len = snprintf(payload, sizeof(payload), "{\"%s\":%d}", key, value);
    pulsync_transport_publish("data", payload, (size_t)len);
}

void PulsyncClass::send(const char *key, bool value) {
    char payload[128];
    int len = snprintf(payload, sizeof(payload), "{\"%s\":%s}", key, value ? "true" : "false");
    pulsync_transport_publish("data", payload, (size_t)len);
}

void PulsyncClass::send(const char *key, const char *value) {
    char payload[256];
    int len = snprintf(payload, sizeof(payload), "{\"%s\":\"%s\"}", key, value);
    pulsync_transport_publish("data", payload, (size_t)len);
}

void PulsyncClass::beginPayload() {
    _payload_active = true;
    _payload_len = 0;
    _payload_buf[0] = '{';
    _payload_len = 1;
}

void PulsyncClass::add(const char *key, float value) {
    if (!_payload_active) return;
    int written = snprintf(_payload_buf + _payload_len,
                           sizeof(_payload_buf) - _payload_len,
                           "%s\"%s\":%.4f",
                           _payload_len > 1 ? "," : "", key, (double)value);
    if (written > 0) _payload_len += (size_t)written;
}

void PulsyncClass::add(const char *key, int value) {
    if (!_payload_active) return;
    int written = snprintf(_payload_buf + _payload_len,
                           sizeof(_payload_buf) - _payload_len,
                           "%s\"%s\":%d",
                           _payload_len > 1 ? "," : "", key, value);
    if (written > 0) _payload_len += (size_t)written;
}

void PulsyncClass::add(const char *key, bool value) {
    if (!_payload_active) return;
    int written = snprintf(_payload_buf + _payload_len,
                           sizeof(_payload_buf) - _payload_len,
                           "%s\"%s\":%s",
                           _payload_len > 1 ? "," : "", key, value ? "true" : "false");
    if (written > 0) _payload_len += (size_t)written;
}

void PulsyncClass::add(const char *key, const char *value) {
    if (!_payload_active) return;
    int written = snprintf(_payload_buf + _payload_len,
                           sizeof(_payload_buf) - _payload_len,
                           "%s\"%s\":\"%s\"",
                           _payload_len > 1 ? "," : "", key, value);
    if (written > 0) _payload_len += (size_t)written;
}

void PulsyncClass::endPayload() {
    if (!_payload_active) return;
    if (pulsync_enroll_get_state() != PULSYNC_ENROLL_DONE) {
        _payload_active = false;
        return;
    }
    if (_payload_len < sizeof(_payload_buf) - 1) {
        _payload_buf[_payload_len++] = '}';
        _payload_buf[_payload_len] = '\0';
    }
    _payload_active = false;

    pulsync_transport_publish("data", _payload_buf, _payload_len);
}

/* ---------- Commands ---------- */

void PulsyncClass::onInt(const char *command, std::function<void(int)> handler) {
    if (_cmd_handler_count >= MAX_CMD_HANDLERS) return;
    CmdHandler &h = _cmd_handlers[_cmd_handler_count++];
    strncpy(h.command, command, sizeof(h.command) - 1);
    h.type = CMD_INT;
    h.cb_int = handler;
}

void PulsyncClass::onFloat(const char *command, std::function<void(float)> handler) {
    if (_cmd_handler_count >= MAX_CMD_HANDLERS) return;
    CmdHandler &h = _cmd_handlers[_cmd_handler_count++];
    strncpy(h.command, command, sizeof(h.command) - 1);
    h.type = CMD_FLOAT;
    h.cb_float = handler;
}

void PulsyncClass::onBool(const char *command, std::function<void(bool)> handler) {
    if (_cmd_handler_count >= MAX_CMD_HANDLERS) return;
    CmdHandler &h = _cmd_handlers[_cmd_handler_count++];
    strncpy(h.command, command, sizeof(h.command) - 1);
    h.type = CMD_BOOL;
    h.cb_bool = handler;
}

void PulsyncClass::onString(const char *command, std::function<void(const char *)> handler) {
    if (_cmd_handler_count >= MAX_CMD_HANDLERS) return;
    CmdHandler &h = _cmd_handlers[_cmd_handler_count++];
    strncpy(h.command, command, sizeof(h.command) - 1);
    h.type = CMD_STR;
    h.cb_str = handler;
}

void PulsyncClass::on(const char *command, std::function<void()> handler) {
    if (_cmd_handler_count >= MAX_CMD_HANDLERS) return;
    CmdHandler &h = _cmd_handlers[_cmd_handler_count++];
    strncpy(h.command, command, sizeof(h.command) - 1);
    h.type = CMD_VOID;
    h.cb_void = handler;
}

void PulsyncClass::on(const char *command, std::function<void(Payload &)> handler) {
    if (_cmd_handler_count >= MAX_CMD_HANDLERS) return;
    CmdHandler &h = _cmd_handlers[_cmd_handler_count++];
    strncpy(h.command, command, sizeof(h.command) - 1);
    h.type = CMD_PAYLOAD;
    h.cb_payload = handler;
}

/* ---------- Payload parser ---------- */

static size_t _json_extract(const char *json, size_t len, const char *key, char *out, size_t out_max) {
    if (!json || !key || !out) return 0;
    size_t key_len = strlen(key);
    const char *pos = json;
    const char *end = json + len;
    while (pos < end) {
        const char *found = strstr(pos, key);
        if (!found || found >= end) return 0;
        if (found > json && *(found - 1) == '"') {
            const char *after = found + key_len;
            if (after < end && *after == '"') {
                after++;
                while (after < end && (*after == ':' || *after == ' ')) after++;
                if (after >= end) return 0;
                /* Value could be string, number, bool */
                if (*after == '"') {
                    after++;
                    const char *vs = after;
                    while (after < end && *after != '"') after++;
                    size_t vl = (size_t)(after - vs);
                    if (vl >= out_max) vl = out_max - 1;
                    memcpy(out, vs, vl);
                    out[vl] = '\0';
                    return vl;
                } else {
                    /* number or bool */
                    const char *vs = after;
                    while (after < end && *after != ',' && *after != '}' && *after != ' ') after++;
                    size_t vl = (size_t)(after - vs);
                    if (vl >= out_max) vl = out_max - 1;
                    memcpy(out, vs, vl);
                    out[vl] = '\0';
                    return vl;
                }
            }
        }
        pos = found + 1;
    }
    return 0;
}

PulsyncClass::Payload::Payload(const char *json, size_t len) : _json(json), _len(len) {
    _str_buf[0] = '\0';
}

int PulsyncClass::Payload::getInt(const char *key, int def) {
    char buf[32];
    if (_json_extract(_json, _len, key, buf, sizeof(buf)) > 0) return atoi(buf);
    return def;
}

float PulsyncClass::Payload::getFloat(const char *key, float def) {
    char buf[32];
    if (_json_extract(_json, _len, key, buf, sizeof(buf)) > 0) return (float)atof(buf);
    return def;
}

bool PulsyncClass::Payload::getBool(const char *key, bool def) {
    char buf[16];
    if (_json_extract(_json, _len, key, buf, sizeof(buf)) > 0) {
        return (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0);
    }
    return def;
}

const char *PulsyncClass::Payload::getString(const char *key, const char *def) {
    if (_json_extract(_json, _len, key, _str_buf, sizeof(_str_buf)) > 0) return _str_buf;
    return def;
}

bool PulsyncClass::Payload::has(const char *key) {
    char buf[4];
    return _json_extract(_json, _len, key, buf, sizeof(buf)) > 0;
}

/* ---------- Remote Config ---------- */

/* Static config callback trampolines */
static void config_int_trampoline(int32_t value) {
    /* This is called from C with the index embedded... we need per-key callbacks.
       We use a simpler approach: register all callbacks and dispatch from _handleHeartbeatResponse */
}

void PulsyncClass::onConfig(const char *key, std::function<void(int)> cb, int default_val) {
    if (_config_int_count >= MAX_CONFIG_CBS) return;
    int idx = _config_int_count++;
    _config_int_cbs[idx].cb = cb;

    /* Register with C config module using a static trampoline */
    /* Since C module fires typed callbacks, we use the generic on_change approach instead */
    pulsync_config_on_int(key, nullptr, (int32_t)default_val);
    /* Actual dispatch happens via generic callback in _handleHeartbeatResponse */
}

void PulsyncClass::onConfig(const char *key, std::function<void(float)> cb, float default_val) {
    if (_config_float_count >= MAX_CONFIG_CBS) return;
    _config_float_cbs[_config_float_count++].cb = cb;
    pulsync_config_on_float(key, nullptr, default_val);
}

void PulsyncClass::onConfig(const char *key, std::function<void(bool)> cb, bool default_val) {
    if (_config_bool_count >= MAX_CONFIG_CBS) return;
    _config_bool_cbs[_config_bool_count++].cb = cb;
    pulsync_config_on_bool(key, nullptr, default_val);
}

void PulsyncClass::onConfig(const char *key, std::function<void(const char *)> cb, const char *default_val) {
    if (_config_str_count >= MAX_CONFIG_CBS) return;
    _config_str_cbs[_config_str_count++].cb = cb;
    pulsync_config_on_string(key, nullptr, default_val);
}

int PulsyncClass::config(const char *key, int default_val) {
    return (int)pulsync_config_get_int(key, (int32_t)default_val);
}

float PulsyncClass::config(const char *key, float default_val) {
    return pulsync_config_get_float(key, default_val);
}

bool PulsyncClass::config(const char *key, bool default_val) {
    return pulsync_config_get_bool(key, default_val);
}

/* ---------- Configuration ---------- */

void PulsyncClass::setHeartbeatInterval(uint32_t interval_ms) {
    pulsync_heartbeat_set_interval(interval_ms);
}

void PulsyncClass::setResetPin(int pin) {
    _reset_pin = pin;
    pinMode(pin, INPUT_PULLUP);
}

/* ---------- Status ---------- */

bool PulsyncClass::isConnected() {
    return pulsync_transport_is_connected();
}

bool PulsyncClass::isWiFiConnected() {
    return pulsync_wifi_is_connected();
}

const char *PulsyncClass::getTransportName() {
    switch (pulsync_transport_get_state()) {
        case PULSYNC_TRANSPORT_CONNECTED: return "mqtt";
        case PULSYNC_TRANSPORT_CONNECTING: return "connecting";
        default: return "none";
    }
}

const char *PulsyncClass::getIP() {
    return pulsync_wifi_get_ip();
}

int PulsyncClass::queuedCount() {
    return pulsync_transport_queue_depth();
}

int PulsyncClass::queueCapacity() {
    return pulsync_transport_queue_capacity();
}

/* ---------- Internal handlers ---------- */

void PulsyncClass::_handleRx(const char *topic, const char *payload, size_t len) {
    if (!payload || len == 0) return;

    /* Check if this is an enrollment response */
    if (pulsync_enroll_get_state() == PULSYNC_ENROLL_IN_PROGRESS) {
        pulsync_enroll_handle_response(payload, len);
        return;
    }

    /* Check if it's a heartbeat response.
     * Over HTTP the response arrives synthetically as "http_response".
     * Over MQTT the server publishes the heartbeat reply (config/ota/commands/
     * new_token) to pulsync/{token}/download/config, so route that topic into
     * the same handler — otherwise OTA offers and config pushes are dropped on
     * the primary (MQTT) transport. */
    if (topic && (strcmp(topic, "http_response") == 0 ||
                  strstr(topic, "/download/config") != NULL)) {
        pulsync_heartbeat_handle_response(payload, len);
        return;
    }

    /* Check for command dispatch */
    /* Try to extract "command" field from JSON */
    const char *cmd_key = "\"command\":\"";
    const char *found = strstr(payload, cmd_key);
    if (found) {
        found += strlen(cmd_key);
        const char *end = strchr(found, '"');
        if (end) {
            char cmd_name[32] = {0};
            size_t cmd_len = (size_t)(end - found);
            if (cmd_len >= sizeof(cmd_name)) cmd_len = sizeof(cmd_name) - 1;
            memcpy(cmd_name, found, cmd_len);

            /* Find matching handler and dispatch by type */
            for (int i = 0; i < _cmd_handler_count; i++) {
                if (strcmp(_cmd_handlers[i].command, cmd_name) == 0) {
                    CmdHandler &h = _cmd_handlers[i];
                    /* Extract "value" from payload */
                    char val_buf[64] = {0};
                    _json_extract(payload, len, "value", val_buf, sizeof(val_buf));

                    switch (h.type) {
                        case CMD_INT:
                            if (h.cb_int) h.cb_int(atoi(val_buf));
                            break;
                        case CMD_FLOAT:
                            if (h.cb_float) h.cb_float((float)atof(val_buf));
                            break;
                        case CMD_BOOL:
                            if (h.cb_bool) h.cb_bool(strcmp(val_buf, "true") == 0 || strcmp(val_buf, "1") == 0);
                            break;
                        case CMD_STR:
                            if (h.cb_str) h.cb_str(val_buf);
                            break;
                        case CMD_VOID:
                            if (h.cb_void) h.cb_void();
                            break;
                        case CMD_PAYLOAD: {
                            Payload p(payload, len);
                            if (h.cb_payload) h.cb_payload(p);
                            break;
                        }
                    }
                    break;
                }
            }
        }
    }
}

void PulsyncClass::_handleHeartbeatResponse(const pulsync_heartbeat_response_t *resp) {
    if (!resp) return;

    /* Handle a server-pushed MQTT endpoint (broker relocation/migration). Only
     * act if it actually differs from the target we're using, then persist and
     * reconnect the transport to the new broker. */
    {
        char new_mqtt[128] = {0};
        if (_json_extract(resp->raw_json, resp->raw_json_len, "mqtt_url",
                          new_mqtt, sizeof(new_mqtt)) > 0 && new_mqtt[0] != '\0') {
            char cur_mqtt[128] = {0};
            pulsync_transport_get_mqtt_url(cur_mqtt, sizeof(cur_mqtt));
            if (strcmp(new_mqtt, cur_mqtt) != 0) {
                Serial.printf("[PULSYNC] MQTT endpoint changed: %s → %s\n", cur_mqtt, new_mqtt);
                if (pulsync_transport_set_mqtt_url(new_mqtt)) {
                    pulsync_nvs_set_str(PULSYNC_NVS_KEY_MQTT_URL, new_mqtt);
                    /* Reconnect to the new broker. */
                    pulsync_transport_stop();
                    pulsync_transport_start();
                }
            }
        }
    }

    /* Handle config-portal password rotation. The server advertises the current
     * hash every heartbeat; if it differs from what we've stored (dashboard
     * regenerate), update NVS so the config menu gates on the new password and
     * the old one stops working. Hash-only — plaintext never reaches us. */
    {
        char new_hash[72] = {0};
        if (_json_extract(resp->raw_json, resp->raw_json_len, "config_pw_hash",
                          new_hash, sizeof(new_hash)) > 0 && new_hash[0] != '\0') {
            char cur_hash[72] = {0};
            pulsync_nvs_get_str(PULSYNC_NVS_KEY_CFG_PW_HASH, cur_hash, sizeof(cur_hash));
            if (strcmp(new_hash, cur_hash) != 0) {
                pulsync_nvs_set_str(PULSYNC_NVS_KEY_CFG_PW_HASH, new_hash);
                Serial.println("[PULSYNC] Config-portal password rotated");
            }
        }
    }

    /* Handle config push */
    if (resp->has_config) {
        /* Extract "config" object from response JSON */
        const char *config_key = "\"config\":";
        const char *found = strstr(resp->raw_json, config_key);
        if (found) {
            found += strlen(config_key);
            /* Find the config object boundaries */
            if (*found == '{') {
                int depth = 0;
                const char *start = found;
                const char *p = found;
                while (p < resp->raw_json + resp->raw_json_len) {
                    if (*p == '{') depth++;
                    else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
                    p++;
                }
                pulsync_config_handle_push(start, (size_t)(p - start));
            }
        }
    }

    /* Handle OTA trigger */
    if (resp->has_ota) {
        pulsync_ota_handle_trigger(resp->raw_json, resp->raw_json_len);
    }

    /* Handle commands in heartbeat response */
    if (resp->has_commands) {
        /* Commands in heartbeat are dispatched via the normal rx path */
        /* The heartbeat response format wraps commands — extract and dispatch */
        const char *cmds_key = "\"commands\":";
        const char *found = strstr(resp->raw_json, cmds_key);
        if (found) {
            found += strlen(cmds_key);
            /* Commands is an array — dispatch each element */
            /* For now, dispatch the whole commands section */
            _handleRx("commands", found, resp->raw_json_len - (size_t)(found - resp->raw_json));
        }
    }
}

void PulsyncClass::_handlePortalCredentials(const char *ssid, const char *password,
                                             const char *server, const char *code,
                                             const char *device_name) {
    Serial.printf("[PORTAL] SSID='%s' code='%s' name='%s'\n", ssid, code ? code : "", device_name ? device_name : "");

    /* Save WiFi via the credential store (enroll = the first/primary network).
     * Adds to the persisted list (dedupe by SSID) and marks the store seeded so
     * hardcoded setWiFi() creds never override a portal-provisioned device. */
    if (ssid && ssid[0] != '\0') {
        pulsync_wifi_cred_t cur[PULSYNC_WIFI_MAX_CREDS];
        int n = pulsync_wifi_store_count();
        for (int i = 0; i < n; i++) pulsync_wifi_store_get(i, &cur[i]);
        /* Put the new network first (highest priority). */
        pulsync_wifi_cred_t next[PULSYNC_WIFI_MAX_CREDS];
        memset(next, 0, sizeof(next));
        strncpy(next[0].ssid, ssid, PULSYNC_WIFI_STORE_SSID_MAXLEN - 1);
        if (password) strncpy(next[0].password, password, PULSYNC_WIFI_STORE_PASS_MAXLEN - 1);
        int nc = 1;
        for (int i = 0; i < n && nc < PULSYNC_WIFI_MAX_CREDS; i++) {
            if (strcmp(cur[i].ssid, ssid) == 0) continue;  /* skip dup */
            next[nc++] = cur[i];
        }
        pulsync_wifi_store_set_all(next, nc);
        pulsync_wifi_store_save();
        pulsync_nvs_set_i32(PULSYNC_NVS_KEY_WIFI_SEEDED, 1);
        pulsync_wifi_store_apply();
    }

    /* Save server URL if provided */
    if (server && server[0] != '\0') {
        pulsync_nvs_set_str(PULSYNC_NVS_KEY_SERVER, server);
        strncpy(_server, server, sizeof(_server) - 1);
        /* Re-parse server URL for transport */
        char host[64]; uint16_t hp, mp; bool tls;
        pulsync_transport_parse_url(server, host, sizeof(host), &hp, &mp, &tls);
        pulsync_transport_set_server(host, hp, mp, tls);
    }

    /* Save pairing code if provided */
    if (code && code[0] != '\0') {
        pulsync_nvs_set_str("pair_code", code);
        strncpy(_pairing_code, code, sizeof(_pairing_code) - 1);
        pulsync_enroll_init(code);
    }

    /* Save device name */
    if (device_name && device_name[0] != '\0') {
        pulsync_nvs_set_str("dev_name", device_name);
    }

    /* WiFi profiles already applied via the store above. */

    /* Stop portal */
    pulsync_portal_stop();

    /* Reconnect WiFi */
    pulsync_wifi_disconnect();
    delay(500);
    pulsync_wifi_start();
}

void PulsyncClass::_handlePortalServer(const char *server) {
    if (!server || server[0] == '\0') return;
    Serial.printf("[PORTAL] Server set: '%s'\n", server);
    pulsync_nvs_set_str(PULSYNC_NVS_KEY_SERVER, server);
    strncpy(_server, server, sizeof(_server) - 1);
    _server[sizeof(_server) - 1] = '\0';
    _server_explicit = true;
    /* Re-parse and update transport target. */
    char host[64]; uint16_t hp, mp; bool tls;
    pulsync_transport_parse_url(server, host, sizeof(host), &hp, &mp, &tls);
    pulsync_transport_set_server(host, hp, mp, tls);
}

void PulsyncClass::_handlePortalRepair(const char *pairing_code) {
    if (!pairing_code || pairing_code[0] == '\0') return;
    Serial.printf("[PORTAL] Re-pair with code '%s' — clearing registration\n", pairing_code);

    /* Tear down the live link first so nothing races the token clear. */
    pulsync_heartbeat_stop();
    pulsync_transport_stop();
    _hb_started = false;

    /* Clear the current device token (server-specific) and the config-portal
     * password hash (belongs to the old pairing — a fresh one is issued on
     * re-enroll). The MQTT endpoint is re-provided by the enroll response. */
    pulsync_enroll_reset();                       /* erases dev_token, state → NEEDED */
    pulsync_nvs_erase_key(PULSYNC_NVS_KEY_CFG_PW_HASH);

    /* Persist and apply the new pairing code. */
    pulsync_nvs_set_str("pair_code", pairing_code);
    strncpy(_pairing_code, pairing_code, sizeof(_pairing_code) - 1);
    _pairing_code[sizeof(_pairing_code) - 1] = '\0';
    pulsync_enroll_init(_pairing_code);

    /* Re-enroll on the SAME server the device currently targets (re-pair moves
     * the device between projects/accounts, not servers). If we were on a
     * discovered server keep it (DISC_RESOLVED); enrollment runs again in loop()
     * because the state is now NEEDED, not DONE. An explicit/NVS server stays
     * OVERRIDE. Only reset to a fresh discovery cycle if we somehow have no
     * target resolved yet. */
    if (_disc_state != DISC_OVERRIDE && _disc_state != DISC_RESOLVED) {
        _disc_state = DISC_TRY_LOCAL;
    }

    /* Close the portal and reconnect WiFi; loop() will re-enroll with the new
     * code and receive a fresh token + config password. */
    pulsync_portal_stop();
    pulsync_wifi_disconnect();
    delay(400);
    pulsync_wifi_start();
}

/* Make _handleRx, _handleHeartbeatResponse, _handlePortalCredentials accessible */
/* They're called from C trampolines via the s_instance pointer */
