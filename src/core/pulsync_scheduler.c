/**
 * pulsync_scheduler — Non-blocking task scheduler implementation
 *
 * Fixed-size array of task slots. No dynamic allocation.
 * Uses monotonic millis counter with wraparound handling.
 */

#include "pulsync_scheduler.h"
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#else
#include "esp_timer.h"
#endif

/* ---------- Internal types ---------- */

typedef enum {
    PULSYNC_TASK_EMPTY = 0,
    PULSYNC_TASK_TIMEOUT,
    PULSYNC_TASK_INTERVAL
} pulsync_task_type_t;

typedef struct {
    pulsync_task_type_t type;
    pulsync_task_id_t   id;
    uint32_t            interval_ms;
    uint32_t            next_run;
    pulsync_task_cb_t   cb;
    pulsync_task_cb_arg_t cb_arg;
    void               *arg;
    bool                has_arg;
} pulsync_task_slot_t;

/* ---------- Static state ---------- */

static pulsync_task_slot_t s_tasks[PULSYNC_MAX_TASKS];
static uint16_t s_next_id = 1;

/* ---------- Platform millis ---------- */

uint32_t pulsync_millis(void) {
#ifdef ARDUINO
    return millis();
#else
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
#endif
}

/* ---------- Internal helpers ---------- */

static pulsync_task_slot_t *find_free_slot(void) {
    for (int i = 0; i < PULSYNC_MAX_TASKS; i++) {
        if (s_tasks[i].type == PULSYNC_TASK_EMPTY) {
            return &s_tasks[i];
        }
    }
    return NULL;
}

static pulsync_task_id_t alloc_id(void) {
    pulsync_task_id_t id = s_next_id++;
    if (s_next_id == 0) s_next_id = 1;  /* skip 0 (invalid) */
    return id;
}

/**
 * Wraparound-safe time comparison.
 * Returns true if 'now' has reached or passed 'target'.
 */
static inline bool time_reached(uint32_t now, uint32_t target) {
    return (int32_t)(now - target) >= 0;
}

/* ---------- Public API ---------- */

void pulsync_scheduler_init(void) {
    memset(s_tasks, 0, sizeof(s_tasks));
    s_next_id = 1;
}

int pulsync_scheduler_tick(void) {
    uint32_t now = pulsync_millis();
    int executed = 0;

    for (int i = 0; i < PULSYNC_MAX_TASKS; i++) {
        pulsync_task_slot_t *t = &s_tasks[i];
        if (t->type == PULSYNC_TASK_EMPTY) continue;

        if (time_reached(now, t->next_run)) {
            /* Execute callback */
            if (t->has_arg) {
                if (t->cb_arg) t->cb_arg(t->arg);
            } else {
                if (t->cb) t->cb();
            }
            executed++;

            if (t->type == PULSYNC_TASK_TIMEOUT) {
                /* One-shot: clear the slot */
                t->type = PULSYNC_TASK_EMPTY;
            } else {
                /* Interval: reschedule from now to avoid drift accumulation on slow callbacks */
                t->next_run = now + t->interval_ms;
            }
        }
    }

    return executed;
}

pulsync_task_id_t pulsync_set_timeout(pulsync_task_cb_t cb, uint32_t delay_ms) {
    pulsync_task_slot_t *slot = find_free_slot();
    if (!slot) return 0;

    slot->type = PULSYNC_TASK_TIMEOUT;
    slot->id = alloc_id();
    slot->interval_ms = delay_ms;
    slot->next_run = pulsync_millis() + delay_ms;
    slot->cb = cb;
    slot->cb_arg = NULL;
    slot->arg = NULL;
    slot->has_arg = false;

    return slot->id;
}

pulsync_task_id_t pulsync_set_timeout_arg(pulsync_task_cb_arg_t cb, uint32_t delay_ms, void *arg) {
    pulsync_task_slot_t *slot = find_free_slot();
    if (!slot) return 0;

    slot->type = PULSYNC_TASK_TIMEOUT;
    slot->id = alloc_id();
    slot->interval_ms = delay_ms;
    slot->next_run = pulsync_millis() + delay_ms;
    slot->cb = NULL;
    slot->cb_arg = cb;
    slot->arg = arg;
    slot->has_arg = true;

    return slot->id;
}

pulsync_task_id_t pulsync_set_interval(pulsync_task_cb_t cb, uint32_t interval_ms) {
    pulsync_task_slot_t *slot = find_free_slot();
    if (!slot) return 0;

    slot->type = PULSYNC_TASK_INTERVAL;
    slot->id = alloc_id();
    slot->interval_ms = interval_ms;
    slot->next_run = pulsync_millis() + interval_ms;
    slot->cb = cb;
    slot->cb_arg = NULL;
    slot->arg = NULL;
    slot->has_arg = false;

    return slot->id;
}

pulsync_task_id_t pulsync_set_interval_arg(pulsync_task_cb_arg_t cb, uint32_t interval_ms, void *arg) {
    pulsync_task_slot_t *slot = find_free_slot();
    if (!slot) return 0;

    slot->type = PULSYNC_TASK_INTERVAL;
    slot->id = alloc_id();
    slot->interval_ms = interval_ms;
    slot->next_run = pulsync_millis() + interval_ms;
    slot->cb = NULL;
    slot->cb_arg = cb;
    slot->arg = arg;
    slot->has_arg = true;

    return slot->id;
}

bool pulsync_clear_task(pulsync_task_id_t id) {
    if (id == 0) return false;

    for (int i = 0; i < PULSYNC_MAX_TASKS; i++) {
        if (s_tasks[i].id == id && s_tasks[i].type != PULSYNC_TASK_EMPTY) {
            s_tasks[i].type = PULSYNC_TASK_EMPTY;
            return true;
        }
    }
    return false;
}

int pulsync_scheduler_active_count(void) {
    int count = 0;
    for (int i = 0; i < PULSYNC_MAX_TASKS; i++) {
        if (s_tasks[i].type != PULSYNC_TASK_EMPTY) count++;
    }
    return count;
}
