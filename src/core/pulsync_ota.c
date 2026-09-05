/**
 * pulsync_ota — OTA implementation
 *
 * Uses ESP-IDF esp_https_ota for secure firmware download.
 * Spawns a one-shot FreeRTOS task for download to avoid blocking.
 * Supports TLS via cert bundle and non-TLS for local dev servers.
 */

#include "pulsync_ota.h"
#include "pulsync_transport.h"
#include "pulsync_scheduler.h"
#include "pulsync_version.h"
#include "pulsync_enroll.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

/* ---------- Server-to-device OTA authentication ---------- */
/* The device authenticates the SERVER's OTA offer using a key derived from its
 * enrollment token (HKDF-SHA256, info "pulsync-ota-v1"). It then HMAC-verifies
 * the offer and, after download, checks the image's sha256 against the offer's
 * checksum. A MITM without the token cannot forge a valid signature. Must stay
 * byte-for-byte compatible with server/src/core/device-signing.js. */

#define PULSYNC_OTA_SIG_ALG   "hmac-sha256-v1"
#define PULSYNC_OTA_HKDF_INFO "pulsync-ota-v1"

/* Set to 1 at build time to allow OTA offers that carry no/invalid signature
 * (dev only). Default: fail closed — unsigned/invalid offers are rejected. */
#ifndef PULSYNC_OTA_ALLOW_UNSIGNED
#define PULSYNC_OTA_ALLOW_UNSIGNED 0
#endif

/* HKDF-SHA256 (RFC 5869) implemented from HMAC, since the prebuilt
 * arduino-esp32 mbedTLS excludes mbedtls_hkdf() but includes mbedtls_md_hmac().
 * Empty salt + single info; 32-byte output = one HMAC block, so Expand is one
 * iteration. Matches node:crypto hkdfSync('sha256', ikm, '', info, 32). */
static bool ota_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                            const char *info, uint8_t out_key[32]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return false;

    /* Extract: PRK = HMAC(salt=zeros(32), IKM). RFC 5869: empty salt -> a string
     * of HashLen zero bytes. */
    uint8_t zero_salt[32] = {0};
    uint8_t prk[32];
    if (mbedtls_md_hmac(md, zero_salt, sizeof(zero_salt), ikm, ikm_len, prk) != 0) {
        return false;
    }

    /* Expand: T(1) = HMAC(PRK, info || 0x01); OKM = first 32 bytes = T(1). */
    size_t info_len = strlen(info);
    uint8_t t_input[64];
    if (info_len + 1 > sizeof(t_input)) { memset(prk, 0, sizeof(prk)); return false; }
    memcpy(t_input, info, info_len);
    t_input[info_len] = 0x01;

    int rc = mbedtls_md_hmac(md, prk, sizeof(prk), t_input, info_len + 1, out_key);
    memset(prk, 0, sizeof(prk));
    return rc == 0;
}

/* Derive the 32-byte per-device signing key from the enrollment token. */
static bool ota_derive_key(uint8_t out_key[32]) {
    char token[128] = {0};
    if (!pulsync_enroll_get_token(token, sizeof(token)) || token[0] == '\0') {
        return false;
    }
    bool ok = ota_hkdf_sha256((const uint8_t *)token, strlen(token),
                              PULSYNC_OTA_HKDF_INFO, out_key);
    memset(token, 0, sizeof(token));  /* wipe token copy */
    return ok;
}

/* HMAC-SHA256(key, msg) -> hex string (65 bytes incl null). */
static bool ota_hmac_hex(const uint8_t key[32], const char *msg, size_t msg_len,
                         char out_hex[65]) {
    uint8_t mac[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return false;
    if (mbedtls_md_hmac(md, key, 32, (const uint8_t *)msg, msg_len, mac) != 0) {
        return false;
    }
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2]     = hexd[(mac[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hexd[mac[i] & 0xF];
    }
    out_hex[64] = '\0';
    return true;
}

/* Constant-time compare of two equal-length NUL-terminated hex strings. */
static bool ota_hex_equal_ct(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* Cert bundle attach symbol — see pulsync_transport.c for the rationale.
 * arduino-esp32 2.x (IDF 4.x) only has arduino_esp_crt_bundle_attach; from
 * arduino-esp32 3.x (IDF 5.x) the standard esp_crt_bundle_attach is used. */
#if defined(ARDUINO) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
esp_err_t arduino_esp_crt_bundle_attach(void *conf);
#define PULSYNC_CRT_BUNDLE_ATTACH arduino_esp_crt_bundle_attach
#else
#include "esp_crt_bundle.h"
#define PULSYNC_CRT_BUNDLE_ATTACH esp_crt_bundle_attach
#endif

static const char *TAG = "pulsync_ota";

#define OTA_TASK_STACK_SIZE  8192
#define OTA_BUF_SIZE         1024

/* ---------- State ---------- */

static pulsync_ota_state_t s_state = PULSYNC_OTA_IDLE;
static pulsync_ota_info_t s_pending_info = {0};
static pulsync_ota_progress_cb_t s_progress_cb = NULL;
static pulsync_ota_complete_cb_t s_complete_cb = NULL;
static TaskHandle_t s_ota_task = NULL;

/* Set in pulsync_ota_init(): true only if the flashed partition table has a
 * second app slot (ota_0/ota_1 + otadata). Without it, OTA cannot work and we
 * skip it entirely rather than failing mid-download. */
static bool s_ota_available = false;

/* ---------- Semver comparison ---------- */

#define PULSYNC_VER_MAX_PARTS 4  /* major.minor.patch + one label counter */

/**
 * Extract up to PULSYNC_VER_MAX_PARTS integers from a version string, in the
 * order they appear. Non-digit characters act as separators, so the label
 * counter in "0.1.1-dev3" is picked up as the 4th part -> {0,1,1,3}.
 * Returns the number of parts extracted (0..PULSYNC_VER_MAX_PARTS).
 */
static int version_extract(const char *ver, int parts[PULSYNC_VER_MAX_PARTS]) {
    int count = 0;
    if (!ver) return 0;

    const char *p = ver;
    while (*p && count < PULSYNC_VER_MAX_PARTS) {
        if (*p >= '0' && *p <= '9') {
            int val = 0;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                p++;
            }
            parts[count++] = val;
        } else {
            p++;
        }
    }
    return count;
}

/**
 * Compare two version strings by their numeric components (numbers only; any
 * text between numbers is treated as a separator). Comparison is per-component,
 * left to right; on a tie the version with MORE components is greater. This
 * yields the intended ordering:
 *   0.1.1 < 0.1.1-dev1 < 0.1.1-dev2 < 0.1.1-dev3
 *   0.1.2 > 0.1.1-dev9   (a real patch bump beats any dev of the prior patch)
 *   0.1.1-rc10 > 0.1.1-rc2   (numeric, not lexical)
 * Returns: >0 if a > b, <0 if a < b, 0 if equal.
 */
static int semver_compare(const char *a, const char *b) {
    int pa[PULSYNC_VER_MAX_PARTS] = {0};
    int pb[PULSYNC_VER_MAX_PARTS] = {0};
    int na = version_extract(a, pa);
    int nb = version_extract(b, pb);

    int n = (na < nb) ? na : nb;
    for (int i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    /* Equal on the shared components -> the one with more components wins. */
    return na - nb;
}

/* ---------- JSON helpers ---------- */

static size_t json_get_string(const char *json, size_t len, const char *key,
                              char *out, size_t out_maxlen) {
    if (!json || !key || !out || out_maxlen == 0) return 0;

    size_t key_len = strlen(key);
    const char *pos = json;
    const char *end = json + len;

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

/* Extract a JSON number value for "key" (unquoted). Returns true if found. */
static bool json_get_number(const char *json, size_t len, const char *key,
                            uint32_t *out) {
    if (!json || !key || !out) return false;
    size_t key_len = strlen(key);
    const char *pos = json;
    const char *end = json + len;

    while (pos < end) {
        const char *found = strstr(pos, key);
        if (!found || found >= end) return false;
        if (found > json && *(found - 1) == '"') {
            const char *after_key = found + key_len;
            if (after_key < end && *after_key == '"') {
                after_key++;
                while (after_key < end && (*after_key == ':' || *after_key == ' ')) after_key++;
                if (after_key < end && *after_key >= '0' && *after_key <= '9') {
                    uint32_t v = 0;
                    while (after_key < end && *after_key >= '0' && *after_key <= '9') {
                        v = v * 10u + (uint32_t)(*after_key - '0');
                        after_key++;
                    }
                    *out = v;
                    return true;
                }
            }
        }
        pos = found + 1;
    }
    return false;
}

/**
 * Verify the server's OTA offer signature. Reconstructs the canonical string
 * (version \n url \n checksum \n size) exactly as device-signing.js does,
 * derives the key from the enrollment token, computes HMAC-SHA256, and compares
 * against the provided signature in constant time.
 * @return true if the offer is authentic (or unsigned + allowed by build flag).
 */
static bool ota_verify_offer(const char *version, const char *url,
                             const char *checksum, uint32_t size,
                             const char *sig_alg, const char *sig) {
    if (sig[0] == '\0' || sig_alg[0] == '\0') {
#if PULSYNC_OTA_ALLOW_UNSIGNED
        ESP_LOGW(TAG, "OTA offer is unsigned; accepting (PULSYNC_OTA_ALLOW_UNSIGNED).");
        return true;
#else
        ESP_LOGE(TAG, "OTA offer is unsigned; rejecting (fail closed).");
        return false;
#endif
    }
    if (strcmp(sig_alg, PULSYNC_OTA_SIG_ALG) != 0) {
        ESP_LOGE(TAG, "OTA offer sig_alg '%s' unsupported; rejecting.", sig_alg);
        return false;
    }

    uint8_t key[32];
    if (!ota_derive_key(key)) {
        ESP_LOGE(TAG, "OTA: cannot derive signing key (no token?); rejecting.");
        return false;
    }

    /* Canonical string: version \n url \n checksum \n size */
    char canonical[512];
    int n = snprintf(canonical, sizeof(canonical), "%s\n%s\n%s\n%u",
                     version, url, checksum, (unsigned)size);
    if (n <= 0 || (size_t)n >= sizeof(canonical)) {
        ESP_LOGE(TAG, "OTA: canonical string too long; rejecting.");
        memset(key, 0, sizeof(key));
        return false;
    }

    char expected[65];
    bool ok = ota_hmac_hex(key, canonical, (size_t)n, expected);
    memset(key, 0, sizeof(key));
    if (!ok) return false;

    if (!ota_hex_equal_ct(expected, sig)) {
        ESP_LOGE(TAG, "OTA offer signature mismatch; rejecting (possible MITM).");
        return false;
    }
    ESP_LOGI(TAG, "OTA offer signature verified.");
    return true;
}

/* Compute the sha256 (hex) of the first `img_len` bytes of a partition. */
static bool ota_partition_sha256_hex(const esp_partition_t *part, size_t img_len,
                                     char out_hex[65]) {
    if (!part || img_len == 0) return false;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  /* 0 = SHA-256 (not SHA-224) */

    uint8_t buf[512];
    size_t off = 0;
    bool ok = true;
    while (off < img_len) {
        size_t chunk = img_len - off;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        if (esp_partition_read(part, off, buf, chunk) != ESP_OK) { ok = false; break; }
        mbedtls_sha256_update(&ctx, buf, chunk);
        off += chunk;
    }
    uint8_t digest[32];
    if (ok) mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    if (!ok) return false;

    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2]     = hexd[(digest[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hexd[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
    return true;
}

/* ---------- OTA download task ---------- */

/* Attach the device token so the server can authorize the firmware download.
 * Fires right after esp_https_ota creates the http client, before the request.
 * The download endpoint (/fw-bin) is gated by the same X-Device-Token the
 * device uses everywhere else; if the server isn't gating it, the extra header
 * is simply ignored. */
static esp_err_t ota_http_client_init_cb(esp_http_client_handle_t client) {
    char token[128] = {0};
    if (pulsync_enroll_get_token(token, sizeof(token)) && token[0] != '\0') {
        esp_http_client_set_header(client, "X-Device-Token", token);
        memset(token, 0, sizeof(token));
    }
    return ESP_OK;
}

static void ota_download_task(void *param) {
    ESP_LOGI(TAG, "OTA download starting: %s (v%s)", s_pending_info.url, s_pending_info.version);
    s_state = PULSYNC_OTA_DOWNLOADING;

    /* Determine if URL uses TLS */
    bool use_tls = (strncmp(s_pending_info.url, "https://", 8) == 0);

    esp_http_client_config_t http_cfg = {
        .url = s_pending_info.url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size = OTA_BUF_SIZE,
    };

    /* Always attach the cert bundle. For https:// URLs it enables TLS server
     * verification. For plain http:// URLs the bundle is never used (transport
     * is TCP), but esp_https_ota_begin() unconditionally requires one of
     * cert_pem / use_global_ca_store / crt_bundle_attach to be set — otherwise
     * it fails with ESP_ERR_INVALID_ARG ("No option for server verification").
     * The precompiled arduino-esp32 SDK has CONFIG_OTA_ALLOW_HTTP disabled, so
     * this is the only way to allow LAN/self-hosted plain-HTTP OTA. */
    http_cfg.crt_bundle_attach = PULSYNC_CRT_BUNDLE_ATTACH;
    (void)use_tls;

    esp_https_ota_config_t ota_config = {
        .http_config = &http_cfg,
        .http_client_init_cb = ota_http_client_init_cb,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        s_state = PULSYNC_OTA_FAILED;
        if (s_complete_cb) s_complete_cb(false, s_pending_info.version);
        goto done;
    }

    /* Get image size for progress reporting */
    int total_size = esp_https_ota_get_image_size(ota_handle);
    int downloaded = 0;

    /* Download loop */
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            downloaded = esp_https_ota_get_image_len_read(ota_handle);
            if (s_progress_cb && total_size > 0) {
                s_progress_cb((uint32_t)downloaded, (uint32_t)total_size);
            }
            continue;
        }
        break;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        s_state = PULSYNC_OTA_FAILED;
        if (s_complete_cb) s_complete_cb(false, s_pending_info.version);
        goto done;
    }

    /* Verify and finish */
    s_state = PULSYNC_OTA_VERIFYING;
    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        s_state = PULSYNC_OTA_FAILED;
        if (s_complete_cb) s_complete_cb(false, s_pending_info.version);
        goto done;
    }

    /* Post-download integrity check: verify the written image's sha256 against
     * the (signed) checksum from the offer. esp_https_ota_finish already
     * validated the ESP image FORMAT; this additionally confirms the bytes are
     * exactly the firmware the server authenticated. If it doesn't match, undo
     * the boot selection so the device stays on the current firmware. */
    if (s_pending_info.checksum[0] != '\0' && s_pending_info.size > 0) {
        const esp_partition_t *updated = esp_ota_get_boot_partition();
        char actual[65];
        if (!ota_partition_sha256_hex(updated, s_pending_info.size, actual)) {
            ESP_LOGE(TAG, "OTA: could not hash written image; aborting.");
            esp_ota_set_boot_partition(esp_ota_get_running_partition());
            s_state = PULSYNC_OTA_FAILED;
            if (s_complete_cb) s_complete_cb(false, s_pending_info.version);
            goto done;
        }
        if (!ota_hex_equal_ct(actual, s_pending_info.checksum)) {
            ESP_LOGE(TAG, "OTA: image checksum mismatch! expected %s got %s. Aborting, not rebooting.",
                     s_pending_info.checksum, actual);
            esp_ota_set_boot_partition(esp_ota_get_running_partition());
            s_state = PULSYNC_OTA_FAILED;
            if (s_complete_cb) s_complete_cb(false, s_pending_info.version);
            goto done;
        }
        ESP_LOGI(TAG, "OTA image checksum verified.");
    }

    ESP_LOGI(TAG, "OTA successful! Version: %s", s_pending_info.version);
    s_state = PULSYNC_OTA_READY;
    if (s_complete_cb) s_complete_cb(true, s_pending_info.version);

    /* Auto-reboot after short delay */
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Rebooting to apply OTA update...");
    esp_restart();

done:
    s_ota_task = NULL;
    vTaskDelete(NULL);
}

/* ---------- Public API ---------- */

void pulsync_ota_init(void) {
    s_state = PULSYNC_OTA_IDLE;

    /* Detect whether OTA is even possible on this partition layout.
     * esp_ota_get_next_update_partition() returns NULL when the flashed
     * partition table has only a single app slot (the ESP32 default). OTA
     * needs two app slots (ota_0/ota_1) plus an otadata partition. */
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    s_ota_available = (next != NULL);

    if (!s_ota_available) {
        ESP_LOGE(TAG, "OTA DISABLED: no second app partition found.");
        ESP_LOGE(TAG, "Your firmware was flashed with a single-app partition table,");
        ESP_LOGE(TAG, "so over-the-air updates cannot be applied. Fix it by selecting");
        ESP_LOGE(TAG, "a partition table with two app slots + otadata, e.g. in");
        ESP_LOGE(TAG, "platformio.ini add:  board_build.partitions = min_spiffs.csv");
        ESP_LOGE(TAG, "See https://pulsync.in/docs/ota for details.");
    }

    /* Confirm running partition if we booted after an OTA */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Confirming OTA partition (first boot after update)");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    ESP_LOGI(TAG, "OTA module initialized (current: %s, ota: %s)",
             pulsync_version_get(),
             s_ota_available ? "ready" : "unavailable");
}

bool pulsync_ota_is_available(void) {
    return s_ota_available;
}

bool pulsync_ota_should_update(const char *version) {
    if (!version || version[0] == '\0') return false;

    const char *current = pulsync_version_get();

    int cmp = semver_compare(version, current);
    if (cmp > 0) {
        ESP_LOGI(TAG, "Update available: %s → %s", current, version);
        return true;
    }
    ESP_LOGD(TAG, "No update needed (offered: %s, current: %s)", version, current);
    return false;
}

bool pulsync_ota_start(const pulsync_ota_info_t *info) {
    if (!info || info->url[0] == '\0') return false;

    if (!s_ota_available) {
        ESP_LOGW(TAG, "Ignoring OTA offer (v%s): no OTA partition on this device. "
                      "Flash with a two-app partition table (e.g. min_spiffs.csv).",
                 info->version);
        return false;
    }

    if (s_state == PULSYNC_OTA_DOWNLOADING || s_state == PULSYNC_OTA_VERIFYING) {
        ESP_LOGW(TAG, "OTA already in progress");
        return false;
    }

    memcpy(&s_pending_info, info, sizeof(pulsync_ota_info_t));
    s_state = PULSYNC_OTA_AVAILABLE;

    /* Spawn OTA task */
    BaseType_t ret = xTaskCreate(ota_download_task, "pulsync_ota",
                                 OTA_TASK_STACK_SIZE, NULL, 4, &s_ota_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        s_state = PULSYNC_OTA_FAILED;
        return false;
    }

    return true;
}

void pulsync_ota_handle_trigger(const char *json, size_t json_len) {
    if (!json || json_len == 0) return;

    pulsync_ota_info_t info = {0};

    /* Extract OTA fields from the "ota" object in heartbeat response */
    json_get_string(json, json_len, "ota_url", info.url, sizeof(info.url));
    if (info.url[0] == '\0') {
        /* Try alternate field name */
        json_get_string(json, json_len, "url", info.url, sizeof(info.url));
    }

    json_get_string(json, json_len, "ota_version", info.version, sizeof(info.version));
    if (info.version[0] == '\0') {
        json_get_string(json, json_len, "version", info.version, sizeof(info.version));
    }

    if (info.url[0] == '\0' || info.version[0] == '\0') {
        ESP_LOGW(TAG, "OTA trigger missing url or version");
        return;
    }

    /* Authenticity fields */
    json_get_string(json, json_len, "checksum", info.checksum, sizeof(info.checksum));
    uint32_t size = 0;
    json_get_number(json, json_len, "size", &size);
    info.size = size;

    char sig[65] = {0};
    char sig_alg[24] = {0};
    json_get_string(json, json_len, "sig", sig, sizeof(sig));
    json_get_string(json, json_len, "sig_alg", sig_alg, sizeof(sig_alg));

    /* Authenticate the SERVER before doing anything else. Fail closed. */
    if (!ota_verify_offer(info.version, info.url, info.checksum, info.size, sig_alg, sig)) {
        ESP_LOGE(TAG, "OTA offer rejected (not authenticated). Ignoring.");
        return;
    }

    /* Check if we should update */
    if (!pulsync_ota_should_update(info.version)) {
        return;
    }

    /* Start OTA (info.checksum is verified after download) */
    pulsync_ota_start(&info);
}

pulsync_ota_state_t pulsync_ota_get_state(void) {
    return s_state;
}

void pulsync_ota_on_progress(pulsync_ota_progress_cb_t cb) {
    s_progress_cb = cb;
}

void pulsync_ota_on_complete(pulsync_ota_complete_cb_t cb) {
    s_complete_cb = cb;
}

void pulsync_ota_reboot(void) {
    if (s_state == PULSYNC_OTA_READY) {
        ESP_LOGI(TAG, "Manual reboot requested for OTA");
        esp_restart();
    }
}

void pulsync_ota_confirm(void) {
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "OTA partition confirmed as valid");
}

void pulsync_ota_rollback(void) {
    ESP_LOGW(TAG, "Rolling back to previous firmware");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}
