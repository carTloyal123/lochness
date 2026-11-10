/*
 * Phone sessions on the lock side: clock updates, LOCK_STATE pushes, and the
 * rules that keep keypads out of both. The "phone" here is a bare Noise IK
 * initiator driving lock_link directly.
 */
#include <stdint.h>
#include <string.h>

#include "fake_platform.h"
#include "link_proto/lp.h"
#include "lock_bridge.h"
#include "lock_core/lc_lock.h"
#include "lock_core/lc_log.h"
#include "lock_core/lc_types.h"
#include "lock_link/ll.h"
#include "secure_link/sl_crypto.h"
#include "secure_link/sl_noise.h"
#include "test.h"

enum {
    PHONE_CONN = 3,
    KEYPAD_CONN = 4,
    MAX_OUT = 8,
    PHONE_SLOT = 0,
    KEYPAD_SLOT = 1,
    SOME_UNIX_S = 1800000000,
    CHICAGO_DST_MIN = -300,
};

typedef struct {
    uint16_t conn;
    uint8_t data[LP_MAX_FRAME];
    size_t len;
} out_t;

typedef struct {
    sl_keypair_t id;
    sl_handshake_t hs;
    sl_session_t sess;
} peer_t;

static struct {
    fake_t fake;
    lc_t lc;
    ll_t ll;
    sl_keypair_t lock_id;
    peer_t phone, keypad;
    out_t out[MAX_OUT];
    int nout;
    int dropped;
    uint16_t dropped_conn;
    int time_sets;
    uint32_t time_unix;
    int16_t time_offset;
    uint8_t rng;
} world; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): test fixture state; hooks get no ctx

static uint64_t h_now(void *ctx) { (void)ctx; return world.fake.now_ms; }

static bool h_rng(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    enum { MUL = 151U, ADD = 7U };
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)((++world.rng * MUL) + ADD);
    }
    return true;
}

static bool h_send(void *ctx, uint16_t conn, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (world.nout < MAX_OUT) {
        world.out[world.nout].conn = conn;
        memcpy(world.out[world.nout].data, frame, len);
        world.out[world.nout++].len = len;
    }
    return true;
}

static void h_disconnect(void *ctx, uint16_t conn)
{
    (void)ctx;
    world.dropped++;
    world.dropped_conn = conn;
}

static ll_access_result_t h_access(void *ctx, lp_role_t role, lp_op_t op, const uint8_t *pin, size_t n)
{
    (void)ctx;
    return lock_bridge_access(&world.lc, role, op, pin, n);
}

static lp_bolt_t h_bolt(void *ctx) { (void)ctx; return lock_bridge_bolt(&world.lc); }

static void h_time_set(void *ctx, uint32_t unix_s, int16_t offset)
{
    (void)ctx;
    world.time_sets++;
    world.time_unix = unix_s;
    world.time_offset = offset;
}

static void h_snapshot(void *ctx, lp_msg_t *state)
{
    (void)ctx;
    lock_bridge_state(&world.lc, NULL, state);
}

static void add_peer(int slot, lp_role_t role, peer_t *p, uint8_t fill)
{
    memset(p->id.priv, fill, SL_KEY_LEN);
    sl_keypair_from_private(&p->id);
    world.ll.peers.peers[slot].in_use = true;
    world.ll.peers.peers[slot].role = (uint8_t)role;
    memcpy(world.ll.peers.peers[slot].static_pub, p->id.pub, SL_KEY_LEN);
}

static void setup(void)
{
    memset(&world, 0, sizeof(world));
    fake_reset(&world.fake);
    lc_config_t cfg = lc_config_default();
    lc_platform_t plat = fake_platform(&world.fake);
    lc_hooks_t hooks = fake_hooks(&world.fake);
    lc_init(&world.lc, &plat, &hooks, &cfg);
    lc_boot(&world.lc, LC_BOLT_LOCKED, LC_DOOR_CLOSED);

    enum { LOCK_FILL = 'L', PHONE_FILL = 'P', KEYPAD_FILL = 'K' };
    memset(world.lock_id.priv, LOCK_FILL, SL_KEY_LEN);
    sl_keypair_from_private(&world.lock_id);
    ll_hooks_t lock_hooks = {.now_ms = h_now,
                     .random = h_rng,
                     .send = h_send,
                     .disconnect = h_disconnect,
                     .access = h_access,
                     .bolt = h_bolt,
                     .time_set = h_time_set,
                     .snapshot = h_snapshot};
    ll_init(&world.ll, &world.lock_id, &lock_hooks);
    add_peer(PHONE_SLOT, LP_ROLE_PHONE, &world.phone, PHONE_FILL);
    add_peer(KEYPAD_SLOT, LP_ROLE_KEYPAD, &world.keypad, KEYPAD_FILL);
}

/* Run IK as `role` on `conn`. Leaves the lock's replies in world.out. */
static bool open_session(peer_t *p, uint16_t conn, lp_role_t role)
{
    uint8_t plain[32];
    uint8_t frame[LP_MAX_FRAME];
    size_t out_len = 0;
    lp_msg_t hello = {.type = LP_MSG_HELLO, .hello = {.proto = LP_PROTO_VERSION, .role = (uint8_t)role}};
    size_t plain_len = lp_encode(&hello, plain, sizeof(plain));
    if (sl_handshake_init(&p->hs, SL_PATTERN_IK, SL_INITIATOR, (const uint8_t *)LP_PROLOGUE, LP_PROLOGUE_LEN,
                          &p->id, world.lock_id.pub, h_rng, NULL) != SL_OK ||
        sl_handshake_write(&p->hs, plain, plain_len, frame + 1, sizeof(frame) - 1, &out_len) != SL_OK) {
        return false;
    }
    frame[0] = LP_FRAME_IK1;
    world.nout = 0;
    ll_on_connect(&world.ll, conn);
    ll_on_frame(&world.ll, conn, frame, out_len + 1);
    if (world.nout < 1 || world.out[0].data[0] != LP_FRAME_IK2) {
        return false;
    }
    size_t ack_len = 0;
    return sl_handshake_read(&p->hs, world.out[0].data + 1, world.out[0].len - 1, plain, sizeof(plain), &ack_len) ==
               SL_OK &&
           sl_handshake_split(&p->hs, &p->sess) == SL_OK;
}

static void send_msg(peer_t *p, uint16_t conn, const lp_msg_t *msg)
{
    uint8_t plain[32];
    uint8_t frame[LP_MAX_FRAME];
    size_t ct_len = 0;
    size_t plain_len = lp_encode(msg, plain, sizeof(plain));
    CHECK(plain_len > 0);
    CHECK(sl_session_encrypt(&p->sess, plain, plain_len, frame + 1, sizeof(frame) - 1, &ct_len) == SL_OK);
    frame[0] = LP_FRAME_DATA;
    ll_on_frame(&world.ll, conn, frame, ct_len + 1);
}

/* Decrypt output i (a DATA frame for `p`). */
static bool recv_msg(peer_t *p, int i, lp_msg_t *msg)
{
    uint8_t plain[64];
    size_t plain_len = 0;
    if (i >= world.nout || world.out[i].data[0] != LP_FRAME_DATA) {
        return false;
    }
    return sl_session_decrypt(&p->sess, world.out[i].data + 1, world.out[i].len - 1, plain, sizeof(plain), &plain_len) ==
               SL_OK &&
           lp_decode(plain, plain_len, msg);
}

TEST(test_lp_time_and_state_roundtrip)
{
    enum { USER = 300 };
    uint8_t buf[16];
    lp_msg_t d;
    lp_msg_t t = {.type = LP_MSG_TIME_SET, .time_set = {.unix_s = SOME_UNIX_S, .utc_offset_min = CHICAGO_DST_MIN}};
    size_t n = lp_encode(&t, buf, sizeof(buf));
    CHECK_EQ(n, 7);
    CHECK(lp_decode(buf, n, &d));
    CHECK_EQ(d.time_set.unix_s, SOME_UNIX_S);
    CHECK_EQ(d.time_set.utc_offset_min, CHICAGO_DST_MIN);
    CHECK_EQ(buf[1], (unsigned)SOME_UNIX_S & 0xFFU); /* little-endian */

    t.time_set.utc_offset_min = LP_UTC_OFFSET_MAX + 1;
    CHECK_EQ(lp_encode(&t, buf, sizeof(buf)), 0);
    /* Wire: type, unix_s (4 bytes LE), offset (2 bytes LE). An offset of
     * INT16_MAX minutes encodes fine but must be rejected on decode. */
    enum { OFFSET_AT = 5, TIME_SET_LEN = 7 };
    buf[OFFSET_AT] = (uint8_t)(INT16_MAX & 0xFF);
    buf[OFFSET_AT + 1] = (uint8_t)(INT16_MAX >> 8);
    CHECK(!lp_decode(buf, TIME_SET_LEN, &d));

    lp_msg_t s = {.type = LP_MSG_LOCK_STATE,
                  .lock_state = {.bolt = LP_BOLT_UNLOCKED,
                                 .door = LP_DOOR_CLOSED,
                                 .event = LP_EVENT_UNLOCKED,
                                 .source = LP_SOURCE_KEYPAD,
                                 .user_index = USER}};
    n = lp_encode(&s, buf, sizeof(buf));
    CHECK_EQ(n, 7);
    CHECK(lp_decode(buf, n, &d));
    CHECK_EQ(d.lock_state.user_index, USER);
    buf[1] = LP_BOLT_JAMMED + 1;
    CHECK(!lp_decode(buf, n, &d));
}

TEST(test_phone_gets_snapshot_on_connect)
{
    setup();
    CHECK(open_session(&world.phone, PHONE_CONN, LP_ROLE_PHONE));
    lp_msg_t msg = {0}; /* CHECK does not return on failure */
    CHECK_EQ(world.nout, 2); /* IK2, then LOCK_STATE */
    CHECK(recv_msg(&world.phone, 1, &msg));
    CHECK_EQ(msg.type, LP_MSG_LOCK_STATE);
    CHECK_EQ(msg.lock_state.bolt, LP_BOLT_LOCKED);
    CHECK_EQ(msg.lock_state.door, LP_DOOR_CLOSED);
    CHECK_EQ(msg.lock_state.event, LP_EVENT_NONE);
}

TEST(test_keypad_gets_no_state)
{
    setup();
    CHECK(open_session(&world.keypad, KEYPAD_CONN, LP_ROLE_KEYPAD));
    CHECK_EQ(world.nout, 1); /* IK2 only */
    world.nout = 0;
    lp_msg_t s;
    lock_bridge_state(&world.lc, NULL, &s);
    ll_broadcast_state(&world.ll, &s);
    CHECK_EQ(world.nout, 0);
}

TEST(test_phone_sets_time_keypad_cannot)
{
    setup();
    CHECK(open_session(&world.keypad, KEYPAD_CONN, LP_ROLE_KEYPAD));
    lp_msg_t t = {.type = LP_MSG_TIME_SET, .time_set = {.unix_s = SOME_UNIX_S, .utc_offset_min = CHICAGO_DST_MIN}};
    send_msg(&world.keypad, KEYPAD_CONN, &t);
    CHECK_EQ(world.time_sets, 0);

    CHECK(open_session(&world.phone, PHONE_CONN, LP_ROLE_PHONE));
    send_msg(&world.phone, PHONE_CONN, &t);
    CHECK_EQ(world.time_sets, 1);
    CHECK_EQ(world.time_unix, SOME_UNIX_S);
    CHECK_EQ(world.time_offset, CHICAGO_DST_MIN);
    CHECK_EQ(world.dropped, 0);
}

TEST(test_phone_receives_events)
{
    setup();
    CHECK(open_session(&world.phone, PHONE_CONN, LP_ROLE_PHONE));
    lp_msg_t snapshot;
    CHECK(recv_msg(&world.phone, 1, &snapshot));
    world.nout = 0;

    lc_event_t ev = {.type = LC_EVENT_UNLOCKED, .source = LC_SOURCE_KEYPAD, .user_index = 1};
    world.lc.bolt = LC_BOLT_UNLOCKED;
    lp_msg_t s;
    lock_bridge_state(&world.lc, &ev, &s);
    ll_broadcast_state(&world.ll, &s);
    lp_msg_t msg = {0}; /* CHECK does not return on failure */
    CHECK(recv_msg(&world.phone, 0, &msg));
    CHECK_EQ(msg.lock_state.bolt, LP_BOLT_UNLOCKED);
    CHECK_EQ(msg.lock_state.event, LP_EVENT_UNLOCKED);
    CHECK_EQ(msg.lock_state.source, LP_SOURCE_KEYPAD);
    CHECK_EQ(msg.lock_state.user_index, 1);
}

TEST(test_log_entries_carry_boot)
{
    enum { BOOT = 42 };
    setup();
    world.lc.boot_count = BOOT;
    lc_request(&world.lc, LC_OP_UNLOCK, LC_SOURCE_APP, NULL, 0, NULL);
    lc_on_motor_done(&world.lc, true);
    CHECK(world.lc.log.count >= 1);
    CHECK_EQ(lc_log_get(&world.lc.log, (uint16_t)(world.lc.log.count - 1))->boot, BOOT);
}
