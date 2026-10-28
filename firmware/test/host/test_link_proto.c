#include <string.h>

#include "link_proto/lp.h"
#include "test.h"

TEST(test_lp_roundtrip)
{
    lp_msg_t m = {.type = LP_MSG_ACCESS_REQ, .access_req = {.seq = 9, .op = LP_OP_UNLOCK, .pin_len = 4}};
    memcpy(m.access_req.pin, "1234", 4);
    uint8_t buf[32];
    size_t n = lp_encode(&m, buf, sizeof(buf));
    CHECK_EQ(n, 12);
    lp_msg_t d;
    CHECK(lp_decode(buf, n, &d));
    CHECK_EQ(d.access_req.seq, 9);
    CHECK_EQ(d.access_req.pin_len, 4);
    CHECK(memcmp(d.access_req.pin, "1234\0\0\0\0", 8) == 0);

    lp_msg_t r = {.type = LP_MSG_ACCESS_RESULT,
                  .access_result = {.seq = 9, .result = LP_RESULT_LOCKED_OUT, .bolt = 1, .lockout_s = 300}};
    n = lp_encode(&r, buf, sizeof(buf));
    CHECK(lp_decode(buf, n, &d));
    CHECK_EQ(d.access_result.lockout_s, 300);
    /* Wire format is little-endian: pin this down for the apps. */
    CHECK_EQ(buf[4], 300 & 0xff);
    CHECK_EQ(buf[5], 300 >> 8);
}

TEST(test_lp_rejects_malformed)
{
    lp_msg_t d;
    uint8_t buf[32] = {LP_MSG_ACCESS_REQ, 1, LP_OP_UNLOCK, 4, '1', '2', 'x', '4', 0, 0, 0, 0};
    CHECK(!lp_decode(buf, 12, &d)); /* non-digit */
    buf[6] = '3';
    CHECK(lp_decode(buf, 12, &d));
    buf[8] = '5';
    CHECK(!lp_decode(buf, 12, &d)); /* junk after pin_len */
    buf[8] = 0;
    CHECK(!lp_decode(buf, 11, &d)); /* wrong length */
    CHECK(!lp_decode(buf, 13, &d));
    buf[3] = 9;
    CHECK(!lp_decode(buf, 12, &d)); /* pin_len > 8 */
    buf[3] = 4;
    buf[2] = 7;
    CHECK(!lp_decode(buf, 12, &d)); /* bad op */
    uint8_t unknown[2] = {0x77, 0};
    CHECK(!lp_decode(unknown, 2, &d));
    CHECK(!lp_decode(buf, 0, &d));
    uint8_t bad_role[7] = {LP_MSG_HELLO, 1, 9, 0, 0, 0, 0};
    CHECK(!lp_decode(bad_role, 7, &d));
}

TEST(test_lp_fixed_sizes)
{
    uint8_t buf[32];
    lp_msg_t a = {.type = LP_MSG_ACCESS_REQ, .access_req = {.op = LP_OP_LOCK}};
    lp_msg_t b = {.type = LP_MSG_ACCESS_REQ, .access_req = {.op = LP_OP_UNLOCK, .pin_len = 8}};
    memcpy(b.access_req.pin, "12345678", 8);
    CHECK_EQ(lp_encode(&a, buf, sizeof(buf)), lp_encode(&b, buf, sizeof(buf)));
    CHECK_EQ(lp_encode(&a, buf, 5), 0); /* too small */
}
