/*
 * Keypad state machine: key entry, link to the lock (Noise IK initiator),
 * pairing (Noise XX initiator), and user feedback.
 *
 * The keypad holds no PINs at rest and makes no access decisions. It buffers
 * digits in RAM, sends one fixed-size encrypted attempt, and wipes it.
 *
 * Flow: the first key press starts connecting, so the link is usually up by
 * the time the user presses enter. Results arrive as feedback events.
 *
 * Transport-agnostic and not thread-safe; drive it from one task.
 */
#pragma once

#include "link_proto/lp.h"
#include "secure_link/sl_noise.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KP_ADDR_MAX 8
#define KP_ENTRY_TIMEOUT_MS 10000 /* digits cleared after this much inactivity */
#define KP_LINK_TIMEOUT_MS 6000   /* connect + handshake */
#define KP_PAIR_TIMEOUT_MS 30000  /* scanning for a lock in pairing mode */
#define KP_RESULT_TIMEOUT_MS 5000
#define KP_LINGER_MS 3000         /* keep the link briefly after a result */

/* Keys: '0'-'9', KP_KEY_CLEAR, KP_KEY_ENTER (unlock), KP_KEY_LOCK. */
#define KP_KEY_CLEAR '*'
#define KP_KEY_ENTER '#'
#define KP_KEY_LOCK 'L'

typedef struct {
    bool paired;
    uint8_t lock_pub[SL_KEY_LEN];
    uint8_t lock_addr[KP_ADDR_MAX]; /* opaque transport address */
    uint8_t lock_addr_len;
} kp_pairing_t;

typedef enum {
    KP_FB_KEY,        /* key accepted (click) */
    KP_FB_CLEARED,
    KP_FB_UNLOCKED,
    KP_FB_LOCKED,
    KP_FB_DENIED,     /* wrong code */
    KP_FB_LOCKED_OUT, /* detail = seconds remaining */
    KP_FB_REFUSED,    /* valid but not now: schedule, mode, door open, busy */
    KP_FB_NO_LINK,    /* could not reach the lock */
    KP_FB_NOT_PAIRED,
    KP_FB_PAIRING,    /* searching for a lock in pairing mode */
    KP_FB_PAIRED,
    KP_FB_PAIR_FAILED,
} kp_feedback_t;

typedef struct {
    void *ctx;
    uint64_t (*now_ms)(void *ctx);
    bool (*random)(void *ctx, uint8_t *buf, size_t len);
    /* Connect to the paired lock, or (pairing == NULL) scan for a lock with
     * an open pairing window. Report with kp_on_connected/kp_on_disconnected. */
    void (*connect)(void *ctx, const kp_pairing_t *pairing);
    void (*disconnect)(void *ctx);
    bool (*send)(void *ctx, const uint8_t *frame, size_t len);
    void (*feedback)(void *ctx, kp_feedback_t kind, uint16_t detail);
    void (*save_pairing)(void *ctx, const kp_pairing_t *pairing);
    uint16_t (*battery_mv)(void *ctx); /* optional */
} kp_hooks_t;

typedef enum {
    KP_LINK_DOWN,
    KP_LINK_CONNECTING,
    KP_LINK_HANDSHAKE,
    KP_LINK_READY,
} kp_link_t;

typedef struct {
    kp_hooks_t hooks;
    sl_keypair_t identity;
    kp_pairing_t pairing;
    uint8_t fw_major, fw_minor;

    kp_link_t link;
    bool pairing_mode;
    sl_handshake_t hs;
    sl_session_t sess;
    uint8_t lock_pub_candidate[SL_KEY_LEN]; /* learned during XX */
    uint8_t conn_addr[KP_ADDR_MAX];
    uint8_t conn_addr_len;

    uint8_t digits[LP_PIN_FIELD_LEN];
    uint8_t ndigits;
    bool overflow;

    bool pending; /* request waiting for the link */
    uint8_t pending_op;
    uint8_t pending_pin[LP_PIN_FIELD_LEN];
    uint8_t pending_len;
    bool awaiting_result;
    uint8_t last_op;
    uint8_t seq;

    /* 0 = not armed */
    uint64_t entry_deadline, link_deadline, result_deadline, linger_deadline;
} kp_t;

void kp_init(kp_t *kp, const sl_keypair_t *identity, const kp_pairing_t *pairing, const kp_hooks_t *hooks);
void kp_on_key(kp_t *kp, char key);
void kp_start_pairing(kp_t *kp);
void kp_forget_pairing(kp_t *kp);

void kp_on_connected(kp_t *kp, const uint8_t *addr, size_t addr_len);
void kp_on_disconnected(kp_t *kp);
void kp_on_frame(kp_t *kp, const uint8_t *frame, size_t len);

void kp_tick(kp_t *kp);
uint64_t kp_next_deadline_ms(const kp_t *kp);
/* True when nothing is in progress and the keypad may deep-sleep. */
bool kp_idle(const kp_t *kp);

#ifdef __cplusplus
}
#endif
