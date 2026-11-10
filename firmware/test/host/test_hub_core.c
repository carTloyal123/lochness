/*
 * End-to-end: hub_core <-> lock_link <-> lock_core over an in-memory
 * transport, using the lock firmware's bridge code.
 */
#include <stdint.h>
#include <string.h>

#include "fake_platform.h"
#include "hub_core/hub.h"
#include "link_proto/lp.h"
#include "lock_bridge.h"
#include "lock_core/lc_creds.h"
#include "lock_core/lc_lock.h"
#include "lock_core/lc_log.h"
#include "lock_core/lc_types.h"
#include "lock_link/ll.h"
#include "secure_link/sl_crypto.h"
#include "test.h"

enum {
    CONN = 11,
    QCAP = 16,
    PAIR_WINDOW_S = 60,
    MAX_PUMP_STEPS = 200,
    ADDR_LEN = 7,
    SOME_UNIX_S = 1800000000,
    OFFSET_MIN = -360,
};

typedef struct {
    uint8_t data[LP_MAX_FRAME];
    size_t len;
} hframe_t;

typedef struct {
    hframe_t q[QCAP];
    int head, count;
} hq_t;

static struct {
    fake_t fake;
    lc_t lc;
    ll_t ll;
    hub_t hub;
    sl_keypair_t lock_id, hub_id;
    hq_t to_lock, to_hub;
    bool connected, connect_requested, disconnect_requested, lock_dropped, connect_fails;
    bool scan_requested;
    int connects;
    hub_pairing_t saved;
    int saves;
    int link_ups, link_downs;
    lp_msg_t last_state;
    int states;
    int results;
    uint8_t last_seq, last_result, last_bolt;
    int paired_ok, paired_fail;
    bool wall_known;
    uint32_t lock_time_unix;
    int16_t lock_time_offset;
    int lock_time_sets;
    uint8_t rng;
} world; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): test fixture state; hooks get no ctx

static void hpush(hq_t *q, const uint8_t *d, size_t len)
{
    if (q->count == QCAP || len > LP_MAX_FRAME) {
        return;
    }
    hframe_t *frame = &q->q[(q->head + q->count++) % QCAP];
    memcpy(frame->data, d, len);
    frame->len = len;
}

static bool hpop(hq_t *q, hframe_t *out)
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

static bool t_rng(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    enum { MUL = 173U, ADD = 29U };
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)((++world.rng * MUL) + ADD);
    }
    return true;
}

/* lock side */
static bool l_send(void *ctx, uint16_t conn, const uint8_t *frame, size_t len)
{
    (void)ctx;
    (void)conn;
    hpush(&world.to_hub, frame, len);
    return true;
}
static void l_disconnect(void *ctx, uint16_t conn) { (void)ctx; (void)conn; world.lock_dropped = true; }
static void l_peers_changed(void *ctx) { (void)ctx; }
static ll_access_result_t l_access(void *ctx, lp_role_t role, lp_op_t op, const uint8_t *pin, size_t n)
{
    (void)ctx;
    return lock_bridge_access(&world.lc, role, op, pin, n);
}
static lp_bolt_t l_bolt(void *ctx) { (void)ctx; return lock_bridge_bolt(&world.lc); }
static void l_time(void *ctx, uint32_t unix_s, int16_t offset)
{
    (void)ctx;
    world.lock_time_sets++;
    world.lock_time_unix = unix_s;
    world.lock_time_offset = offset;
}
static void l_snapshot(void *ctx, lp_msg_t *state) { (void)ctx; lock_bridge_state(&world.lc, NULL, state); }

/* hub side */
static void h_connect(void *ctx, const uint8_t *addr, size_t len)
{
    (void)ctx;
    (void)len;
    world.connect_requested = true;
    world.scan_requested = addr == NULL;
    world.connects++;
}
static void h_disconnect(void *ctx) { (void)ctx; world.disconnect_requested = true; }
static bool h_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    hpush(&world.to_lock, frame, len);
    return true;
}
static void h_save(void *ctx, const hub_pairing_t *p) { (void)ctx; world.saved = *p; world.saves++; }
static bool h_wall(void *ctx, uint32_t *unix_s, int16_t *offset)
{
    (void)ctx;
    if (!world.wall_known) {
        return false;
    }
    *unix_s = SOME_UNIX_S + (uint32_t)(world.fake.now_ms / 1000U);
    *offset = OFFSET_MIN;
    return true;
}
static void h_link(void *ctx, bool is_up)
{
    (void)ctx;
    if (is_up) {
        world.link_ups++;
    } else {
        world.link_downs++;
    }
}
static void h_state(void *ctx, const lp_msg_t *s) { (void)ctx; world.last_state = *s; world.states++; }
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): mirrors hub_hooks_t.result
static void h_result(void *ctx, uint8_t seq, uint8_t result, uint8_t bolt)
{
    (void)ctx;
    world.results++;
    world.last_seq = seq;
    world.last_result = result;
    world.last_bolt = bolt;
}
static void h_paired(void *ctx, bool ok)
{
    (void)ctx;
    if (ok) {
        world.paired_ok++;
    } else {
        world.paired_fail++;
    }
}

static void hpump(void)
{
    for (int guard = 0; guard < MAX_PUMP_STEPS; guard++) {
        hframe_t frame;
        if (world.connect_requested) {
            world.connect_requested = false;
            if (world.connect_fails) {
                continue;
            }
            world.connected = true;
            ll_on_connect(&world.ll, CONN);
            static const uint8_t ADDR[ADDR_LEN] = {0, 1, 2, 3, 4, 5, 6};
            hub_on_connected(&world.hub, ADDR, sizeof(ADDR));
        } else if (world.disconnect_requested || world.lock_dropped) {
            bool from_lock = world.lock_dropped;
            world.disconnect_requested = world.lock_dropped = false;
            if (world.connected) {
                world.connected = false;
                world.to_lock.count = world.to_hub.count = 0;
                if (from_lock) {
                    hub_on_disconnected(&world.hub);
                } else {
                    ll_on_disconnect(&world.ll, CONN);
                }
            }
        } else if (world.connected && hpop(&world.to_lock, &frame)) {
            ll_on_frame(&world.ll, CONN, frame.data, frame.len);
        } else if (world.connected && hpop(&world.to_hub, &frame)) {
            hub_on_frame(&world.hub, frame.data, frame.len);
        } else {
            return;
        }
    }
}

static void hadvance(uint64_t ms)
{
    world.fake.now_ms += ms;
    hub_tick(&world.hub);
    ll_tick(&world.ll);
    lc_tick(&world.lc);
    hpump();
}

static void hworld_init(void)
{
    memset(&world, 0, sizeof(world));
    fake_reset(&world.fake);
    lc_config_t cfg = lc_config_default();
    cfg.remote_unlock_allowed = true;
    lc_platform_t plat = fake_platform(&world.fake);
    lc_hooks_t hooks = fake_hooks(&world.fake);
    lc_init(&world.lc, &plat, &hooks, &cfg);
    lc_user_set(&world.lc.creds, 1, LC_USER_UNRESTRICTED, "Alice");
    lc_pin_set(&world.lc.creds, &world.lc.plat, 1, 1, (const uint8_t *)"1234", 4);
    lc_boot(&world.lc, LC_BOLT_LOCKED, LC_DOOR_CLOSED);

    enum { LOCK_FILL = 'Q', HUB_FILL = 'U' };
    memset(world.lock_id.priv, LOCK_FILL, SL_KEY_LEN);
    sl_keypair_from_private(&world.lock_id);
    memset(world.hub_id.priv, HUB_FILL, SL_KEY_LEN);
    sl_keypair_from_private(&world.hub_id);

    ll_hooks_t lock_hooks = {.now_ms = t_now,
                     .random = t_rng,
                     .send = l_send,
                     .disconnect = l_disconnect,
                     .peers_changed = l_peers_changed,
                     .access = l_access,
                     .bolt = l_bolt,
                     .time_set = l_time,
                     .snapshot = l_snapshot};
    ll_init(&world.ll, &world.lock_id, &lock_hooks);
    hub_hooks_t hub_hooks = {.now_ms = t_now,
                      .random = t_rng,
                      .connect = h_connect,
                      .disconnect = h_disconnect,
                      .send = h_send,
                      .save_pairing = h_save,
                      .wall_time = h_wall,
                      .link_changed = h_link,
                      .lock_state = h_state,
                      .result = h_result,
                      .paired = h_paired};
    hub_init(&world.hub, &world.hub_id, NULL, &hub_hooks);
}

static void hub_pair(void)
{
    ll_open_pairing(&world.ll, LP_ROLE_HUB, PAIR_WINDOW_S);
    hub_start_pairing(&world.hub);
    hpump();
}

TEST(test_hubcore_pair_keeps_session)
{
    hworld_init();
    hub_pair();
    CHECK_EQ(world.paired_ok, 1);
    CHECK_EQ(world.saves, 1);
    CHECK(world.saved.paired);
    CHECK(memcmp(world.saved.lock_pub, world.lock_id.pub, SL_KEY_LEN) == 0);
    CHECK_EQ(world.ll.peers.peers[0].role, LP_ROLE_HUB);
    CHECK(hub_ready(&world.hub));
    CHECK(world.connected);
    CHECK_EQ(world.link_ups, 1);
    CHECK_EQ(world.states, 1); /* snapshot after PAIR_RESULT */
    CHECK_EQ(world.last_state.lock_state.bolt, LP_BOLT_LOCKED);
}

TEST(test_hubcore_pairing_needs_hub_window)
{
    hworld_init();
    ll_open_pairing(&world.ll, LP_ROLE_KEYPAD, PAIR_WINDOW_S);
    hub_start_pairing(&world.hub);
    hpump();
    CHECK_EQ(world.paired_fail, 1);
    CHECK(!world.hub.pairing.paired);
    CHECK_EQ(ll_peer_count(&world.ll), 0);
    CHECK(!hub_ready(&world.hub));
}

TEST(test_hubcore_remote_unlock_and_events)
{
    hworld_init();
    hub_pair();
    uint8_t seq = hub_request(&world.hub, LP_OP_UNLOCK, NULL, 0);
    CHECK(seq != 0);
    hpump();
    CHECK_EQ(world.results, 1);
    CHECK_EQ(world.last_seq, seq);
    CHECK_EQ(world.last_result, LP_RESULT_OK);
    CHECK_EQ(world.lc.bolt, LC_BOLT_UNLOCKING);
    /* The lock pushes the moving state and, once the motor is done, the event. */
    lc_on_motor_done(&world.lc, true);
    lp_msg_t s;
    lock_bridge_state(&world.lc, &(lc_event_t){.type = LC_EVENT_UNLOCKED, .source = LC_SOURCE_REMOTE}, &s);
    ll_broadcast_state(&world.ll, &s);
    hpump();
    CHECK_EQ(world.last_state.lock_state.bolt, LP_BOLT_UNLOCKED);
    CHECK_EQ(world.last_state.lock_state.event, LP_EVENT_UNLOCKED);
    CHECK_EQ(world.last_state.lock_state.source, LP_SOURCE_REMOTE);
}

TEST(test_hubcore_remote_unlock_policy_enforced)
{
    hworld_init();
    world.lc.cfg.remote_unlock_allowed = false;
    hub_pair();
    CHECK(hub_request(&world.hub, LP_OP_UNLOCK, NULL, 0) != 0);
    hpump();
    CHECK_EQ(world.last_result, LP_RESULT_MODE);
    CHECK_EQ(world.lc.bolt, LC_BOLT_LOCKED);
    /* Locking is still allowed. */
    world.lc.bolt = LC_BOLT_UNLOCKED;
    CHECK(hub_request(&world.hub, LP_OP_LOCK, NULL, 0) != 0);
    hpump();
    CHECK_EQ(world.last_result, LP_RESULT_OK);
}

TEST(test_hubcore_one_request_at_a_time)
{
    hworld_init();
    hub_pair();
    CHECK(hub_request(&world.hub, LP_OP_UNLOCK, NULL, 0) != 0);
    CHECK_EQ(hub_request(&world.hub, LP_OP_LOCK, NULL, 0), 0);
    hpump();
    CHECK_EQ(world.results, 1);
}

TEST(test_hubcore_sends_time_and_keepalive)
{
    hworld_init();
    world.wall_known = true;
    hub_pair();
    CHECK_EQ(world.lock_time_sets, 1); /* right after the session came up */
    CHECK_EQ(world.lock_time_offset, OFFSET_MIN);
    /* Six keepalives span 6 min, past the lock's 5 min hub idle timeout. */
    enum { KEEPALIVES = 6 };
    for (int i = 0; i < KEEPALIVES; i++) {
        hadvance(HUB_KEEPALIVE_MS);
    }
    CHECK_EQ(world.lock_time_sets, 1 + KEEPALIVES);
    CHECK(hub_ready(&world.hub)); /* the lock never idled it out */
    CHECK_EQ(world.link_downs, 0);
}

TEST(test_hubcore_ping_without_clock_keeps_session)
{
    hworld_init();
    hub_pair();
    CHECK_EQ(world.lock_time_sets, 0);
    hadvance(LL_HUB_IDLE_TIMEOUT_MS - 1);
    hadvance(LL_HUB_IDLE_TIMEOUT_MS - 1);
    CHECK(hub_ready(&world.hub));
    CHECK_EQ(world.link_downs, 0);
}

TEST(test_hubcore_reconnects_with_backoff)
{
    hworld_init();
    hub_pair();
    int connects = world.connects;
    /* Lock goes away: both ends see the link drop. */
    world.connect_fails = true;
    ll_on_disconnect(&world.ll, CONN);
    world.lock_dropped = true;
    hpump();
    CHECK_EQ(world.link_downs, 1);
    CHECK(!hub_ready(&world.hub));
    hadvance(HUB_BACKOFF_MIN_MS);
    CHECK_EQ(world.connects, connects + 1);
    hadvance(HUB_LINK_TIMEOUT_MS); /* that attempt times out */
    hadvance(HUB_BACKOFF_MIN_MS);  /* backoff doubled: not yet */
    CHECK_EQ(world.connects, connects + 1);
    hadvance(HUB_BACKOFF_MIN_MS);
    CHECK_EQ(world.connects, connects + 2);
    /* Lock is back: the next attempt succeeds and resets the backoff. */
    world.connect_fails = false;
    hadvance(HUB_LINK_TIMEOUT_MS);
    hadvance(HUB_BACKOFF_MAX_MS);
    CHECK(hub_ready(&world.hub));
    CHECK_EQ(world.hub.backoff_ms, HUB_BACKOFF_MIN_MS);
    CHECK_EQ(world.link_ups, 2);
}

TEST(test_hubcore_request_fails_when_link_drops)
{
    hworld_init();
    hub_pair();
    world.connect_fails = true;
    CHECK(hub_request(&world.hub, LP_OP_LOCK, NULL, 0) != 0);
    world.to_lock.count = 0; /* request lost in the air */
    ll_on_disconnect(&world.ll, CONN);
    world.lock_dropped = true;
    hpump();
    CHECK_EQ(world.results, 1);
    CHECK_EQ(world.last_result, LP_RESULT_ERROR);
    CHECK_EQ(hub_request(&world.hub, LP_OP_LOCK, NULL, 0), 0); /* not ready */
}

TEST(test_hubcore_removed_hub_cannot_reconnect)
{
    hworld_init();
    hub_pair();
    CHECK(ll_peer_remove(&world.ll, 0));
    hpump();
    CHECK(!hub_ready(&world.hub));
    hadvance(HUB_BACKOFF_MIN_MS);
    CHECK(!hub_ready(&world.hub)); /* IK from an unknown key is dropped */
    CHECK_EQ(world.lc.bolt, LC_BOLT_LOCKED);
}
