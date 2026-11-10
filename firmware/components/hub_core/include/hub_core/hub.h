/*
 * Hub side of the lock link: pairing (Noise XX initiator), a long-lived
 * session to the lock (Noise IK initiator), remote lock/unlock requests, and
 * LOCK_STATE updates from the lock.
 *
 * Unlike the keypad, the hub stays connected: it reconnects with backoff
 * whenever the link drops and keeps the session alive with TIME_SET (or PING
 * while it has no clock) every HUB_KEEPALIVE_MS.
 *
 * Transport-agnostic and not thread-safe; drive it from one task.
 */
#pragma once

#include "link_proto/lp.h"
#include "secure_link/sl_noise.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_ADDR_MAX 8
#define HUB_LINK_TIMEOUT_MS 8000     /* connect + handshake */
#define HUB_PAIR_TIMEOUT_MS 30000    /* scanning for a lock in pairing mode */
#define HUB_RESULT_TIMEOUT_MS 5000
#define HUB_KEEPALIVE_MS 60000       /* well inside the lock's LL_HUB_IDLE_TIMEOUT_MS */
#define HUB_BACKOFF_MIN_MS 500
#define HUB_BACKOFF_MAX_MS 30000

typedef struct {
    bool paired;
    uint8_t lock_pub[SL_KEY_LEN];
    uint8_t lock_addr[HUB_ADDR_MAX]; /* opaque transport address */
    uint8_t lock_addr_len;
} hub_pairing_t;

typedef enum {
    HUB_LINK_DOWN,
    HUB_LINK_WAITING, /* backoff before the next connect */
    HUB_LINK_CONNECTING,
    HUB_LINK_HANDSHAKE,
    HUB_LINK_READY,
} hub_link_t;

typedef struct {
    void *ctx;
    uint64_t (*now_ms)(void *ctx);
    bool (*random)(void *ctx, uint8_t *buf, size_t len);
    /* Connect to `addr`, or (addr == NULL) scan for a lock with an open
     * pairing window. Report with hub_on_connected / hub_on_disconnected. */
    void (*connect)(void *ctx, const uint8_t *addr, size_t addr_len);
    void (*disconnect)(void *ctx);
    bool (*send)(void *ctx, const uint8_t *frame, size_t len);
    void (*save_pairing)(void *ctx, const hub_pairing_t *pairing);
    /* Optional. Current UTC and local offset; false while unknown. */
    bool (*wall_time)(void *ctx, uint32_t *unix_s, int16_t *utc_offset_min);
    /* Optional. The session came up (true) or went down (false). */
    void (*link_changed)(void *ctx, bool is_up);
    /* Optional. LOCK_STATE from the lock (snapshot or event). */
    void (*lock_state)(void *ctx, const lp_msg_t *state);
    /* Optional. Answer to hub_request(): result is an lp_result_t;
     * LP_RESULT_ERROR when the link failed or the lock did not answer. */
    void (*result)(void *ctx, uint8_t seq, uint8_t result, uint8_t bolt);
    /* Optional. Pairing finished (ok) or failed. */
    void (*paired)(void *ctx, bool ok);
} hub_hooks_t;

typedef struct {
    hub_hooks_t hooks;
    sl_keypair_t identity;
    hub_pairing_t pairing;

    hub_link_t link;
    bool pairing_mode;
    sl_handshake_t hs;
    sl_session_t sess;
    uint8_t lock_pub_candidate[SL_KEY_LEN];
    uint8_t conn_addr[HUB_ADDR_MAX];
    uint8_t conn_addr_len;

    bool awaiting; /* one request in flight */
    uint8_t seq;
    uint32_t backoff_ms;

    /* 0 = not armed */
    uint64_t link_deadline, result_deadline, keepalive_at, retry_at;
} hub_t;

void hub_init(hub_t *hub, const sl_keypair_t *identity, const hub_pairing_t *pairing, const hub_hooks_t *hooks);
/* Bring the session up if paired. Call once after init. */
void hub_start(hub_t *hub);
void hub_start_pairing(hub_t *hub);
void hub_forget_pairing(hub_t *hub);

/*
 * Ask the lock to lock or unlock. pin may be NULL (pin_len 0); the lock's
 * remote policy decides whether that is enough. Returns the request's seq
 * (never 0), or 0 when the link is not ready or a request is in flight.
 */
uint8_t hub_request(hub_t *hub, lp_op_t op, const uint8_t *pin, size_t pin_len);
/* Send the wall clock now (e.g. right after it first becomes known). */
void hub_send_time(hub_t *hub);

void hub_on_connected(hub_t *hub, const uint8_t *addr, size_t addr_len);
void hub_on_disconnected(hub_t *hub);
void hub_on_frame(hub_t *hub, const uint8_t *frame, size_t len);

void hub_tick(hub_t *hub);
uint64_t hub_next_deadline_ms(const hub_t *hub);
bool hub_ready(const hub_t *hub);

#ifdef __cplusplus
}
#endif
