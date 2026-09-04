/**
 * pulsync_enroll — Device enrollment via pairing code (C API)
 *
 * Flow:
 *   1. Check NVS for existing device token
 *   2. If token exists → enrollment done, start transport
 *   3. If no token → POST pairing code to /api/device/enroll
 *   4. Server responds with device token → store in NVS
 *   5. Subsequent boots skip enrollment (token already in NVS)
 *   6. NVS wipe → re-enrolls automatically using stored pairing code
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PULSYNC_PAIRING_CODE_MAXLEN  16   /* e.g., "PUL-A7K9X2" + null */

/** Enrollment state */
typedef enum {
    PULSYNC_ENROLL_IDLE = 0,
    PULSYNC_ENROLL_NEEDED,        /* no token in NVS, need to enroll */
    PULSYNC_ENROLL_IN_PROGRESS,   /* enrollment request sent, awaiting response */
    PULSYNC_ENROLL_DONE,          /* token acquired, stored in NVS */
    PULSYNC_ENROLL_FAILED         /* enrollment failed (invalid code, server error) */
} pulsync_enroll_state_t;

/** Callback on enrollment completion (success or failure) */
typedef void (*pulsync_enroll_cb_t)(bool success, const char *token);

/**
 * Initialize enrollment module.
 * @param pairing_code  The pairing code from the portal (e.g., "PUL-A7K9X2")
 */
void pulsync_enroll_init(const char *pairing_code);

/**
 * Check if device is already enrolled (token exists in NVS).
 * If enrolled, populates token into the transport layer.
 * @return true if already enrolled
 */
bool pulsync_enroll_check(void);

/**
 * Start the enrollment process (sends pairing code to server).
 * Non-blocking — uses transport layer.
 * Result arrives via the on_complete callback or the transport rx handler.
 */
void pulsync_enroll_start(void);

/**
 * Process enrollment response from server.
 * Called internally by the transport rx handler.
 * @param payload  JSON response from /api/device/enroll
 * @param len      Length of payload
 */
void pulsync_enroll_handle_response(const char *payload, size_t len);

/**
 * Get current enrollment state.
 */
pulsync_enroll_state_t pulsync_enroll_get_state(void);

/**
 * Get the device token (only valid after PULSYNC_ENROLL_DONE).
 * @param out     Buffer to write token to
 * @param maxlen  Buffer size
 * @return true if token is available
 */
bool pulsync_enroll_get_token(char *out, size_t maxlen);

/**
 * Register callback for enrollment completion.
 */
void pulsync_enroll_on_complete(pulsync_enroll_cb_t cb);

/**
 * Force re-enrollment (erases stored token from NVS).
 */
void pulsync_enroll_reset(void);

#ifdef __cplusplus
}
#endif
