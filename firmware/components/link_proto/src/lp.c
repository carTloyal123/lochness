#include "link_proto/lp.h"

#include <stdint.h>
#include <string.h>

/* Body sizes, excluding the type byte. Layouts are in docs/LINK_PROTOCOL.md. */
enum {
    HELLO_BODY_LEN = 6,         /* proto, role, fw_major, fw_minor, battery_mv(2) */
    HELLO_ACK_BODY_LEN = 2,     /* proto, bolt */
    ACCESS_REQ_HEAD_LEN = 3,    /* seq, op, pin_len; then the PIN field */
    ACCESS_RESULT_BODY_LEN = 5, /* seq, result, bolt, lockout_s(2) */
    KEYPAD_STATUS_BODY_LEN = 3, /* battery_mv(2), flags */
    PAIR_RESULT_BODY_LEN = 1,   /* accepted */
    TIME_SET_BODY_LEN = 6,      /* unix_s(4), utc_offset_min(2, signed) */
    LOCK_STATE_BODY_LEN = 6,    /* bolt, door, event, source, user_index(2) */
};

static int body_len(uint8_t type)
{
    switch (type) {
    case LP_MSG_HELLO: return HELLO_BODY_LEN;
    case LP_MSG_HELLO_ACK: return HELLO_ACK_BODY_LEN;
    case LP_MSG_ACCESS_REQ: return ACCESS_REQ_HEAD_LEN + LP_PIN_FIELD_LEN;
    case LP_MSG_ACCESS_RESULT: return ACCESS_RESULT_BODY_LEN;
    case LP_MSG_KEYPAD_STATUS: return KEYPAD_STATUS_BODY_LEN;
    case LP_MSG_PAIR_RESULT: return PAIR_RESULT_BODY_LEN;
    case LP_MSG_TIME_SET: return TIME_SET_BODY_LEN;
    case LP_MSG_PING: return 0;
    case LP_MSG_LOCK_STATE: return LOCK_STATE_BODY_LEN;
    default: return -1;
    }
}

/* 16-bit fields are little-endian. */
static void put16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)((unsigned)value >> 8U);
}

static uint16_t get16(const uint8_t *src) { return (uint16_t)((unsigned)src[0] | ((unsigned)src[1] << 8U)); }

static void put32(uint8_t *dst, uint32_t value)
{
    put16(dst, (uint16_t)value);
    put16(dst + 2, (uint16_t)(value >> 16U));
}

static uint32_t get32(const uint8_t *src) { return (uint32_t)get16(src) | ((uint32_t)get16(src + 2) << 16U); }

static bool offset_ok(int16_t offset) { return offset >= LP_UTC_OFFSET_MIN && offset <= LP_UTC_OFFSET_MAX; }

static bool lock_state_ok(uint8_t bolt, uint8_t door, uint8_t event, uint8_t source)
{
    return bolt <= LP_BOLT_JAMMED && door <= LP_DOOR_OPEN &&
           (event <= LP_EVENT_DOOR_CLOSED || event == LP_EVENT_NONE) &&
           (source <= LP_SOURCE_AUTO || source == LP_SOURCE_NONE);
}

static bool pin_ok(uint8_t len, const uint8_t *pin)
{
    if (len > LP_PIN_FIELD_LEN) {
        return false;
    }
    for (uint8_t i = 0; i < LP_PIN_FIELD_LEN; i++) {
        if (i < len ? (pin[i] < '0' || pin[i] > '9') : pin[i] != 0) {
            return false;
        }
    }
    return true;
}

size_t lp_encode(const lp_msg_t *msg, uint8_t *buf, size_t cap)
{
    int n = body_len(msg->type);
    if (n < 0 || (size_t)n + 1 > cap) {
        return 0;
    }
    uint8_t *body = buf + 1;
    buf[0] = msg->type;
    switch (msg->type) {
    case LP_MSG_HELLO:
        body[0] = msg->hello.proto;
        body[1] = msg->hello.role;
        body[2] = msg->hello.fw_major;
        body[3] = msg->hello.fw_minor;
        put16(body + 4, msg->hello.battery_mv);
        break;
    case LP_MSG_HELLO_ACK:
        body[0] = msg->hello_ack.proto;
        body[1] = msg->hello_ack.bolt;
        break;
    case LP_MSG_ACCESS_REQ:
        if (msg->access_req.op > LP_OP_UNLOCK || !pin_ok(msg->access_req.pin_len, msg->access_req.pin)) {
            return 0;
        }
        body[0] = msg->access_req.seq;
        body[1] = msg->access_req.op;
        body[2] = msg->access_req.pin_len;
        memcpy(body + ACCESS_REQ_HEAD_LEN, msg->access_req.pin, LP_PIN_FIELD_LEN);
        break;
    case LP_MSG_ACCESS_RESULT:
        body[0] = msg->access_result.seq;
        body[1] = msg->access_result.result;
        body[2] = msg->access_result.bolt;
        put16(body + 3, msg->access_result.lockout_s);
        break;
    case LP_MSG_KEYPAD_STATUS:
        put16(body, msg->keypad_status.battery_mv);
        body[2] = msg->keypad_status.flags;
        break;
    case LP_MSG_PAIR_RESULT:
        body[0] = msg->pair_result.accepted;
        break;
    case LP_MSG_TIME_SET:
        if (!offset_ok(msg->time_set.utc_offset_min)) {
            return 0;
        }
        put32(body, msg->time_set.unix_s);
        put16(body + 4, (uint16_t)msg->time_set.utc_offset_min);
        break;
    case LP_MSG_PING:
        break;
    case LP_MSG_LOCK_STATE:
        if (!lock_state_ok(msg->lock_state.bolt, msg->lock_state.door, msg->lock_state.event,
                           msg->lock_state.source)) {
            return 0;
        }
        body[0] = msg->lock_state.bolt;
        body[1] = msg->lock_state.door;
        body[2] = msg->lock_state.event;
        body[3] = msg->lock_state.source;
        put16(body + 4, msg->lock_state.user_index);
        break;
    default:
        return 0; /* unreachable: body_len() rejected unknown types */
    }
    return (size_t)n + 1;
}

bool lp_decode(const uint8_t *buf, size_t len, lp_msg_t *msg)
{
    memset(msg, 0, sizeof(*msg));
    if (len < 1) {
        return false;
    }
    int n = body_len(buf[0]);
    if (n < 0 || len != (size_t)n + 1) {
        return false;
    }
    const uint8_t *body = buf + 1;
    msg->type = buf[0];
    switch (msg->type) {
    case LP_MSG_HELLO:
        msg->hello.proto = body[0];
        msg->hello.role = body[1];
        msg->hello.fw_major = body[2];
        msg->hello.fw_minor = body[3];
        msg->hello.battery_mv = get16(body + 4);
        return msg->hello.role >= LP_ROLE_KEYPAD && msg->hello.role <= LP_ROLE_PHONE;
    case LP_MSG_HELLO_ACK:
        msg->hello_ack.proto = body[0];
        msg->hello_ack.bolt = body[1];
        return true;
    case LP_MSG_ACCESS_REQ:
        msg->access_req.seq = body[0];
        msg->access_req.op = body[1];
        msg->access_req.pin_len = body[2];
        memcpy(msg->access_req.pin, body + ACCESS_REQ_HEAD_LEN, LP_PIN_FIELD_LEN);
        if (msg->access_req.op > LP_OP_UNLOCK || !pin_ok(msg->access_req.pin_len, msg->access_req.pin)) {
            lp_msg_wipe(msg);
            return false;
        }
        return true;
    case LP_MSG_ACCESS_RESULT:
        msg->access_result.seq = body[0];
        msg->access_result.result = body[1];
        msg->access_result.bolt = body[2];
        msg->access_result.lockout_s = get16(body + 3);
        return true;
    case LP_MSG_KEYPAD_STATUS:
        msg->keypad_status.battery_mv = get16(body);
        msg->keypad_status.flags = body[2];
        return true;
    case LP_MSG_PAIR_RESULT:
        msg->pair_result.accepted = body[0];
        return body[0] <= 1;
    case LP_MSG_TIME_SET:
        msg->time_set.unix_s = get32(body);
        msg->time_set.utc_offset_min = (int16_t)get16(body + 4);
        return offset_ok(msg->time_set.utc_offset_min);
    case LP_MSG_PING:
        return true;
    case LP_MSG_LOCK_STATE:
        msg->lock_state.bolt = body[0];
        msg->lock_state.door = body[1];
        msg->lock_state.event = body[2];
        msg->lock_state.source = body[3];
        msg->lock_state.user_index = get16(body + 4);
        return lock_state_ok(body[0], body[1], body[2], body[3]);
    default:
        return false; /* unreachable: body_len() rejected unknown types */
    }
}

void lp_msg_wipe(lp_msg_t *msg)
{
    volatile uint8_t *bytes = (volatile uint8_t *)msg;
    for (size_t i = 0; i < sizeof(*msg); i++) {
        bytes[i] = 0;
    }
}
