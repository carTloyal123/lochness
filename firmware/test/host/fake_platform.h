/* Deterministic platform + hooks for host tests. */
#pragma once

#include "lock_core/lc_lock.h"

typedef struct {
    uint64_t now_ms;
    bool wall_known;
    lc_wall_time_t wall;
    uint8_t rng_counter;
    bool digest_fail;

    int motor_starts;
    lc_op_t last_motor_op;
    int motor_stops;
    int persist_counts[3];
    int notify_count;
    lc_event_t events[32];
} fake_t;

void fake_reset(fake_t *fake);
lc_platform_t fake_platform(fake_t *fake);
lc_hooks_t fake_hooks(fake_t *fake);
/* Last notified event, or NULL. */
const lc_event_t *fake_last_event(const fake_t *fake);
