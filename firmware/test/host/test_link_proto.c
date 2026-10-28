#include <stdint.h>
#include <string.h>

#include "link_proto/lp.h"
#include "test.h"

/* ACCESS_REQ on the wire: type, seq, op, pin_len, then the 8-byte PIN field. */
enum {
    REQ_OFF_OP = 2,
    REQ_OFF_PIN_LEN = 3,
    REQ_OFF_PIN = 4,
    REQ_LEN = REQ_OFF_PIN + LP_PIN_FIELD_LEN,
    HELLO_LEN = 7,
    HELLO_OFF_ROLE = 2,
};

TEST(test_lp_roundtrip)
{
    enum { SEQ = 9, LOCKOUT_S = 300, RESULT_OFF_LOCKOUT = 4 };
    lp_msg_t req = {.type = LP_MSG_ACCESS_REQ,
                    .access_req = {.seq = SEQ, .op = LP_OP_UNLOCK, .pin_len = 4}};
    memcpy(req.access_req.pin, "1234", 4);
    uint8_t buf[32];
    size_t n = lp_encode(&req, buf, sizeof(buf));
    CHECK_EQ(n, REQ_LEN);
    lp_msg_t d;
    CHECK(lp_decode(buf, n, &d));
    CHECK_EQ(d.access_req.seq, SEQ);
    CHECK_EQ(d.access_req.pin_len, 4);
    CHECK(memcmp(d.access_req.pin, "1234\0\0\0\0", 8) == 0);

    lp_msg_t res = {.type = LP_MSG_ACCESS_RESULT,
                    .access_result = {.seq = SEQ,
                                      .result = LP_RESULT_LOCKED_OUT,
                                      .bolt = LP_BOLT_LOCKED,
                                      .lockout_s = LOCKOUT_S}};
    n = lp_encode(&res, buf, sizeof(buf));
    CHECK(lp_decode(buf, n, &d));
    CHECK_EQ(d.access_result.lockout_s, LOCKOUT_S);
    /* Wire format is little-endian: pin this down for the apps. */
    CHECK_EQ(buf[RESULT_OFF_LOCKOUT], (unsigned)LOCKOUT_S & 0xFFU);
    CHECK_EQ(buf[RESULT_OFF_LOCKOUT + 1], (unsigned)LOCKOUT_S >> 8U);
}

TEST(test_lp_rejects_malformed)
{
    lp_msg_t d;
    enum { BAD_OP = 7, UNKNOWN_TYPE = 0x77, BAD_ROLE = 9 };
    uint8_t buf[32] = {LP_MSG_ACCESS_REQ, 1, LP_OP_UNLOCK, 4, '1', '2', 'x', '4', 0, 0, 0, 0};
    CHECK(!lp_decode(buf, REQ_LEN, &d)); /* non-digit */
    buf[REQ_OFF_PIN + 2] = '3';
    CHECK(lp_decode(buf, REQ_LEN, &d));
    buf[REQ_OFF_PIN + 4] = '5';
    CHECK(!lp_decode(buf, REQ_LEN, &d)); /* junk after pin_len */
    buf[REQ_OFF_PIN + 4] = 0;
    CHECK(!lp_decode(buf, REQ_LEN - 1, &d)); /* wrong length */
    CHECK(!lp_decode(buf, REQ_LEN + 1, &d));
    buf[REQ_OFF_PIN_LEN] = LP_PIN_FIELD_LEN + 1;
    CHECK(!lp_decode(buf, REQ_LEN, &d)); /* pin_len > 8 */
    buf[REQ_OFF_PIN_LEN] = 4;
    buf[REQ_OFF_OP] = BAD_OP;
    CHECK(!lp_decode(buf, REQ_LEN, &d)); /* bad op */
    uint8_t unknown[2] = {UNKNOWN_TYPE, 0};
    CHECK(!lp_decode(unknown, 2, &d));
    CHECK(!lp_decode(buf, 0, &d));
    uint8_t bad_role[HELLO_LEN] = {LP_MSG_HELLO, 1, 0, 0, 0, 0, 0};
    bad_role[HELLO_OFF_ROLE] = BAD_ROLE;
    CHECK(!lp_decode(bad_role, HELLO_LEN, &d));
}

TEST(test_lp_fixed_sizes)
{
    uint8_t buf[32];
    lp_msg_t lock = {.type = LP_MSG_ACCESS_REQ, .access_req = {.op = LP_OP_LOCK}};
    lp_msg_t unlock = {.type = LP_MSG_ACCESS_REQ, .access_req = {.op = LP_OP_UNLOCK, .pin_len = 8}};
    memcpy(unlock.access_req.pin, "12345678", 8);
    CHECK_EQ(lp_encode(&lock, buf, sizeof(buf)), lp_encode(&unlock, buf, sizeof(buf)));
    CHECK_EQ(lp_encode(&lock, buf, REQ_OFF_PIN + 1), 0); /* too small */
}
