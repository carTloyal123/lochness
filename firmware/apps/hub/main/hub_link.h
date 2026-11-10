/*
 * The hub's link to the lock: owns hub_core on its own task. Matter command
 * handlers call hub_link_request(), which blocks until the lock answers.
 * State pushes from the lock arrive through the callbacks.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_proto/lp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Called on the hub task; must only queue work. */
    void (*lock_state)(const lp_msg_t *state);
    void (*link_changed)(bool is_up);
    /* Wall clock for the lock; false while unknown. */
    bool (*wall_time)(uint32_t *unix_s, int16_t *utc_offset_min);
} hub_link_cbs_t;

/* Call after the BLE host is up (i.e. after esp_matter::start). */
void hub_link_start(const hub_link_cbs_t *cbs);

/*
 * Lock or unlock, waiting up to timeout_ms for the lock's answer. Returns an
 * lp_result_t; LP_RESULT_ERROR when the link is down or the lock did not
 * answer in time. *bolt (optional) gets the bolt state from the answer.
 */
uint8_t hub_link_request(lp_op_t op, const uint8_t *pin, size_t pin_len, uint32_t timeout_ms, uint8_t *bolt);

void hub_link_pair(void);
void hub_link_forget(void);
/* The wall clock became known or changed: push it to the lock now. */
void hub_link_time_changed(void);
bool hub_link_ready(void);
void hub_link_print_status(void);

#ifdef __cplusplus
}
#endif
