#include "lock_bridge.h"
#include "link_proto/lp.h"
#include "lock_core/lc_lock.h"
#include "lock_core/lc_log.h"
#include "lock_core/lc_types.h"
#include "lock_link/ll.h"
#include <stddef.h>
#include <stdint.h>

static lp_result_t to_result(lc_err_t e)
{
    switch (e) {
    case LC_OK: return LP_RESULT_OK;
    case LC_ERR_DENIED: return LP_RESULT_DENIED;
    case LC_ERR_LOCKED_OUT: return LP_RESULT_LOCKED_OUT;
    case LC_ERR_SCHEDULE: return LP_RESULT_SCHEDULE;
    case LC_ERR_MODE: return LP_RESULT_MODE;
    case LC_ERR_BUSY: return LP_RESULT_BUSY;
    case LC_ERR_DOOR_OPEN: return LP_RESULT_DOOR_OPEN;
    default: return LP_RESULT_ERROR;
    }
}

lp_bolt_t lock_bridge_bolt(const lc_t *lc)
{
    switch (lc->bolt) {
    case LC_BOLT_LOCKED: return LP_BOLT_LOCKED;
    case LC_BOLT_UNLOCKED: return LP_BOLT_UNLOCKED;
    case LC_BOLT_LOCKING:
    case LC_BOLT_UNLOCKING: return LP_BOLT_MOVING;
    case LC_BOLT_JAMMED: return LP_BOLT_JAMMED;
    default: return LP_BOLT_UNKNOWN;
    }
}

enum { MS_PER_S = 1000 };

ll_access_result_t lock_bridge_access(lc_t *lc, lp_role_t role, lp_op_t op, const uint8_t *pin,
                                      size_t pin_len)
{
    ll_access_result_t r = {.result = LP_RESULT_NOT_PERMITTED};
    lc_source_t source = LC_SOURCE_KEYPAD;
    switch (role) {
    case LP_ROLE_KEYPAD: source = LC_SOURCE_KEYPAD; break;
    case LP_ROLE_PHONE: source = LC_SOURCE_APP; break;
    default: return r;
    }
    lc_err_t e = lc_request(lc, op == LP_OP_LOCK ? LC_OP_LOCK : LC_OP_UNLOCK, source,
                            pin_len ? pin : NULL, pin_len, NULL);
    r.result = to_result(e);
    r.bolt = lock_bridge_bolt(lc);
    if (lc->rl.locked) {
        uint64_t now = lc->plat.now_ms(lc->plat.ctx);
        uint64_t left = lc->rl.locked_until_ms > now ? lc->rl.locked_until_ms - now : 0;
        uint64_t s = (left + MS_PER_S - 1) / MS_PER_S; /* round up */
        r.lockout_s = (uint16_t)(s > UINT16_MAX ? UINT16_MAX : s);
    }
    return r;
}

static lp_event_t to_event(lc_event_type_t t)
{
    switch (t) {
    case LC_EVENT_LOCKED: return LP_EVENT_LOCKED;
    case LC_EVENT_UNLOCKED: return LP_EVENT_UNLOCKED;
    case LC_EVENT_WRONG_CODE: return LP_EVENT_WRONG_CODE;
    case LC_EVENT_LOCKOUT_STARTED: return LP_EVENT_LOCKOUT;
    case LC_EVENT_ACCESS_DENIED: return LP_EVENT_ACCESS_DENIED;
    case LC_EVENT_JAMMED: return LP_EVENT_JAMMED;
    case LC_EVENT_DOOR_OPENED: return LP_EVENT_DOOR_OPENED;
    case LC_EVENT_DOOR_CLOSED: return LP_EVENT_DOOR_CLOSED;
    }
    return LP_EVENT_NONE;
}

static lp_source_t to_source(lc_source_t s)
{
    switch (s) {
    case LC_SOURCE_KEYPAD: return LP_SOURCE_KEYPAD;
    case LC_SOURCE_REMOTE: return LP_SOURCE_REMOTE;
    case LC_SOURCE_APP: return LP_SOURCE_APP;
    case LC_SOURCE_MANUAL: return LP_SOURCE_MANUAL;
    case LC_SOURCE_AUTO: return LP_SOURCE_AUTO;
    }
    return LP_SOURCE_NONE;
}

static lp_door_t to_door(lc_door_t d)
{
    switch (d) {
    case LC_DOOR_CLOSED: return LP_DOOR_CLOSED;
    case LC_DOOR_OPEN: return LP_DOOR_OPEN;
    default: return LP_DOOR_UNKNOWN;
    }
}

void lock_bridge_state(const lc_t *lc, const lc_event_t *ev, lp_msg_t *out)
{
    *out = (lp_msg_t){.type = LP_MSG_LOCK_STATE,
                      .lock_state = {.bolt = (uint8_t)lock_bridge_bolt(lc),
                                     .door = (uint8_t)to_door(lc->door),
                                     .event = ev ? (uint8_t)to_event(ev->type) : (uint8_t)LP_EVENT_NONE,
                                     .source = ev ? (uint8_t)to_source(ev->source) : (uint8_t)LP_SOURCE_NONE,
                                     .user_index = ev ? ev->user_index : 0}};
}
