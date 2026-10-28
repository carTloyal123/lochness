/*
 * End-to-end: keypad_core <-> lock_link <-> lock_core over an in-memory
 * transport, using the same bridge code as the lock firmware.
 */
#include <stdint.h>
#include <string.h>

#include "fake_platform.h"
#include "keypad_core/kp.h"
#include "link_proto/lp.h"
#include "lock_bridge.h"
#include "lock_core/lc_creds.h"
#include "lock_core/lc_lock.h"
#include "lock_core/lc_types.h"
#include "lock_link/ll.h"
#include "secure_link/sl_crypto.h"
#include "test.h"

enum {
    CONN = 7,       /* connection handle of the keypad's link */
    ATTACKER_CONN = 99,
    QCAP = 16,      /* frames buffered per direction */
    PAIR_WINDOW_S = 60,
    DEFAULT_LOCKOUT_S = 60, /* lc_config_default() */
    MAX_PUMP_STEPS = 100,
    ADDR_LEN = 6,
};

typedef struct {
    uint8_t data[LP_MAX_FRAME];
    size_t len;
} frame_t;

typedef struct {
    frame_t q[QCAP];
    int head, count;
} fq_t;

static struct {
    fake_t fake; /* shared clock + lock_core fakes */
    lc_t lc;
    ll_t ll;
    kp_t kp;
    sl_keypair_t lock_id, kp_id;
    fq_t to_lock, to_kp;
    bool connected;
    bool connect_requested, disconnect_requested;
    bool lock_dropped;
    bool connect_fails; /* simulate lock out of range */
    kp_feedback_t fbs[32];
    uint16_t fb_detail[32];
    int nfb;
    kp_pairing_t saved;
    int saves;
    uint8_t rng;
    frame_t capture[16];
    int ncapture;
    bool capturing;
} world; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): test fixture state; hooks get no ctx

static void push(fq_t *q, const uint8_t *d, size_t len)
{
    if (q->count == QCAP || len > LP_MAX_FRAME) {
        return;
    }
    frame_t *frame = &q->q[(q->head + q->count++) % QCAP];
    memcpy(frame->data, d, len);
    frame->len = len;
}

static bool pop(fq_t *q, frame_t *out)
{
    if (!q->count) {
        return false;
    }
    *out = q->q[q->head];
    q->head = (q->head + 1) % QCAP;
    q->count--;
    return true;
}

static uint64_t t_now(void *ctx) { (void)ctx; return world.fake.now_ms; }

/* Deterministic, not random: an LCG-style byte sequence. */
static const unsigned RNG_MUL = 167U;
static const unsigned RNG_ADD = 13U;

static bool t_rng(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)((++world.rng * RNG_MUL) + RNG_ADD);
    }
    return true;
}

/* lock_link hooks */
static bool l_send(void *ctx, uint16_t conn, const uint8_t *frame, size_t len)
{
    (void)ctx;
    (void)conn;
    push(&world.to_kp, frame, len);
    return true;
}
static void l_disconnect(void *ctx, uint16_t conn) { (void)ctx; (void)conn; world.lock_dropped = true; }
static ll_access_result_t l_access(void *ctx, lp_role_t role, lp_op_t op, const uint8_t *pin, size_t n)
{
    (void)ctx;
    return lock_bridge_access(&world.lc, role, op, pin, n);
}
static lp_bolt_t l_bolt(void *ctx) { (void)ctx; return lock_bridge_bolt(&world.lc); }

/* keypad hooks */
static void k_connect(void *ctx, const kp_pairing_t *p) { (void)ctx; (void)p; world.connect_requested = true; }
static void k_disconnect(void *ctx) { (void)ctx; world.disconnect_requested = true; }
static bool k_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (world.capturing && world.ncapture < (int)(sizeof(world.capture) / sizeof(world.capture[0]))) {
        memcpy(world.capture[world.ncapture].data, frame, len);
        world.capture[world.ncapture++].len = len;
    }
    push(&world.to_lock, frame, len);
    return true;
}
static void k_feedback(void *ctx, kp_feedback_t feedback, uint16_t detail)
{
    (void)ctx;
    if (world.nfb < (int)(sizeof(world.fbs) / sizeof(world.fbs[0]))) {
        world.fb_detail[world.nfb] = detail;
        world.fbs[world.nfb++] = feedback;
    }
}
static void k_save(void *ctx, const kp_pairing_t *p) { (void)ctx; world.saved = *p; world.saves++; }

/* Deliver everything until quiet, emulating the BLE connection lifecycle. */
static void pump(void)
{
    for (int guard = 0; guard < MAX_PUMP_STEPS; guard++) {
        frame_t frame;
        if (world.connect_requested) {
            world.connect_requested = false;
            if (world.connect_fails) {
                continue; /* keypad times out on its own */
            }
            world.connected = true;
            ll_on_connect(&world.ll, CONN);
            static const uint8_t ADDR[ADDR_LEN] = {1, 2, 3, 4, 5, 6};
            kp_on_connected(&world.kp, ADDR, sizeof(ADDR));
        } else if (world.disconnect_requested || world.lock_dropped) {
            bool from_lock = world.lock_dropped;
            world.disconnect_requested = world.lock_dropped = false;
            if (world.connected) {
                world.connected = false;
                world.to_lock.count = world.to_kp.count = 0;
                if (from_lock) {
                    kp_on_disconnected(&world.kp);
                } else {
                    ll_on_disconnect(&world.ll, CONN);
                }
            }
        } else if (world.connected && pop(&world.to_lock, &frame)) {
            ll_on_frame(&world.ll, CONN, frame.data, frame.len);
        } else if (world.connected && pop(&world.to_kp, &frame)) {
            kp_on_frame(&world.kp, frame.data, frame.len);
        } else {
            return;
        }
    }
}

static void keys(const char *s)
{
    for (; *s; s++) {
        kp_on_key(&world.kp, *s);
        pump();
    }
}

static kp_feedback_t last_fb(void)
{
    /* -1 is deliberately not a kp_feedback_t: "no feedback yet" never equals an expected value. */
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): sentinel outside the enum on purpose
    return world.nfb ? world.fbs[world.nfb - 1] : (kp_feedback_t)-1;
}

static void advance(uint64_t ms)
{
    world.fake.now_ms += ms;
    kp_tick(&world.kp);
    ll_tick(&world.ll);
    lc_tick(&world.lc);
    pump();
}

static void world_init(void)
{
    memset(&world, 0, sizeof(world));
    fake_reset(&world.fake);

    lc_config_t cfg = lc_config_default();
    lc_platform_t plat = fake_platform(&world.fake);
    lc_hooks_t hooks = fake_hooks(&world.fake);
    lc_init(&world.lc, &plat, &hooks, &cfg);
    lc_user_set(&world.lc.creds, 1, LC_USER_UNRESTRICTED, "Alice");
    lc_pin_set(&world.lc.creds, &world.lc.plat, 1, 1, (const uint8_t *)"1234", 4);
    lc_boot(&world.lc, LC_BOLT_LOCKED, LC_DOOR_CLOSED);

    enum { LOCK_KEY_FILL = 'A', KEYPAD_KEY_FILL = 'K' }; /* fixed identities */
    memset(world.lock_id.priv, LOCK_KEY_FILL, 32);
    sl_keypair_from_private(&world.lock_id);
    memset(world.kp_id.priv, KEYPAD_KEY_FILL, 32);
    sl_keypair_from_private(&world.kp_id);

    ll_hooks_t lock_hooks = {.now_ms = t_now, .random = t_rng, .send = l_send, .disconnect = l_disconnect,
                     .access = l_access, .bolt = l_bolt};
    ll_init(&world.ll, &world.lock_id, &lock_hooks);
    kp_hooks_t keypad_hooks = {.now_ms = t_now, .random = t_rng, .connect = k_connect, .disconnect = k_disconnect,
                     .send = k_send, .feedback = k_feedback, .save_pairing = k_save};
    kp_init(&world.kp, &world.kp_id, NULL, &keypad_hooks);
}

static void pair(void)
{
    ll_open_pairing(&world.ll, LP_ROLE_KEYPAD, PAIR_WINDOW_S);
    kp_start_pairing(&world.kp);
    pump();
}

TEST(test_link_pairing_requires_window)
{
    world_init();
    kp_start_pairing(&world.kp);
    pump();
    CHECK_EQ(last_fb(), KP_FB_PAIR_FAILED);
    CHECK(!world.kp.pairing.paired);
    CHECK_EQ(ll_peer_count(&world.ll), 0);
}

TEST(test_link_pair_then_unlock)
{
    world_init();
    pair();
    CHECK_EQ(last_fb(), KP_FB_PAIRED);
    CHECK(world.kp.pairing.paired);
    CHECK_EQ(world.saves, 1);
    CHECK(memcmp(world.saved.lock_pub, world.lock_id.pub, 32) == 0);
    CHECK_EQ(world.saved.lock_addr_len, ADDR_LEN);
    CHECK_EQ(ll_peer_count(&world.ll), 1);
    CHECK(memcmp(world.ll.peers.peers[0].static_pub, world.kp_id.pub, 32) == 0);
    CHECK(!ll_pairing_open(&world.ll)); /* one pairing per window */
    CHECK(!world.connected);

    keys("1234#");
    CHECK_EQ(last_fb(), KP_FB_UNLOCKED);
    CHECK_EQ(world.lc.bolt, LC_BOLT_UNLOCKING);
    CHECK_EQ(world.fake.motor_starts, 1);

    /* The link lingers, then closes, and the keypad can sleep. */
    CHECK(world.connected);
    advance(KP_LINGER_MS);
    CHECK(!world.connected);
    CHECK(kp_idle(&world.kp));
}

TEST(test_link_wrong_pin_and_lockout)
{
    world_init();
    pair();
    for (int i = 0; i < 4; i++) {
        keys("9999#");
        CHECK_EQ(last_fb(), KP_FB_DENIED);
    }
    keys("9999#"); /* fifth wrong code starts the lockout */
    CHECK_EQ(last_fb(), KP_FB_DENIED);
    keys("1234#");
    CHECK_EQ(last_fb(), KP_FB_LOCKED_OUT);
    CHECK_EQ(world.fb_detail[world.nfb - 1], DEFAULT_LOCKOUT_S);
    CHECK_EQ(world.fake.motor_starts, 0);
}

TEST(test_link_one_touch_lock)
{
    world_init();
    pair();
    world.lc.bolt = LC_BOLT_UNLOCKED;
    keys("L");
    CHECK_EQ(last_fb(), KP_FB_LOCKED);
    CHECK_EQ(world.lc.bolt, LC_BOLT_LOCKING);
}

TEST(test_link_unpaired_keypad_rejected)
{
    world_init();
    /* A keypad that knows the lock's public key but was never paired. */
    kp_pairing_t forged = {.paired = true, .lock_addr_len = ADDR_LEN};
    memcpy(forged.lock_pub, world.lock_id.pub, 32);
    world.kp.pairing = forged;
    keys("1234#");
    CHECK_EQ(last_fb(), KP_FB_NO_LINK);
    CHECK_EQ(world.fake.motor_starts, 0);
}

TEST(test_link_removed_peer_rejected)
{
    world_init();
    pair();
    CHECK(ll_peer_remove(&world.ll, 0));
    keys("1234#");
    CHECK_EQ(last_fb(), KP_FB_NO_LINK);
    CHECK_EQ(world.fake.motor_starts, 0);
}

TEST(test_link_replay_rejected)
{
    world_init();
    pair();
    world.capturing = true;
    keys("1234#");
    world.capturing = false;
    CHECK_EQ(world.ncapture, 2); /* IK1 + DATA(access request) */
    CHECK_EQ(world.fake.motor_starts, 1);
    lc_on_motor_done(&world.lc, true);
    advance(KP_LINGER_MS);
    lc_request(&world.lc, LC_OP_LOCK, LC_SOURCE_APP, NULL, 0, NULL);
    lc_on_motor_done(&world.lc, true);
    int starts = world.fake.motor_starts;

    /* Attacker replays the captured frames on a fresh connection. The lock
     * answers IK1 (replayable by design), but the access request was bound to
     * the old session's keys and must fail. */
    ll_on_connect(&world.ll, ATTACKER_CONN);
    world.lock_dropped = false;
    ll_on_frame(&world.ll, ATTACKER_CONN, world.capture[0].data, world.capture[0].len);
    CHECK(!world.lock_dropped);
    ll_on_frame(&world.ll, ATTACKER_CONN, world.capture[1].data, world.capture[1].len);
    CHECK(world.lock_dropped);
    CHECK_EQ(world.fake.motor_starts, starts);
}

TEST(test_link_ciphertext_length_hides_pin)
{
    world_init();
    pair();
    world.capturing = true;
    keys("1234#");
    advance(KP_LINGER_MS);
    keys("12345678#");
    world.capturing = false;
    CHECK_EQ(world.ncapture, 4);
    CHECK_EQ(world.capture[1].len, world.capture[3].len);
    /* Lock and unlock requests are the same size too. */
    world.capturing = true;
    advance(KP_LINGER_MS);
    keys("L");
    CHECK_EQ(world.capture[5].len, world.capture[1].len);
}

TEST(test_link_wrong_role_window)
{
    world_init();
    ll_open_pairing(&world.ll, LP_ROLE_HUB, PAIR_WINDOW_S);
    kp_start_pairing(&world.kp);
    pump();
    CHECK_EQ(last_fb(), KP_FB_PAIR_FAILED);
    CHECK_EQ(ll_peer_count(&world.ll), 0);
}

TEST(test_link_pairing_window_expires)
{
    world_init();
    ll_open_pairing(&world.ll, LP_ROLE_KEYPAD, PAIR_WINDOW_S);
    advance((uint64_t)PAIR_WINDOW_S * 1000U);
    CHECK(!ll_pairing_open(&world.ll));
    kp_start_pairing(&world.kp);
    pump();
    CHECK_EQ(last_fb(), KP_FB_PAIR_FAILED);
}

TEST(test_link_lock_unreachable)
{
    world_init();
    pair();
    world.connect_fails = true;
    keys("1234#");
    CHECK(world.kp.pending);
    advance(KP_LINK_TIMEOUT_MS);
    CHECK_EQ(last_fb(), KP_FB_NO_LINK);
    CHECK(kp_idle(&world.kp));
}

TEST(test_link_entry_timeout_clears_digits)
{
    world_init();
    pair();
    keys("12");
    CHECK(world.connected); /* link comes up while typing */
    advance(KP_ENTRY_TIMEOUT_MS);
    CHECK_EQ(world.kp.ndigits, 0);
    CHECK(!world.connected);
    keys("34#"); /* the old "12" is gone */
    CHECK_EQ(last_fb(), KP_FB_DENIED);
}

TEST(test_link_lock_idle_timeout)
{
    world_init();
    pair();
    keys("1");
    CHECK(world.connected);
    /* A peer that goes quiet is dropped by the lock. */
    world.fake.now_ms += LL_IDLE_TIMEOUT_MS;
    ll_tick(&world.ll);
    pump();
    CHECK(!world.connected);
}

TEST(test_link_too_long_pin_rejected_locally)
{
    world_init();
    pair();
    int sends = world.ncapture;
    world.capturing = true;
    keys("123456789#");
    CHECK_EQ(last_fb(), KP_FB_DENIED);
    /* Only the handshake went out; no access request. */
    CHECK_EQ(world.ncapture - sends, 1);
    CHECK_EQ(world.fake.motor_starts, 0);
}
