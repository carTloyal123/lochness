/*
 * End-to-end: keypad_core <-> lock_link <-> lock_core over an in-memory
 * transport, using the same bridge code as the lock firmware.
 */
#include <string.h>

#include "fake_platform.h"
#include "keypad_core/kp.h"
#include "lock_bridge.h"
#include "lock_link/ll.h"
#include "test.h"

#define CONN 7
#define QCAP 16

typedef struct {
    uint8_t data[LP_MAX_FRAME];
    size_t len;
} frame_t;

typedef struct {
    frame_t q[QCAP];
    int head, count;
} fq_t;

static struct {
    fake_t f; /* shared clock + lock_core fakes */
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
} w;

static void push(fq_t *q, const uint8_t *d, size_t len)
{
    if (q->count == QCAP || len > LP_MAX_FRAME) {
        return;
    }
    frame_t *fr = &q->q[(q->head + q->count++) % QCAP];
    memcpy(fr->data, d, len);
    fr->len = len;
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

static uint64_t t_now(void *ctx) { (void)ctx; return w.f.now_ms; }

static bool t_rng(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(++w.rng * 167u + 13u);
    }
    return true;
}

/* lock_link hooks */
static bool l_send(void *ctx, uint16_t conn, const uint8_t *fr, size_t len)
{
    (void)ctx;
    (void)conn;
    push(&w.to_kp, fr, len);
    return true;
}
static void l_disconnect(void *ctx, uint16_t conn) { (void)ctx; (void)conn; w.lock_dropped = true; }
static ll_access_result_t l_access(void *ctx, lp_role_t role, lp_op_t op, const uint8_t *pin, size_t n)
{
    (void)ctx;
    return lock_bridge_access(&w.lc, role, op, pin, n);
}
static lp_bolt_t l_bolt(void *ctx) { (void)ctx; return lock_bridge_bolt(&w.lc); }

/* keypad hooks */
static void k_connect(void *ctx, const kp_pairing_t *p) { (void)ctx; (void)p; w.connect_requested = true; }
static void k_disconnect(void *ctx) { (void)ctx; w.disconnect_requested = true; }
static bool k_send(void *ctx, const uint8_t *fr, size_t len)
{
    (void)ctx;
    if (w.capturing && w.ncapture < 16) {
        memcpy(w.capture[w.ncapture].data, fr, len);
        w.capture[w.ncapture++].len = len;
    }
    push(&w.to_lock, fr, len);
    return true;
}
static void k_feedback(void *ctx, kp_feedback_t fb, uint16_t detail)
{
    (void)ctx;
    if (w.nfb < 32) {
        w.fb_detail[w.nfb] = detail;
        w.fbs[w.nfb++] = fb;
    }
}
static void k_save(void *ctx, const kp_pairing_t *p) { (void)ctx; w.saved = *p; w.saves++; }

/* Deliver everything until quiet, emulating the BLE connection lifecycle. */
static void pump(void)
{
    for (int guard = 0; guard < 100; guard++) {
        frame_t fr;
        if (w.connect_requested) {
            w.connect_requested = false;
            if (w.connect_fails) {
                continue; /* keypad times out on its own */
            }
            w.connected = true;
            ll_on_connect(&w.ll, CONN);
            static const uint8_t addr[6] = {1, 2, 3, 4, 5, 6};
            kp_on_connected(&w.kp, addr, sizeof(addr));
        } else if (w.disconnect_requested || w.lock_dropped) {
            bool from_lock = w.lock_dropped;
            w.disconnect_requested = w.lock_dropped = false;
            if (w.connected) {
                w.connected = false;
                w.to_lock.count = w.to_kp.count = 0;
                if (from_lock) {
                    kp_on_disconnected(&w.kp);
                } else {
                    ll_on_disconnect(&w.ll, CONN);
                }
            }
        } else if (w.connected && pop(&w.to_lock, &fr)) {
            ll_on_frame(&w.ll, CONN, fr.data, fr.len);
        } else if (w.connected && pop(&w.to_kp, &fr)) {
            kp_on_frame(&w.kp, fr.data, fr.len);
        } else {
            return;
        }
    }
}

static void keys(const char *s)
{
    for (; *s; s++) {
        kp_on_key(&w.kp, *s);
        pump();
    }
}

static kp_feedback_t last_fb(void) { return w.nfb ? w.fbs[w.nfb - 1] : (kp_feedback_t)-1; }

static void advance(uint64_t ms)
{
    w.f.now_ms += ms;
    kp_tick(&w.kp);
    ll_tick(&w.ll);
    lc_tick(&w.lc);
    pump();
}

static void world_init(void)
{
    memset(&w, 0, sizeof(w));
    fake_reset(&w.f);

    lc_config_t cfg = lc_config_default();
    lc_platform_t plat = fake_platform(&w.f);
    lc_hooks_t hooks = fake_hooks(&w.f);
    lc_init(&w.lc, &plat, &hooks, &cfg);
    lc_user_set(&w.lc.creds, 1, LC_USER_UNRESTRICTED, "Alice");
    lc_pin_set(&w.lc.creds, &w.lc.plat, 1, 1, (const uint8_t *)"1234", 4);
    lc_boot(&w.lc, LC_BOLT_LOCKED, LC_DOOR_CLOSED);

    memset(w.lock_id.priv, 0x41, 32);
    sl_keypair_from_private(&w.lock_id);
    memset(w.kp_id.priv, 0x4b, 32);
    sl_keypair_from_private(&w.kp_id);

    ll_hooks_t lh = {.now_ms = t_now, .random = t_rng, .send = l_send, .disconnect = l_disconnect,
                     .access = l_access, .bolt = l_bolt};
    ll_init(&w.ll, &w.lock_id, &lh);
    kp_hooks_t kh = {.now_ms = t_now, .random = t_rng, .connect = k_connect, .disconnect = k_disconnect,
                     .send = k_send, .feedback = k_feedback, .save_pairing = k_save};
    kp_init(&w.kp, &w.kp_id, NULL, &kh);
}

static void pair(void)
{
    ll_open_pairing(&w.ll, LP_ROLE_KEYPAD, 60);
    kp_start_pairing(&w.kp);
    pump();
}

TEST(test_link_pairing_requires_window)
{
    world_init();
    kp_start_pairing(&w.kp);
    pump();
    CHECK_EQ(last_fb(), KP_FB_PAIR_FAILED);
    CHECK(!w.kp.pairing.paired);
    CHECK_EQ(ll_peer_count(&w.ll), 0);
}

TEST(test_link_pair_then_unlock)
{
    world_init();
    pair();
    CHECK_EQ(last_fb(), KP_FB_PAIRED);
    CHECK(w.kp.pairing.paired);
    CHECK_EQ(w.saves, 1);
    CHECK(memcmp(w.saved.lock_pub, w.lock_id.pub, 32) == 0);
    CHECK_EQ(w.saved.lock_addr_len, 6);
    CHECK_EQ(ll_peer_count(&w.ll), 1);
    CHECK(memcmp(w.ll.peers.peers[0].static_pub, w.kp_id.pub, 32) == 0);
    CHECK(!ll_pairing_open(&w.ll)); /* one pairing per window */
    CHECK(!w.connected);

    keys("1234#");
    CHECK_EQ(last_fb(), KP_FB_UNLOCKED);
    CHECK_EQ(w.lc.bolt, LC_BOLT_UNLOCKING);
    CHECK_EQ(w.f.motor_starts, 1);

    /* The link lingers, then closes, and the keypad can sleep. */
    CHECK(w.connected);
    advance(KP_LINGER_MS);
    CHECK(!w.connected);
    CHECK(kp_idle(&w.kp));
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
    CHECK_EQ(w.fb_detail[w.nfb - 1], 60);
    CHECK_EQ(w.f.motor_starts, 0);
}

TEST(test_link_one_touch_lock)
{
    world_init();
    pair();
    w.lc.bolt = LC_BOLT_UNLOCKED;
    keys("L");
    CHECK_EQ(last_fb(), KP_FB_LOCKED);
    CHECK_EQ(w.lc.bolt, LC_BOLT_LOCKING);
}

TEST(test_link_unpaired_keypad_rejected)
{
    world_init();
    /* A keypad that knows the lock's public key but was never paired. */
    kp_pairing_t forged = {.paired = true, .lock_addr_len = 6};
    memcpy(forged.lock_pub, w.lock_id.pub, 32);
    w.kp.pairing = forged;
    keys("1234#");
    CHECK_EQ(last_fb(), KP_FB_NO_LINK);
    CHECK_EQ(w.f.motor_starts, 0);
}

TEST(test_link_removed_peer_rejected)
{
    world_init();
    pair();
    CHECK(ll_peer_remove(&w.ll, 0));
    keys("1234#");
    CHECK_EQ(last_fb(), KP_FB_NO_LINK);
    CHECK_EQ(w.f.motor_starts, 0);
}

TEST(test_link_replay_rejected)
{
    world_init();
    pair();
    w.capturing = true;
    keys("1234#");
    w.capturing = false;
    CHECK_EQ(w.ncapture, 2); /* IK1 + DATA(access request) */
    CHECK_EQ(w.f.motor_starts, 1);
    lc_on_motor_done(&w.lc, true);
    advance(KP_LINGER_MS);
    lc_request(&w.lc, LC_OP_LOCK, LC_SOURCE_APP, NULL, 0, NULL);
    lc_on_motor_done(&w.lc, true);
    int starts = w.f.motor_starts;

    /* Attacker replays the captured frames on a fresh connection. The lock
     * answers IK1 (replayable by design), but the access request was bound to
     * the old session's keys and must fail. */
    ll_on_connect(&w.ll, 99);
    w.lock_dropped = false;
    ll_on_frame(&w.ll, 99, w.capture[0].data, w.capture[0].len);
    CHECK(!w.lock_dropped);
    ll_on_frame(&w.ll, 99, w.capture[1].data, w.capture[1].len);
    CHECK(w.lock_dropped);
    CHECK_EQ(w.f.motor_starts, starts);
}

TEST(test_link_ciphertext_length_hides_pin)
{
    world_init();
    pair();
    w.capturing = true;
    keys("1234#");
    advance(KP_LINGER_MS);
    keys("12345678#");
    w.capturing = false;
    CHECK_EQ(w.ncapture, 4);
    CHECK_EQ(w.capture[1].len, w.capture[3].len);
    /* Lock and unlock requests are the same size too. */
    w.capturing = true;
    advance(KP_LINGER_MS);
    keys("L");
    CHECK_EQ(w.capture[5].len, w.capture[1].len);
}

TEST(test_link_wrong_role_window)
{
    world_init();
    ll_open_pairing(&w.ll, LP_ROLE_HUB, 60);
    kp_start_pairing(&w.kp);
    pump();
    CHECK_EQ(last_fb(), KP_FB_PAIR_FAILED);
    CHECK_EQ(ll_peer_count(&w.ll), 0);
}

TEST(test_link_pairing_window_expires)
{
    world_init();
    ll_open_pairing(&w.ll, LP_ROLE_KEYPAD, 60);
    advance(60000);
    CHECK(!ll_pairing_open(&w.ll));
    kp_start_pairing(&w.kp);
    pump();
    CHECK_EQ(last_fb(), KP_FB_PAIR_FAILED);
}

TEST(test_link_lock_unreachable)
{
    world_init();
    pair();
    w.connect_fails = true;
    keys("1234#");
    CHECK(w.kp.pending);
    advance(KP_LINK_TIMEOUT_MS);
    CHECK_EQ(last_fb(), KP_FB_NO_LINK);
    CHECK(kp_idle(&w.kp));
}

TEST(test_link_entry_timeout_clears_digits)
{
    world_init();
    pair();
    keys("12");
    CHECK(w.connected); /* link comes up while typing */
    advance(KP_ENTRY_TIMEOUT_MS);
    CHECK_EQ(w.kp.ndigits, 0);
    CHECK(!w.connected);
    keys("34#"); /* the old "12" is gone */
    CHECK_EQ(last_fb(), KP_FB_DENIED);
}

TEST(test_link_lock_idle_timeout)
{
    world_init();
    pair();
    keys("1");
    CHECK(w.connected);
    /* A peer that goes quiet is dropped by the lock. */
    w.f.now_ms += LL_IDLE_TIMEOUT_MS;
    ll_tick(&w.ll);
    pump();
    CHECK(!w.connected);
}

TEST(test_link_too_long_pin_rejected_locally)
{
    world_init();
    pair();
    int sends = w.ncapture;
    w.capturing = true;
    keys("123456789#");
    CHECK_EQ(last_fb(), KP_FB_DENIED);
    /* Only the handshake went out; no access request. */
    CHECK_EQ(w.ncapture - sends, 1);
    CHECK_EQ(w.f.motor_starts, 0);
}
