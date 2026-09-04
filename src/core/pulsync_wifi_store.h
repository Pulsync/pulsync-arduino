/**
 * pulsync_wifi_store — persistent, ordered WiFi credential list (C API)
 *
 * Owns up to PULSYNC_WIFI_MAX_CREDS credentials in NVS. Order = connection
 * priority (index 0 tried first). NVS is the source of truth once seeded.
 *
 * Responsibilities:
 *   - Load/save the credential list to/from NVS (indexed keys).
 *   - Seed-once from hardcoded setWiFi() creds on very first boot.
 *   - Apply the in-memory list to the pulsync_wifi manager (profiles).
 *   - Mutations used by the captive portal: replace the whole list at once
 *     (list built client-side: add / remove / reorder), dedupe by SSID.
 *
 * This module holds an in-memory mirror of the list; call the mutators, then
 * pulsync_wifi_store_save() to persist and pulsync_wifi_store_apply() to push
 * into the WiFi manager.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "pulsync_nvs.h"  /* PULSYNC_WIFI_MAX_CREDS */

#ifdef __cplusplus
extern "C" {
#endif

#define PULSYNC_WIFI_STORE_SSID_MAXLEN  33  /* 32 + null */
#define PULSYNC_WIFI_STORE_PASS_MAXLEN  65  /* 64 + null */

typedef struct {
    char ssid[PULSYNC_WIFI_STORE_SSID_MAXLEN];
    char password[PULSYNC_WIFI_STORE_PASS_MAXLEN];
} pulsync_wifi_cred_t;

/**
 * Load the credential list from NVS into the in-memory mirror.
 * Migrates a legacy single "wifi0"/"wifi0_pw" profile if present and no
 * indexed list exists yet. Safe to call multiple times.
 * @return number of credentials loaded (0..MAX)
 */
int pulsync_wifi_store_load(void);

/**
 * Has the store ever been seeded (first-boot import done)?
 * When true, hardcoded setWiFi() creds are ignored.
 */
bool pulsync_wifi_store_is_seeded(void);

/**
 * Seed the store from a hardcoded credential (called once per setWiFi()).
 * No-op if already seeded. Appends (dedupe by SSID), does NOT persist or set
 * the seeded flag — call pulsync_wifi_store_commit_seed() after seeding all.
 * @return true if the cred was added to the in-memory list
 */
bool pulsync_wifi_store_seed_add(const char *ssid, const char *password);

/**
 * Persist the seeded list to NVS and mark the store as seeded. No-op if the
 * store was already seeded or if nothing was seeded.
 */
void pulsync_wifi_store_commit_seed(void);

/** Number of credentials currently in the in-memory list. */
int pulsync_wifi_store_count(void);

/**
 * Read credential at index. Returns false if out of range.
 * (Callers that only need the SSID can ignore the password field.)
 */
bool pulsync_wifi_store_get(int index, pulsync_wifi_cred_t *out);

/**
 * Replace the entire in-memory list with the given creds (used by the portal
 * "Save" for the WiFi section). Dedupes by SSID (last write wins for a repeat
 * SSID), skips empty SSIDs, and caps at PULSYNC_WIFI_MAX_CREDS. A cred whose
 * password is NULL or empty keeps the previously-stored password for that
 * SSID (so the UI need not resend secrets).
 * Does NOT persist — call pulsync_wifi_store_save().
 * @return resulting count
 */
int pulsync_wifi_store_set_all(const pulsync_wifi_cred_t *creds, int count);

/** Persist the current in-memory list to NVS. @return true on success. */
bool pulsync_wifi_store_save(void);

/**
 * Push the current in-memory list into the pulsync_wifi manager (clears and
 * re-adds profiles in order). Does not trigger a (re)connect by itself.
 */
void pulsync_wifi_store_apply(void);

/** Clear the in-memory list (does not touch NVS). */
void pulsync_wifi_store_clear(void);

#ifdef __cplusplus
}
#endif
