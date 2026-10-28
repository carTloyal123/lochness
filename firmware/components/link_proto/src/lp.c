#include "link_proto/lp.h"

#include <string.h>

/* Body sizes, excluding the type byte. */
static int body_len(uint8_t type)
{
    switch (type) {
    case LP_MSG_HELLO: return 6;
    case LP_MSG_HELLO_ACK: return 2;
    case LP_MSG_ACCESS_REQ: return 3 + LP_PIN_FIELD_LEN;
    case LP_MSG_ACCESS_RESULT: return 5;
    case LP_MSG_KEYPAD_STATUS: return 3;
    case LP_MSG_PAIR_RESULT: return 1;
    default: return -1;
    }
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

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

size_t lp_encode(const lp_msg_t *m, uint8_t *buf, size_t cap)
{
    int n = body_len(m->type);
    if (n < 0 || (size_t)n + 1 > cap) {
        return 0;
    }
    uint8_t *b = buf + 1;
    buf[0] = m->type;
    switch (m->type) {
    case LP_MSG_HELLO:
        b[0] = m->hello.proto;
        b[1] = m->hello.role;
        b[2] = m->hello.fw_major;
        b[3] = m->hello.fw_minor;
        put16(b + 4, m->hello.battery_mv);
        break;
    case LP_MSG_HELLO_ACK:
        b[0] = m->hello_ack.proto;
        b[1] = m->hello_ack.bolt;
        break;
    case LP_MSG_ACCESS_REQ:
        if (m->access_req.op > LP_OP_UNLOCK || !pin_ok(m->access_req.pin_len, m->access_req.pin)) {
            return 0;
        }
        b[0] = m->access_req.seq;
        b[1] = m->access_req.op;
        b[2] = m->access_req.pin_len;
        memcpy(b + 3, m->access_req.pin, LP_PIN_FIELD_LEN);
        break;
    case LP_MSG_ACCESS_RESULT:
        b[0] = m->access_result.seq;
        b[1] = m->access_result.result;
        b[2] = m->access_result.bolt;
        put16(b + 3, m->access_result.lockout_s);
        break;
    case LP_MSG_KEYPAD_STATUS:
        put16(b, m->keypad_status.battery_mv);
        b[2] = m->keypad_status.flags;
        break;
    case LP_MSG_PAIR_RESULT:
        b[0] = m->pair_result.accepted;
        break;
    }
    return (size_t)n + 1;
}

bool lp_decode(const uint8_t *buf, size_t len, lp_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    if (len < 1) {
        return false;
    }
    int n = body_len(buf[0]);
    if (n < 0 || len != (size_t)n + 1) {
        return false;
    }
    const uint8_t *b = buf + 1;
    m->type = buf[0];
    switch (m->type) {
    case LP_MSG_HELLO:
        m->hello.proto = b[0];
        m->hello.role = b[1];
        m->hello.fw_major = b[2];
        m->hello.fw_minor = b[3];
        m->hello.battery_mv = get16(b + 4);
        return m->hello.role >= LP_ROLE_KEYPAD && m->hello.role <= LP_ROLE_PHONE;
    case LP_MSG_HELLO_ACK:
        m->hello_ack.proto = b[0];
        m->hello_ack.bolt = b[1];
        return true;
    case LP_MSG_ACCESS_REQ:
        m->access_req.seq = b[0];
        m->access_req.op = b[1];
        m->access_req.pin_len = b[2];
        memcpy(m->access_req.pin, b + 3, LP_PIN_FIELD_LEN);
        if (m->access_req.op > LP_OP_UNLOCK || !pin_ok(m->access_req.pin_len, m->access_req.pin)) {
            lp_msg_wipe(m);
            return false;
        }
        return true;
    case LP_MSG_ACCESS_RESULT:
        m->access_result.seq = b[0];
        m->access_result.result = b[1];
        m->access_result.bolt = b[2];
        m->access_result.lockout_s = get16(b + 3);
        return true;
    case LP_MSG_KEYPAD_STATUS:
        m->keypad_status.battery_mv = get16(b);
        m->keypad_status.flags = b[2];
        return true;
    case LP_MSG_PAIR_RESULT:
        m->pair_result.accepted = b[0];
        return b[0] <= 1;
    }
    return false;
}

void lp_msg_wipe(lp_msg_t *m)
{
    volatile uint8_t *p = (volatile uint8_t *)m;
    for (size_t i = 0; i < sizeof(*m); i++) {
        p[i] = 0;
    }
}
