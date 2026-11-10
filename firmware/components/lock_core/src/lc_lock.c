#include "lock_core/lc_lock.h"
#include "lock_core/lc_creds.h"
#include "lock_core/lc_log.h"
#include "lock_core/lc_ratelimit.h"
#include "lock_core/lc_types.h"

#include <stdint.h>
#include <string.h>

static uint64_t now_ms(const lc_t *lc) { return lc->plat.now_ms(lc->plat.ctx); }

static bool wall_time(const lc_t *lc, lc_wall_time_t *out)
{
    return lc->plat.wall_time && lc->plat.wall_time(lc->plat.ctx, out);
}

static void persist(lc_t *lc, lc_persist_t what)
{
    if (lc->hooks.persist) {
        lc->hooks.persist(lc->hooks.ctx, what);
    }
}

static bool is_moving(const lc_t *lc)
{
    return lc->bolt == LC_BOLT_LOCKING || lc->bolt == LC_BOLT_UNLOCKING;
}

static void emit(lc_t *lc, lc_event_type_t type, lc_source_t source, uint16_t user_index)
{
    lc_wall_time_t wall = {0};
    bool known = wall_time(lc, &wall);

    if (lc->cfg.log.enabled) {
        if (known) {
            lc_log_prune(&lc->log, &lc->cfg.log, wall.unix_s);
        }
        lc_log_entry_t e = {
            .unix_s = known ? wall.unix_s : 0,
            .uptime_s = (uint32_t)(now_ms(lc) / 1000U),
            .type = (uint8_t)type,
            .source = (uint8_t)source,
            .user_index = user_index,
            .boot = lc->boot_count,
        };
        lc_log_append(&lc->log, &lc->cfg.log, &e);
        persist(lc, LC_PERSIST_LOG);
    }

    if (lc->hooks.notify) {
        lc_event_t ev = {
            .type = type,
            .source = source,
            .user_index = lc->cfg.private_events ? LC_INDEX_NONE : user_index,
        };
        lc->hooks.notify(lc->hooks.ctx, &ev);
    }
}

/* Residential defaults; see lc_config_default(). */
enum {
    DEFAULT_MOTOR_TIMEOUT_MS = 5000,
    DEFAULT_WRONG_CODE_LIMIT = 5,  /* wrong codes before a lockout */
    DEFAULT_BASE_LOCKOUT_S = 60,   /* first lockout; doubles each time */
    DEFAULT_MAX_LOCKOUT_S = 3600,
    DEFAULT_LOG_MAX_AGE_DAYS = 30,
};

lc_config_t lc_config_default(void)
{
    return (lc_config_t){
        .auto_relock_s = 0,
        .one_touch_lock = true,
        .remote_unlock_allowed = true,
        .require_pin_for_remote = false,
        .private_events = true, /* privacy by default; users opt in to attribution */
        .motor_timeout_ms = DEFAULT_MOTOR_TIMEOUT_MS,
        .ratelimit = {.limit = DEFAULT_WRONG_CODE_LIMIT,
                      .base_lockout_s = DEFAULT_BASE_LOCKOUT_S,
                      .max_lockout_s = DEFAULT_MAX_LOCKOUT_S},
        .log = {.enabled = true, .max_age_days = DEFAULT_LOG_MAX_AGE_DAYS},
    };
}

void lc_init(lc_t *lc, const lc_platform_t *plat, const lc_hooks_t *hooks, const lc_config_t *cfg)
{
    memset(lc, 0, sizeof(*lc));
    lc->plat = *plat;
    lc->hooks = *hooks;
    lc->cfg = *cfg;
    lc_creds_init(&lc->creds);
    lc_rl_init(&lc->rl);
    lc_log_init(&lc->log);
    lc->bolt = LC_BOLT_UNKNOWN;
    lc->door = LC_DOOR_UNKNOWN;
    lc->mode = LC_MODE_NORMAL;
}

void lc_boot(lc_t *lc, lc_bolt_t bolt, lc_door_t door)
{
    uint64_t t = now_ms(lc);
    lc->bolt = (bolt == LC_BOLT_LOCKING || bolt == LC_BOLT_UNLOCKING) ? LC_BOLT_UNKNOWN : bolt;
    lc->door = door;
    lc->relock_ref_ms = t;
    lc_rl_on_boot(&lc->rl, &lc->cfg.ratelimit, t);
    if (lc->rl.locked) {
        persist(lc, LC_PERSIST_RATELIMIT);
    }
}

static lc_err_t policy_check(const lc_t *lc, lc_op_t op, lc_source_t source)
{
    switch (lc->mode) {
    case LC_MODE_VACATION:
        if (source == LC_SOURCE_KEYPAD) {
            return LC_ERR_MODE;
        }
        break;
    case LC_MODE_PRIVACY:
        if (source == LC_SOURCE_KEYPAD || source == LC_SOURCE_REMOTE) {
            return LC_ERR_MODE;
        }
        break;
    case LC_MODE_NO_REMOTE:
        if (source == LC_SOURCE_REMOTE) {
            return LC_ERR_MODE;
        }
        break;
    case LC_MODE_NORMAL:
    case LC_MODE_PASSAGE:
        break;
    }
    if (source == LC_SOURCE_REMOTE && op == LC_OP_UNLOCK && !lc->cfg.remote_unlock_allowed) {
        return LC_ERR_MODE;
    }
    return LC_OK;
}

static lc_err_t authenticate(lc_t *lc, lc_source_t source, const uint8_t *pin, size_t pin_len,
                             uint16_t *user_out)
{
    /* While locked out the PIN is not even evaluated, so guesses during a
     * lockout reveal nothing. */
    if (lc_rl_is_locked(&lc->rl, now_ms(lc))) {
        return LC_ERR_LOCKED_OUT;
    }

    uint16_t cred = LC_INDEX_NONE;
    lc_err_t err = lc_pin_match(&lc->creds, &lc->plat, pin, pin_len, &cred);
    if (err == LC_ERR_PLATFORM) {
        return err;
    }
    if (err != LC_OK) {
        bool lockout = lc_rl_on_failure(&lc->rl, &lc->cfg.ratelimit, now_ms(lc));
        persist(lc, LC_PERSIST_RATELIMIT);
        emit(lc, LC_EVENT_WRONG_CODE, source, LC_INDEX_NONE);
        if (lockout) {
            emit(lc, LC_EVENT_LOCKOUT_STARTED, source, LC_INDEX_NONE);
        }
        return LC_ERR_DENIED;
    }

    uint16_t user = lc->creds.creds[cred - 1].user_index;
    lc_wall_time_t wall;
    bool known = wall_time(lc, &wall);
    err = lc_user_access_check(&lc->creds, user, known ? &wall : NULL);
    if (err != LC_OK) {
        emit(lc, LC_EVENT_ACCESS_DENIED, source, user);
        return err;
    }

    if (lc->rl.wrong_count || lc->rl.lockout_level) {
        lc_rl_on_success(&lc->rl);
        persist(lc, LC_PERSIST_RATELIMIT);
    }
    *user_out = user;
    return LC_OK;
}

static void start_motor(lc_t *lc, lc_op_t op, lc_source_t source, uint16_t user)
{
    lc->bolt = op == LC_OP_LOCK ? LC_BOLT_LOCKING : LC_BOLT_UNLOCKING;
    lc->pending_op = op;
    lc->pending_source = source;
    lc->pending_user = user;
    lc->motor_started_ms = now_ms(lc);
    lc->hooks.motor_start(lc->hooks.ctx, op);
}

lc_err_t lc_request(lc_t *lc, lc_op_t op, lc_source_t source, const uint8_t *pin, size_t pin_len,
                    uint16_t *user_out)
{
    uint16_t user = LC_INDEX_NONE;
    if (user_out) {
        *user_out = LC_INDEX_NONE;
    }
    if ((op != LC_OP_LOCK && op != LC_OP_UNLOCK) ||
        (source != LC_SOURCE_KEYPAD && source != LC_SOURCE_REMOTE && source != LC_SOURCE_APP)) {
        return LC_ERR_INVALID_ARG;
    }
    if (is_moving(lc)) {
        return LC_ERR_BUSY;
    }

    lc_err_t err = policy_check(lc, op, source);
    if (err != LC_OK) {
        emit(lc, LC_EVENT_ACCESS_DENIED, source, LC_INDEX_NONE);
        return err;
    }

    bool need_pin = false;
    if (source == LC_SOURCE_KEYPAD) {
        need_pin = !(op == LC_OP_LOCK && lc->cfg.one_touch_lock);
    } else if (source == LC_SOURCE_REMOTE) {
        need_pin = lc->cfg.require_pin_for_remote;
    }
    /* A PIN supplied when not required is still verified, for attribution. */
    if (need_pin || pin) {
        err = authenticate(lc, source, pin, pin_len, &user);
        if (err != LC_OK) {
            return err;
        }
    }

    if (op == LC_OP_LOCK && lc->door == LC_DOOR_OPEN) {
        return LC_ERR_DOOR_OPEN;
    }

    if (user != LC_INDEX_NONE && lc->creds.users[user - 1].type == LC_USER_DISPOSABLE) {
        lc_user_clear(&lc->creds, user);
        persist(lc, LC_PERSIST_CREDS);
    }
    if (user_out) {
        *user_out = user;
    }

    if (op == LC_OP_LOCK && lc->bolt == LC_BOLT_LOCKED) {
        return LC_OK;
    }
    if (op == LC_OP_UNLOCK && lc->bolt == LC_BOLT_UNLOCKED) {
        lc->relock_ref_ms = now_ms(lc); /* restart the auto-relock countdown */
        return LC_OK;
    }
    start_motor(lc, op, source, user);
    return LC_OK;
}

void lc_on_motor_done(lc_t *lc, bool success)
{
    if (!is_moving(lc)) {
        return;
    }
    if (!success) {
        lc->bolt = LC_BOLT_JAMMED;
        emit(lc, LC_EVENT_JAMMED, lc->pending_source, lc->pending_user);
        return;
    }
    if (lc->pending_op == LC_OP_LOCK) {
        lc->bolt = LC_BOLT_LOCKED;
        emit(lc, LC_EVENT_LOCKED, lc->pending_source, lc->pending_user);
    } else {
        lc->bolt = LC_BOLT_UNLOCKED;
        lc->relock_ref_ms = now_ms(lc);
        emit(lc, LC_EVENT_UNLOCKED, lc->pending_source, lc->pending_user);
    }
}

void lc_on_bolt_position(lc_t *lc, lc_bolt_t observed)
{
    if ((observed != LC_BOLT_LOCKED && observed != LC_BOLT_UNLOCKED) || is_moving(lc) ||
        lc->bolt == observed) {
        return;
    }
    lc->bolt = observed;
    if (observed == LC_BOLT_UNLOCKED) {
        lc->relock_ref_ms = now_ms(lc);
        emit(lc, LC_EVENT_UNLOCKED, LC_SOURCE_MANUAL, LC_INDEX_NONE);
    } else {
        emit(lc, LC_EVENT_LOCKED, LC_SOURCE_MANUAL, LC_INDEX_NONE);
    }
}

void lc_on_door(lc_t *lc, lc_door_t door)
{
    if (door == lc->door) {
        return;
    }
    lc->door = door;
    if (door == LC_DOOR_CLOSED) {
        lc->relock_ref_ms = now_ms(lc);
        emit(lc, LC_EVENT_DOOR_CLOSED, LC_SOURCE_MANUAL, LC_INDEX_NONE);
    } else if (door == LC_DOOR_OPEN) {
        emit(lc, LC_EVENT_DOOR_OPENED, LC_SOURCE_MANUAL, LC_INDEX_NONE);
    }
}

void lc_set_mode(lc_t *lc, lc_mode_t mode)
{
    if (lc->mode == LC_MODE_PASSAGE && mode != LC_MODE_PASSAGE) {
        lc->relock_ref_ms = now_ms(lc);
    }
    lc->mode = mode;
}

static bool relock_armed(const lc_t *lc)
{
    return lc->bolt == LC_BOLT_UNLOCKED && lc->cfg.auto_relock_s && lc->mode != LC_MODE_PASSAGE &&
           lc->door != LC_DOOR_OPEN;
}

static uint64_t relock_deadline(const lc_t *lc)
{
    return lc->relock_ref_ms + ((uint64_t)lc->cfg.auto_relock_s * 1000U);
}

void lc_tick(lc_t *lc)
{
    uint64_t t = now_ms(lc);
    if (is_moving(lc) && t - lc->motor_started_ms >= lc->cfg.motor_timeout_ms) {
        lc->hooks.motor_stop(lc->hooks.ctx);
        lc->bolt = LC_BOLT_JAMMED;
        emit(lc, LC_EVENT_JAMMED, lc->pending_source, lc->pending_user);
        return;
    }
    if (relock_armed(lc) && t >= relock_deadline(lc)) {
        start_motor(lc, LC_OP_LOCK, LC_SOURCE_AUTO, LC_INDEX_NONE);
    }
}

uint64_t lc_next_deadline_ms(const lc_t *lc)
{
    if (is_moving(lc)) {
        return lc->motor_started_ms + lc->cfg.motor_timeout_ms;
    }
    if (relock_armed(lc)) {
        return relock_deadline(lc);
    }
    return UINT64_MAX;
}
