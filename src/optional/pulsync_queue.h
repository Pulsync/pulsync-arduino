/**
 * pulsync_queue — Offline persistent queue (C API)
 *
 * Reserved for a future release. Persistent SPIFFS/LittleFS queuing is not
 * available in the beta library. The built-in transport queue remains an
 * in-RAM, reconnect-only buffer.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PULSYNC_USE_QUEUE
#error "PULSYNC_USE_QUEUE is not implemented; remove this flag for the beta library"
#endif

#ifdef __cplusplus
}
#endif
