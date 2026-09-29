/**
 * pulsync_time — Time service (C API)
 *
 * Reserved for a future release. NTP and server-time fallback are not
 * available in the beta library.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PULSYNC_USE_TIME
#error "PULSYNC_USE_TIME is not implemented; remove this flag for the beta library"
#endif

#ifdef __cplusplus
}
#endif
