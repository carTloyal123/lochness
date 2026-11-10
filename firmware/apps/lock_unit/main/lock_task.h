/*
 * The lock task owns the lc_t instance; lock_core is not thread-safe, so every
 * other task (console, BLE, drivers) talks to it through this API.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "link_proto/lp.h"
#include "lock_core/lc_lock.h"
#include "lock_core/lc_types.h"
#include "lock_link/ll.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Everything the lock task owns. */
/* What the lock last heard from each paired keypad (RAM only). */
typedef struct {
    bool seen;
    uint16_t battery_mv; /* 0 = keypad has no battery sense */
    uint8_t flags;       /* LP_STATUS_* */
    uint16_t boot;       /* lock boot and uptime when last heard */
    uint32_t uptime_s;
} lock_peer_status_t;

typedef struct {
    lc_t lc;
    ll_t ll;
    lock_peer_status_t peer_status[LL_MAX_PEERS];
} lock_ctx_t;

typedef int (*lock_task_fn_t)(lock_ctx_t *ctx, void *arg);

void lock_task_start(void);

/* Run func(ctx, arg) on the lock task and return its result. Blocks the caller. */
int lock_task_call(lock_task_fn_t func, void *arg);

/* Driver events. Safe from tasks and timer callbacks (not ISRs). */
void lock_task_post_motor_done(bool success);
void lock_task_post_door(lc_door_t door);
void lock_task_post_bolt_position(lc_bolt_t bolt);

/* BLE transport events (from the NimBLE host task). */
void lock_task_post_link_connect(uint16_t conn);
void lock_task_post_link_disconnect(uint16_t conn);
void lock_task_post_link_frame(uint16_t conn, const uint8_t *frame, size_t len);

/* Persist the current config (call from inside lock_task_call). */
void lock_task_save_config(const lc_t *lc);
/* Persist the paired-peer table (call from inside lock_task_call). */
void lock_task_save_peers(const ll_t *ll);

/* Lock or unlock on behalf of `source` (Matter, console). Blocks until the
 * lock has decided; the bolt moves afterwards and shows up as state. */
lc_err_t lock_task_request(lc_op_t op, lc_source_t source, const uint8_t *pin, size_t pin_len);

/* Receives every LOCK_STATE the lock produces (events and bolt/door changes),
 * the same messages phone sessions get, plus one snapshot (event
 * LP_EVENT_NONE) when it is set. Runs on the lock task: queue work only.
 * One observer (Matter); NULL removes it. */
typedef void (*lock_state_observer_t)(const lp_msg_t *state);
void lock_task_set_state_observer(lock_state_observer_t observer);

/* Receives the settings whenever they are saved (console, Matter), plus once
 * when set. Runs on the lock task: copy what you need and queue work only.
 * One observer (Matter); NULL removes it. */
typedef void (*lock_config_observer_t)(const lc_config_t *cfg);
void lock_task_set_config_observer(lock_config_observer_t observer);

/* Change and persist the auto-relock time (seconds, 0 = off). */
void lock_task_set_auto_relock(uint32_t seconds);

#ifdef __cplusplus
}
#endif
