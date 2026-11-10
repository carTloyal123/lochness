/*
 * Lock state machine and access policy. This is the single place where
 * lock/unlock decisions are made.
 *
 * Usage:
 *   lc_init(&lc, &plat, &hooks, &cfg);   // zeroes persistent state
 *   // platform restores lc.creds / lc.rl / lc.log from storage here
 *   lc_boot(&lc, bolt_from_encoder, door_from_sensor);
 *   loop: feed events (lc_request, lc_on_*), call lc_tick() by
 *         lc_next_deadline_ms() at the latest.
 *
 * Not thread-safe: call from one task.
 */
#pragma once

#include "lock_core/lc_creds.h"
#include "lock_core/lc_log.h"
#include "lock_core/lc_ratelimit.h"
#include "lock_core/lc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LC_BOLT_UNKNOWN,
    LC_BOLT_LOCKED,
    LC_BOLT_UNLOCKED,
    LC_BOLT_LOCKING,
    LC_BOLT_UNLOCKING,
    LC_BOLT_JAMMED,
} lc_bolt_t;

typedef enum {
    LC_DOOR_UNKNOWN, /* no sensor or not yet read */
    LC_DOOR_CLOSED,
    LC_DOOR_OPEN,
} lc_door_t;

/* Mirrors Matter OperatingModeEnum. */
typedef enum {
    LC_MODE_NORMAL,
    LC_MODE_VACATION,  /* keypad disabled; remote and app still work */
    LC_MODE_PRIVACY,   /* keypad and remote disabled; app and manual still work */
    LC_MODE_NO_REMOTE, /* remote lock/unlock disabled */
    LC_MODE_PASSAGE,   /* auto-relock suspended */
} lc_mode_t;

typedef struct {
    uint32_t auto_relock_s;     /* 0 = off. Counted from unlock or door close, whichever is later. */
    bool one_touch_lock;        /* keypad may lock without a PIN */
    bool remote_unlock_allowed; /* lock-side cap; Matter controllers cannot change this */
    bool require_pin_for_remote;
    bool private_events;        /* strip user_index from notifications leaving the lock */
    uint32_t motor_timeout_ms;
    lc_ratelimit_cfg_t ratelimit;
    lc_log_cfg_t log;
} lc_config_t;

typedef enum {
    LC_PERSIST_CREDS,
    LC_PERSIST_RATELIMIT,
    LC_PERSIST_LOG,
} lc_persist_t;

typedef struct {
    lc_event_type_t type;
    lc_source_t source;
    uint16_t user_index; /* LC_INDEX_NONE when unknown or private_events is set */
} lc_event_t;

typedef struct {
    void *ctx;
    /* Start driving the bolt. Report completion with lc_on_motor_done(). */
    void (*motor_start)(void *ctx, lc_op_t op);
    void (*motor_stop)(void *ctx);
    /* Event for Matter and the app. May be NULL. */
    void (*notify)(void *ctx, const lc_event_t *ev);
    /*
     * Persist the named state. CREDS and RATELIMIT must be durable before this
     * returns (a battery pull must not undo a wrong-code count). LOG may be
     * batched to limit flash wear.
     */
    void (*persist)(void *ctx, lc_persist_t what);
} lc_hooks_t;

typedef struct {
    /* Persistent state: the platform loads/saves these. */
    lc_creds_t creds;
    lc_ratelimit_t rl;
    lc_log_t log;

    /* Configuration and bindings. */
    lc_config_t cfg;
    lc_platform_t plat;
    lc_hooks_t hooks;

    /* Runtime state (read-only for callers). */
    lc_bolt_t bolt;
    lc_door_t door;
    lc_mode_t mode;
    /* Set by the platform after lc_init; stamped on log entries. */
    uint16_t boot_count;

    /* Internal. */
    lc_op_t pending_op;
    lc_source_t pending_source;
    uint16_t pending_user;
    uint64_t motor_started_ms;
    uint64_t relock_ref_ms;
} lc_t;

void lc_init(lc_t *lc, const lc_platform_t *plat, const lc_hooks_t *hooks, const lc_config_t *cfg);
void lc_boot(lc_t *lc, lc_bolt_t bolt, lc_door_t door);
/* Defaults suitable for a residential deadbolt. */
lc_config_t lc_config_default(void);

/*
 * Request a lock/unlock. pin may be NULL when the source does not need one.
 * On success the motor has been started (or the bolt was already in the
 * requested state) and *user_out (optional) holds the authorizing user.
 */
lc_err_t lc_request(lc_t *lc, lc_op_t op, lc_source_t source, const uint8_t *pin, size_t pin_len,
                    uint16_t *user_out);

void lc_on_motor_done(lc_t *lc, bool success);
/* Encoder reported a resting bolt position (LOCKED/UNLOCKED). */
void lc_on_bolt_position(lc_t *lc, lc_bolt_t observed);
void lc_on_door(lc_t *lc, lc_door_t door);
void lc_set_mode(lc_t *lc, lc_mode_t mode);

/* Handles motor timeouts and auto-relock. */
void lc_tick(lc_t *lc);
/* Next monotonic time lc_tick() needs to run, or UINT64_MAX if none. */
uint64_t lc_next_deadline_ms(const lc_t *lc);

#ifdef __cplusplus
}
#endif
