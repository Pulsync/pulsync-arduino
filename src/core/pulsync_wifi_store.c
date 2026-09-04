/**
 * pulsync_wifi_store — implementation
 */

#include "pulsync_wifi_store.h"
#include "pulsync_wifi.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "pulsync_wifi_store";

/* In-memory mirror of the persisted list. */
static pulsync_wifi_cred_t s_creds[PULSYNC_WIFI_MAX_CREDS];
static int s_count = 0;

/* Seed staging: creds added via seed_add before commit. */
static bool s_seed_dirty = false;

/* ---------- key helpers ---------- */

static void ssid_key(int i, char *buf, size_t n) {
    snprintf(buf, n, PULSYNC_NVS_KEY_WIFI_SSID_FMT, i);
}
static void pw_key(int i, char *buf, size_t n) {
    snprintf(buf, n, PULSYNC_NVS_KEY_WIFI_PW_FMT, i);
}

/* Find index of an SSID in the in-memory list, or -1. */
static int find_ssid(const char *ssid) {
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_creds[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

/* Append a cred to the in-memory list (dedupe by SSID: update password unless
 * the incoming password is empty, in which case keep the existing one). */
static bool mem_add(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') return false;
    int existing = find_ssid(ssid);
    if (existing >= 0) {
        if (password && password[0] != '\0') {
            strncpy(s_creds[existing].password, password,
                    PULSYNC_WIFI_STORE_PASS_MAXLEN - 1);
            s_creds[existing].password[PULSYNC_WIFI_STORE_PASS_MAXLEN - 1] = '\0';
        }
        return true;
    }
    if (s_count >= PULSYNC_WIFI_MAX_CREDS) return false;

    strncpy(s_creds[s_count].ssid, ssid, PULSYNC_WIFI_STORE_SSID_MAXLEN - 1);
    s_creds[s_count].ssid[PULSYNC_WIFI_STORE_SSID_MAXLEN - 1] = '\0';
    if (password) {
        strncpy(s_creds[s_count].password, password,
                PULSYNC_WIFI_STORE_PASS_MAXLEN - 1);
        s_creds[s_count].password[PULSYNC_WIFI_STORE_PASS_MAXLEN - 1] = '\0';
    } else {
        s_creds[s_count].password[0] = '\0';
    }
    s_count++;
    return true;
}

/* ---------- load / migrate ---------- */

int pulsync_wifi_store_load(void) {
    s_count = 0;
    memset(s_creds, 0, sizeof(s_creds));

    int32_t cnt = 0;
    bool have_list = pulsync_nvs_get_i32(PULSYNC_NVS_KEY_WIFI_COUNT, &cnt);

    if (have_list && cnt > 0) {
        if (cnt > PULSYNC_WIFI_MAX_CREDS) cnt = PULSYNC_WIFI_MAX_CREDS;
        for (int i = 0; i < cnt; i++) {
            char kssid[24], kpw[24];
            ssid_key(i, kssid, sizeof(kssid));
            pw_key(i, kpw, sizeof(kpw));
            char ssid[PULSYNC_WIFI_STORE_SSID_MAXLEN] = {0};
            char pw[PULSYNC_WIFI_STORE_PASS_MAXLEN] = {0};
            if (pulsync_nvs_get_str(kssid, ssid, sizeof(ssid)) && ssid[0] != '\0') {
                pulsync_nvs_get_str(kpw, pw, sizeof(pw));
                mem_add(ssid, pw);
            }
        }
        ESP_LOGI(TAG, "Loaded %d WiFi cred(s) from NVS", s_count);
        return s_count;
    }

    /* No indexed list. Try migrating a legacy single profile. */
    char lssid[PULSYNC_WIFI_STORE_SSID_MAXLEN] = {0};
    char lpw[PULSYNC_WIFI_STORE_PASS_MAXLEN] = {0};
    if (pulsync_nvs_get_str(PULSYNC_NVS_KEY_WIFI0_LEGACY, lssid, sizeof(lssid)) &&
        lssid[0] != '\0') {
        pulsync_nvs_get_str(PULSYNC_NVS_KEY_WIFI0_PW_LEGACY, lpw, sizeof(lpw));
        mem_add(lssid, lpw);
        ESP_LOGI(TAG, "Migrated legacy WiFi profile '%s'", lssid);
        /* Persist as indexed list + mark seeded so we never re-migrate/seed. */
        pulsync_wifi_store_save();
        pulsync_nvs_set_i32(PULSYNC_NVS_KEY_WIFI_SEEDED, 1);
        /* Best-effort cleanup of legacy keys. */
        pulsync_nvs_erase_key(PULSYNC_NVS_KEY_WIFI0_LEGACY);
        pulsync_nvs_erase_key(PULSYNC_NVS_KEY_WIFI0_PW_LEGACY);
    }

    return s_count;
}

bool pulsync_wifi_store_is_seeded(void) {
    int32_t seeded = 0;
    return pulsync_nvs_get_i32(PULSYNC_NVS_KEY_WIFI_SEEDED, &seeded) && seeded != 0;
}

/* ---------- seeding (first boot from setWiFi) ---------- */

bool pulsync_wifi_store_seed_add(const char *ssid, const char *password) {
    if (pulsync_wifi_store_is_seeded()) return false;
    bool ok = mem_add(ssid, password);
    if (ok) s_seed_dirty = true;
    return ok;
}

void pulsync_wifi_store_commit_seed(void) {
    if (pulsync_wifi_store_is_seeded()) return;
    if (!s_seed_dirty) return;
    pulsync_wifi_store_save();
    pulsync_nvs_set_i32(PULSYNC_NVS_KEY_WIFI_SEEDED, 1);
    s_seed_dirty = false;
    ESP_LOGI(TAG, "Seeded %d WiFi cred(s) into NVS (first boot)", s_count);
}

/* ---------- accessors ---------- */

int pulsync_wifi_store_count(void) { return s_count; }

bool pulsync_wifi_store_get(int index, pulsync_wifi_cred_t *out) {
    if (index < 0 || index >= s_count || !out) return false;
    *out = s_creds[index];
    return true;
}

/* ---------- portal "set all" ---------- */

int pulsync_wifi_store_set_all(const pulsync_wifi_cred_t *creds, int count) {
    /* Snapshot old list so we can preserve passwords for kept SSIDs. */
    pulsync_wifi_cred_t old[PULSYNC_WIFI_MAX_CREDS];
    int old_count = s_count;
    memcpy(old, s_creds, sizeof(old));

    s_count = 0;
    memset(s_creds, 0, sizeof(s_creds));

    if (!creds) return 0;
    if (count > PULSYNC_WIFI_MAX_CREDS) count = PULSYNC_WIFI_MAX_CREDS;

    for (int i = 0; i < count; i++) {
        const char *ssid = creds[i].ssid;
        if (!ssid || ssid[0] == '\0') continue;
        const char *pw = creds[i].password;
        if (!pw || pw[0] == '\0') {
            /* Keep previous password for this SSID if we had one. */
            for (int j = 0; j < old_count; j++) {
                if (strcmp(old[j].ssid, ssid) == 0) {
                    pw = old[j].password;
                    break;
                }
            }
        }
        mem_add(ssid, pw);
    }
    return s_count;
}

/* ---------- persist ---------- */

bool pulsync_wifi_store_save(void) {
    /* Write current slots. */
    for (int i = 0; i < s_count; i++) {
        char kssid[24], kpw[24];
        ssid_key(i, kssid, sizeof(kssid));
        pw_key(i, kpw, sizeof(kpw));
        pulsync_nvs_set_str(kssid, s_creds[i].ssid);
        pulsync_nvs_set_str(kpw, s_creds[i].password);
    }
    /* Erase any trailing stale slots from a previously-longer list. */
    for (int i = s_count; i < PULSYNC_WIFI_MAX_CREDS; i++) {
        char kssid[24], kpw[24];
        ssid_key(i, kssid, sizeof(kssid));
        pw_key(i, kpw, sizeof(kpw));
        pulsync_nvs_erase_key(kssid);
        pulsync_nvs_erase_key(kpw);
    }
    pulsync_nvs_set_i32(PULSYNC_NVS_KEY_WIFI_COUNT, s_count);
    ESP_LOGI(TAG, "Saved %d WiFi cred(s) to NVS", s_count);
    return true;
}

/* ---------- apply to WiFi manager ---------- */

void pulsync_wifi_store_apply(void) {
    pulsync_wifi_clear_profiles();
    for (int i = 0; i < s_count; i++) {
        pulsync_wifi_add_profile(i, s_creds[i].ssid, s_creds[i].password);
    }
    ESP_LOGI(TAG, "Applied %d WiFi cred(s) to manager", s_count);
}

void pulsync_wifi_store_clear(void) {
    s_count = 0;
    memset(s_creds, 0, sizeof(s_creds));
}
