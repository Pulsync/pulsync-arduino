/**
 * pulsync_scheduler — Non-blocking task scheduler (C API)
 *
 * millis()-based timing. Supports setTimeout, setInterval, clearTimeout,
 * clearInterval. Max tasks configurable via PULSYNC_MAX_TASKS.
 *
 * Thread safety: All functions must be called from the same context (loop task).
 * The scheduler does NOT use mutexes — it runs in user context only.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PULSYNC_MAX_TASKS
#define PULSYNC_MAX_TASKS 32
#endif

/** Task handle (0 = invalid) */
typedef uint16_t pulsync_task_id_t;

/** Callback function pointer for C API */
typedef void (*pulsync_task_cb_t)(void);

/** Callback with user data */
typedef void (*pulsync_task_cb_arg_t)(void *arg);

/**
 * Initialize the scheduler. Call once before any other scheduler function.
 */
void pulsync_scheduler_init(void);

/**
 * Process pending tasks. Call from loop() — runs ready callbacks.
 * Returns number of tasks executed this tick.
 */
int pulsync_scheduler_tick(void);

/**
 * Schedule a one-shot callback after delay_ms milliseconds.
 * Returns task ID (>0) on success, 0 if scheduler is full.
 */
pulsync_task_id_t pulsync_set_timeout(pulsync_task_cb_t cb, uint32_t delay_ms);

/**
 * Schedule a one-shot callback with user data.
 */
pulsync_task_id_t pulsync_set_timeout_arg(pulsync_task_cb_arg_t cb, uint32_t delay_ms, void *arg);

/**
 * Schedule a repeating callback every interval_ms milliseconds.
 * Returns task ID (>0) on success, 0 if scheduler is full.
 */
pulsync_task_id_t pulsync_set_interval(pulsync_task_cb_t cb, uint32_t interval_ms);

/**
 * Schedule a repeating callback with user data.
 */
pulsync_task_id_t pulsync_set_interval_arg(pulsync_task_cb_arg_t cb, uint32_t interval_ms, void *arg);

/**
 * Cancel a scheduled task (works for both timeout and interval).
 * Returns true if task was found and cancelled.
 */
bool pulsync_clear_task(pulsync_task_id_t id);

/**
 * Get number of active tasks.
 */
int pulsync_scheduler_active_count(void);

/**
 * Get current millis (platform abstraction).
 * On Arduino: millis(). On ESP-IDF: esp_timer_get_time() / 1000.
 */
uint32_t pulsync_millis(void);

#ifdef __cplusplus
}
#endif
