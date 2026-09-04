/**
 * pulsync_version — single source of truth for the device firmware version.
 *
 * Resolution order (highest priority first):
 *   1. Runtime override set via pulsync_version_set() (e.g. from
 *      Pulsync.setVersion() in user code). Must be set before enrollment /
 *      first heartbeat to be reported.
 *   2. The PULSYNC_FW_VERSION compile-time flag (recommended for PlatformIO:
 *      -D PULSYNC_FW_VERSION=\"1.2.3\").
 *   3. "0.0.0" fallback — OTA version comparisons will not work until a real
 *      version is set.
 *
 * IMPORTANT: whatever version is reported MUST match the .bin that is actually
 * flashed. OTA compares "what I'm running" against "what's offered"; a mismatch
 * makes that logic lie.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set a runtime version override. Copied into an internal buffer
 * (truncated to 31 chars). Call before pulsync enrollment/heartbeat start.
 * Passing NULL or an empty string clears the override.
 */
void pulsync_version_set(const char *version);

/**
 * Get the effective firmware version string (never NULL).
 * Returns the runtime override if set, else PULSYNC_FW_VERSION, else "0.0.0".
 */
const char *pulsync_version_get(void);

#ifdef __cplusplus
}
#endif
