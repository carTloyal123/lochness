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
