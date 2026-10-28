#include "keypad_core/kp.h"
#include "link_proto/lp.h"
#include "secure_link/sl_crypto.h"
#include "secure_link/sl_noise.h"

#include <stdint.h>
#include <string.h>

static uint64_t now_ms(const kp_t *kp) { return kp->hooks.now_ms(kp->hooks.ctx); }

static bool rng(void *ctx, uint8_t *buf, size_t len)
{
    kp_t *kp = ctx;
    return kp->hooks.random(kp->hooks.ctx, buf, len);
}

static void feedback(kp_t *kp, kp_feedback_t kind, uint16_t detail)
{
    kp->hooks.feedback(kp->hooks.ctx, kind, detail);
}

static void clear_digits(kp_t *kp)
{
    sl_wipe(kp->digits, sizeof(kp->digits));
    kp->ndigits = 0;
    kp->overflow = false;
    kp->entry_deadline = 0;
}

static void clear_pending(kp_t *kp)
{
    sl_wipe(kp->pending_pin, sizeof(kp->pending_pin));
    kp->pending = false;
    kp->pending_len = 0;
    kp->awaiting_result = false;
    kp->result_deadline = 0;
}

static void reset_link(kp_t *kp)
{
    sl_handshake_wipe(&kp->hs);
    sl_session_wipe(&kp->sess);
    kp->link = KP_LINK_DOWN;
    kp->link_deadline = 0;
    kp->linger_deadline = 0;
}

/* Tear down the link deliberately (no error feedback). */
static void close_link(kp_t *kp)
{
    if (kp->link != KP_LINK_DOWN) {
        kp->hooks.disconnect(kp->hooks.ctx);
    }
    reset_link(kp);
}

/* Tear down after an error and tell the user if they were waiting on us. */
static void fail_link(kp_t *kp)
{
    if (kp->pairing_mode) {
        feedback(kp, KP_FB_PAIR_FAILED, 0);
        kp->pairing_mode = false;
    } else if (kp->pending || kp->awaiting_result) {
        feedback(kp, KP_FB_NO_LINK, 0);
    }
    clear_pending(kp);
    close_link(kp);
}

void kp_init(kp_t *kp, const sl_keypair_t *identity, const kp_pairing_t *pairing, const kp_hooks_t *hooks)
{
    memset(kp, 0, sizeof(*kp));
    kp->identity = *identity;
    if (pairing) {
        kp->pairing = *pairing;
    }
    kp->hooks = *hooks;
}

static void ensure_link(kp_t *kp)
{
    if (kp->link != KP_LINK_DOWN || !kp->pairing.paired) {
        return;
    }
    kp->link = KP_LINK_CONNECTING;
    kp->link_deadline = now_ms(kp) + KP_LINK_TIMEOUT_MS;
    kp->hooks.connect(kp->hooks.ctx, &kp->pairing);
}

static bool send_msg(kp_t *kp, const lp_msg_t *msg)
{
    uint8_t plain[32];
    uint8_t frame[LP_MAX_FRAME];
    size_t plain_len = lp_encode(msg, plain, sizeof(plain));
    size_t ct_len = 0;
    bool ok = plain_len &&
              sl_session_encrypt(&kp->sess, plain, plain_len, frame + 1, sizeof(frame) - 1, &ct_len) == SL_OK;
    sl_wipe(plain, sizeof(plain));
    if (!ok) {
        return false;
    }
    frame[0] = LP_FRAME_DATA;
    return kp->hooks.send(kp->hooks.ctx, frame, ct_len + 1);
}

static void send_pending(kp_t *kp)
{
    lp_msg_t req = {.type = LP_MSG_ACCESS_REQ,
                    .access_req = {.seq = ++kp->seq, .op = kp->pending_op, .pin_len = kp->pending_len}};
    memcpy(req.access_req.pin, kp->pending_pin, kp->pending_len);
    kp->last_op = kp->pending_op;
    bool ok = send_msg(kp, &req);
    lp_msg_wipe(&req);
    sl_wipe(kp->pending_pin, sizeof(kp->pending_pin));
    kp->pending = false;
    if (!ok) {
        kp->awaiting_result = true; /* so fail_link reports NO_LINK */
        fail_link(kp);
        return;
    }
    kp->awaiting_result = true;
    kp->result_deadline = now_ms(kp) + KP_RESULT_TIMEOUT_MS;
    kp->linger_deadline = 0;
}

static void submit(kp_t *kp, lp_op_t op)
{
    if (kp->pending || kp->awaiting_result) {
        return;
    }
    if (!kp->pairing.paired) {
        clear_digits(kp);
        feedback(kp, KP_FB_NOT_PAIRED, 0);
        return;
    }
    if (kp->overflow) {
        /* Longer than any valid PIN: reject locally, nothing to send. */
        clear_digits(kp);
        feedback(kp, KP_FB_DENIED, 0);
        return;
    }
    kp->pending = true;
    kp->pending_op = (uint8_t)op;
    kp->pending_len = kp->ndigits;
    memcpy(kp->pending_pin, kp->digits, kp->ndigits);
    clear_digits(kp);
    ensure_link(kp);
    if (kp->link == KP_LINK_READY) {
        send_pending(kp);
    }
}

void kp_on_key(kp_t *kp, char key)
{
    if (kp->pairing_mode || kp->awaiting_result || kp->pending) {
        return;
    }
    if (key >= '0' && key <= '9') {
        if (kp->ndigits < LP_PIN_FIELD_LEN) {
            kp->digits[kp->ndigits++] = (uint8_t)key;
        } else {
            kp->overflow = true;
        }
        kp->entry_deadline = now_ms(kp) + KP_ENTRY_TIMEOUT_MS;
        feedback(kp, KP_FB_KEY, 0);
        ensure_link(kp);
    } else if (key == KP_KEY_CLEAR) {
        clear_digits(kp);
        feedback(kp, KP_FB_CLEARED, 0);
    } else if (key == KP_KEY_ENTER) {
        if (kp->ndigits || kp->overflow) {
            submit(kp, LP_OP_UNLOCK);
        }
    } else if (key == KP_KEY_LOCK) {
        submit(kp, LP_OP_LOCK);
    }
}

void kp_start_pairing(kp_t *kp)
{
    close_link(kp);
    clear_digits(kp);
    clear_pending(kp);
    kp->pairing_mode = true;
    kp->link = KP_LINK_CONNECTING;
    kp->link_deadline = now_ms(kp) + KP_PAIR_TIMEOUT_MS;
    feedback(kp, KP_FB_PAIRING, 0);
    kp->hooks.connect(kp->hooks.ctx, NULL);
}

void kp_forget_pairing(kp_t *kp)
{
    close_link(kp);
    sl_wipe(&kp->pairing, sizeof(kp->pairing));
    kp->hooks.save_pairing(kp->hooks.ctx, &kp->pairing);
}

void kp_on_connected(kp_t *kp, const uint8_t *addr, size_t addr_len)
{
    if (kp->link != KP_LINK_CONNECTING) {
        kp->hooks.disconnect(kp->hooks.ctx);
        return;
    }
    kp->conn_addr_len = (uint8_t)(addr_len > KP_ADDR_MAX ? KP_ADDR_MAX : addr_len);
    memcpy(kp->conn_addr, addr, kp->conn_addr_len);

    uint8_t plain[32];
    uint8_t frame[LP_MAX_FRAME];
    size_t plain_len = 0;
    size_t out_len = 0;
    sl_err_t err = SL_OK;
    if (kp->pairing_mode) {
        err = sl_handshake_init(&kp->hs, SL_PATTERN_XX, SL_INITIATOR, (const uint8_t *)LP_PROLOGUE,
                                LP_PROLOGUE_LEN, &kp->identity, NULL, rng, kp);
        frame[0] = LP_FRAME_XX1;
    } else {
        lp_msg_t hello = {.type = LP_MSG_HELLO,
                          .hello = {.proto = LP_PROTO_VERSION,
                                    .role = LP_ROLE_KEYPAD,
                                    .fw_major = kp->fw_major,
                                    .fw_minor = kp->fw_minor,
                                    .battery_mv = kp->hooks.battery_mv ? kp->hooks.battery_mv(kp->hooks.ctx) : 0}};
        plain_len = lp_encode(&hello, plain, sizeof(plain));
        err = sl_handshake_init(&kp->hs, SL_PATTERN_IK, SL_INITIATOR, (const uint8_t *)LP_PROLOGUE,
                                LP_PROLOGUE_LEN, &kp->identity, kp->pairing.lock_pub, rng, kp);
        frame[0] = LP_FRAME_IK1;
    }
    if (err != SL_OK ||
        sl_handshake_write(&kp->hs, plain, plain_len, frame + 1, sizeof(frame) - 1, &out_len) != SL_OK ||
        !kp->hooks.send(kp->hooks.ctx, frame, out_len + 1)) {
        fail_link(kp);
        return;
    }
    kp->link = KP_LINK_HANDSHAKE;
    kp->link_deadline = now_ms(kp) + KP_LINK_TIMEOUT_MS;
}

void kp_on_disconnected(kp_t *kp)
{
    if (kp->link == KP_LINK_DOWN) {
        return;
    }
    reset_link(kp); /* transport is already gone; don't call disconnect */
    if (kp->pairing_mode) {
        feedback(kp, KP_FB_PAIR_FAILED, 0);
        kp->pairing_mode = false;
    } else if (kp->pending || kp->awaiting_result) {
        feedback(kp, KP_FB_NO_LINK, 0);
    }
    clear_pending(kp);
}

static void on_ik2(kp_t *kp, const uint8_t *body, size_t len)
{
    uint8_t plain[32];
    size_t plain_len = 0;
    lp_msg_t ack;
    if (sl_handshake_read(&kp->hs, body, len, plain, sizeof(plain), &plain_len) != SL_OK ||
        !lp_decode(plain, plain_len, &ack) || ack.type != LP_MSG_HELLO_ACK ||
        sl_handshake_split(&kp->hs, &kp->sess) != SL_OK) {
        fail_link(kp);
        return;
    }
    kp->link = KP_LINK_READY;
    kp->link_deadline = 0;
    if (kp->pending) {
        send_pending(kp);
    } else if (!kp->ndigits) {
        kp->linger_deadline = now_ms(kp) + KP_LINGER_MS;
    }
}

static void on_xx2(kp_t *kp, const uint8_t *body, size_t len)
{
    uint8_t plain[32];
    uint8_t frame[LP_MAX_FRAME];
    size_t plain_len = 0;
    size_t out_len = 0;
    if (sl_handshake_read(&kp->hs, body, len, plain, sizeof(plain), &plain_len) != SL_OK || plain_len != 0) {
        fail_link(kp);
        return;
    }
    memcpy(kp->lock_pub_candidate, sl_handshake_remote_static(&kp->hs), SL_KEY_LEN);
    lp_msg_t hello = {.type = LP_MSG_HELLO,
                      .hello = {.proto = LP_PROTO_VERSION,
                                .role = LP_ROLE_KEYPAD,
                                .fw_major = kp->fw_major,
                                .fw_minor = kp->fw_minor}};
    plain_len = lp_encode(&hello, plain, sizeof(plain));
    frame[0] = LP_FRAME_XX3;
    if (sl_handshake_write(&kp->hs, plain, plain_len, frame + 1, sizeof(frame) - 1, &out_len) != SL_OK ||
        sl_handshake_split(&kp->hs, &kp->sess) != SL_OK ||
        !kp->hooks.send(kp->hooks.ctx, frame, out_len + 1)) {
        fail_link(kp);
        return;
    }
    kp->link = KP_LINK_READY;
    kp->link_deadline = 0;
    kp->result_deadline = now_ms(kp) + KP_RESULT_TIMEOUT_MS;
}

static kp_feedback_t result_feedback(uint8_t result, uint8_t op)
{
    switch (result) {
    case LP_RESULT_OK: return op == LP_OP_UNLOCK ? KP_FB_UNLOCKED : KP_FB_LOCKED;
    case LP_RESULT_DENIED: return KP_FB_DENIED;
    case LP_RESULT_LOCKED_OUT: return KP_FB_LOCKED_OUT;
    default: return KP_FB_REFUSED;
    }
}

static void on_data(kp_t *kp, const uint8_t *body, size_t len)
{
    uint8_t plain[64];
    size_t plain_len = 0;
    lp_msg_t msg;
    if (sl_session_decrypt(&kp->sess, body, len, plain, sizeof(plain), &plain_len) != SL_OK) {
        fail_link(kp);
        return;
    }
    bool ok = lp_decode(plain, plain_len, &msg);
    sl_wipe(plain, sizeof(plain));
    if (!ok) {
        return;
    }

    if (msg.type == LP_MSG_PAIR_RESULT && kp->pairing_mode) {
        kp->pairing_mode = false;
        kp->result_deadline = 0;
        if (msg.pair_result.accepted) {
            kp->pairing.paired = true;
            memcpy(kp->pairing.lock_pub, kp->lock_pub_candidate, SL_KEY_LEN);
            memcpy(kp->pairing.lock_addr, kp->conn_addr, kp->conn_addr_len);
            kp->pairing.lock_addr_len = kp->conn_addr_len;
            kp->hooks.save_pairing(kp->hooks.ctx, &kp->pairing);
            feedback(kp, KP_FB_PAIRED, 0);
        } else {
            feedback(kp, KP_FB_PAIR_FAILED, 0);
        }
        close_link(kp);
    } else if (msg.type == LP_MSG_ACCESS_RESULT && kp->awaiting_result &&
               msg.access_result.seq == kp->seq) {
        kp->awaiting_result = false;
        kp->result_deadline = 0;
        feedback(kp, result_feedback(msg.access_result.result, kp->last_op), msg.access_result.lockout_s);
        kp->linger_deadline = now_ms(kp) + KP_LINGER_MS;
    }
}

void kp_on_frame(kp_t *kp, const uint8_t *frame, size_t len)
{
    if (len < 1) {
        fail_link(kp);
        return;
    }
    uint8_t type = frame[0];
    if (kp->link == KP_LINK_HANDSHAKE && !kp->pairing_mode && type == LP_FRAME_IK2) {
        on_ik2(kp, frame + 1, len - 1);
    } else if (kp->link == KP_LINK_HANDSHAKE && kp->pairing_mode && type == LP_FRAME_XX2) {
        on_xx2(kp, frame + 1, len - 1);
    } else if (kp->link == KP_LINK_READY && type == LP_FRAME_DATA) {
        on_data(kp, frame + 1, len - 1);
    } else {
        fail_link(kp);
    }
}

static bool due(uint64_t deadline, uint64_t t) { return deadline && t >= deadline; }

void kp_tick(kp_t *kp)
{
    uint64_t t = now_ms(kp);
    if (due(kp->link_deadline, t) || due(kp->result_deadline, t)) {
        fail_link(kp);
    }
    if (due(kp->entry_deadline, t)) {
        clear_digits(kp);
        if (!kp->pending && !kp->awaiting_result && !kp->pairing_mode) {
            close_link(kp);
        }
    }
    if (due(kp->linger_deadline, t)) {
        kp->linger_deadline = 0;
        if (!kp->ndigits && !kp->pending && !kp->awaiting_result) {
            close_link(kp);
        }
    }
}

uint64_t kp_next_deadline_ms(const kp_t *kp)
{
    uint64_t d = UINT64_MAX;
    const uint64_t all[] = {kp->entry_deadline, kp->link_deadline, kp->result_deadline, kp->linger_deadline};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (all[i] && all[i] < d) {
            d = all[i];
        }
    }
    return d;
}

bool kp_idle(const kp_t *kp)
{
    return kp->link == KP_LINK_DOWN && !kp->ndigits && !kp->pending && !kp->awaiting_result &&
           !kp->pairing_mode;
}
