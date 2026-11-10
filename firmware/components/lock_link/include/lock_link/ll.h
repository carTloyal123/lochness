/*
 * Lock-side link server: paired-peer table, Noise responder for each
 * connection, and dispatch of authenticated requests to the lock.
 *
 * Transport-agnostic: the platform feeds connection events and frames in and
 * provides send/disconnect hooks. Not thread-safe; drive it from the lock task.
 *
 * Policy:
 *  - Session (IK) handshakes from unknown static keys are dropped silently.
 *  - Pairing (XX) is only accepted while a pairing window, opened by a
 *    physical action on the lock, is open, and only for the window's role.
 *    One pairing per window.
 *  - Any authentication failure drops the connection.
 *  - Only phone sessions may set the clock, and only they receive
 *    LOCK_STATE pushes. Keypads get neither.
 */
#pragma once

#include "link_proto/lp.h"
#include "secure_link/sl_noise.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LL_MAX_PEERS 8
#define LL_MAX_CONNS 3
#define LL_HANDSHAKE_TIMEOUT_MS 5000
#define LL_IDLE_TIMEOUT_MS 30000
#define LL_NO_PEER 0xFF

typedef struct {
    bool in_use;
    uint8_t role; /* lp_role_t */
    uint8_t static_pub[SL_KEY_LEN];
} ll_peer_t;

/* Persisted by the platform as one blob. */
typedef struct {
    ll_peer_t peers[LL_MAX_PEERS];
} ll_peers_t;

typedef struct {
    lp_result_t result;
    lp_bolt_t bolt;
    uint16_t lockout_s;
} ll_access_result_t;

typedef struct {
    void *ctx;
    uint64_t (*now_ms)(void *ctx);
    bool (*random)(void *ctx, uint8_t *buf, size_t len);
    bool (*send)(void *ctx, uint16_t conn, const uint8_t *frame, size_t len);
    /* Close the transport connection. ll has already forgotten it. */
    void (*disconnect)(void *ctx, uint16_t conn);
    /* Peer table changed; persist ll->peers. */
    void (*peers_changed)(void *ctx);
    /* Authenticated lock/unlock request. pin is NULL when pin_len is 0. */
    ll_access_result_t (*access)(void *ctx, lp_role_t role, lp_op_t op, const uint8_t *pin,
                                 size_t pin_len);
    lp_bolt_t (*bolt)(void *ctx);
    /* Optional. */
    void (*keypad_status)(void *ctx, uint8_t peer, uint16_t battery_mv, uint8_t flags);
    void (*paired)(void *ctx, uint8_t peer, lp_role_t role, const uint8_t handshake_hash[SL_HASH_LEN]);
    /* Wall clock from a phone session. */
    void (*time_set)(void *ctx, uint32_t unix_s, int16_t utc_offset_min);
    /* Current state as a LOCK_STATE message (event LP_EVENT_NONE); sent to
     * phone sessions as soon as they are established. */
    void (*snapshot)(void *ctx, lp_msg_t *state);
} ll_hooks_t;

typedef enum {
    LL_CONN_FREE,
    LL_CONN_AWAIT_HELLO, /* expecting IK1 (session) or XX1 (pairing) */
    LL_CONN_AWAIT_XX3,
    LL_CONN_ESTABLISHED,
} ll_conn_state_t;

typedef struct {
    ll_conn_state_t state;
    uint16_t handle;
    uint8_t peer; /* index into peers, or LL_NO_PEER */
    uint8_t role;
    uint64_t deadline_ms;
    sl_handshake_t hs;
    sl_session_t sess;
} ll_conn_t;

typedef struct {
    ll_peers_t peers; /* platform restores after ll_init */
    sl_keypair_t identity;
    ll_hooks_t hooks;
    ll_conn_t conns[LL_MAX_CONNS];
    bool pairing_open;
    uint8_t pairing_role;
    uint64_t pairing_until_ms;
} ll_t;

void ll_init(ll_t *ll, const sl_keypair_t *identity, const ll_hooks_t *hooks);

void ll_open_pairing(ll_t *ll, lp_role_t role, uint32_t seconds);
void ll_close_pairing(ll_t *ll);
bool ll_pairing_open(ll_t *ll);

void ll_on_connect(ll_t *ll, uint16_t handle);
void ll_on_disconnect(ll_t *ll, uint16_t handle);
void ll_on_frame(ll_t *ll, uint16_t handle, const uint8_t *frame, size_t len);

void ll_tick(ll_t *ll);
uint64_t ll_next_deadline_ms(const ll_t *ll);

/* Send a LOCK_STATE message to every established phone session. */
void ll_broadcast_state(ll_t *ll, const lp_msg_t *state);

/* Remove a peer and drop any of its live connections. */
bool ll_peer_remove(ll_t *ll, uint8_t index);
uint8_t ll_peer_count(const ll_t *ll);

#ifdef __cplusplus
}
#endif
