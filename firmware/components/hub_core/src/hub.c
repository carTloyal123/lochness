#include "hub_core/hub.h"
#include "link_proto/lp.h"
#include "secure_link/sl_crypto.h"
#include "secure_link/sl_noise.h"

#include <stdint.h>
#include <string.h>

static uint64_t now_ms(const hub_t *hub) { return hub->hooks.now_ms(hub->hooks.ctx); }

static bool rng(void *ctx, uint8_t *buf, size_t len)
{
    hub_t *hub = ctx;
    return hub->hooks.random(hub->hooks.ctx, buf, len);
}

static void link_changed(hub_t *hub, bool is_up)
{
    if (hub->hooks.link_changed) {
        hub->hooks.link_changed(hub->hooks.ctx, is_up);
    }
}

static void finish_request(hub_t *hub, uint8_t result, uint8_t bolt)
{
    if (!hub->awaiting) {
        return;
    }
    hub->awaiting = false;
    hub->result_deadline = 0;
    if (hub->hooks.result) {
        hub->hooks.result(hub->hooks.ctx, hub->seq, result, bolt);
    }
}

/* Schedule the next connect attempt with exponential backoff. */
static void schedule_retry(hub_t *hub)
{
    hub->link = HUB_LINK_WAITING;
    hub->retry_at = now_ms(hub) + hub->backoff_ms;
    hub->backoff_ms = hub->backoff_ms * 2 > HUB_BACKOFF_MAX_MS ? HUB_BACKOFF_MAX_MS : hub->backoff_ms * 2;
}

/* Forget the session state. `was_ready` reports the link going down. */
static void reset_link(hub_t *hub)
{
    bool was_ready = hub->link == HUB_LINK_READY;
    sl_handshake_wipe(&hub->hs);
    sl_session_wipe(&hub->sess);
    hub->link = HUB_LINK_DOWN;
    hub->link_deadline = 0;
    hub->keepalive_at = 0;
    hub->retry_at = 0;
    finish_request(hub, LP_RESULT_ERROR, LP_BOLT_UNKNOWN);
    if (was_ready) {
        link_changed(hub, false);
    }
}

static void connect_session(hub_t *hub)
{
    if (!hub->pairing.paired) {
        hub->link = HUB_LINK_DOWN;
        return;
    }
    hub->link = HUB_LINK_CONNECTING;
    hub->retry_at = 0;
    hub->link_deadline = now_ms(hub) + HUB_LINK_TIMEOUT_MS;
    hub->hooks.connect(hub->hooks.ctx, hub->pairing.lock_addr, hub->pairing.lock_addr_len);
}

/* Something went wrong: drop the link and (when paired) try again later. */
static void fail_link(hub_t *hub)
{
    bool had_transport = hub->link == HUB_LINK_CONNECTING || hub->link == HUB_LINK_HANDSHAKE ||
                         hub->link == HUB_LINK_READY;
    if (hub->pairing_mode) {
        hub->pairing_mode = false;
        if (hub->hooks.paired) {
            hub->hooks.paired(hub->hooks.ctx, false);
        }
    }
    reset_link(hub);
    if (had_transport) {
        hub->hooks.disconnect(hub->hooks.ctx);
    }
    if (hub->pairing.paired) {
        schedule_retry(hub);
    }
}

void hub_init(hub_t *hub, const sl_keypair_t *identity, const hub_pairing_t *pairing, const hub_hooks_t *hooks)
{
    memset(hub, 0, sizeof(*hub));
    hub->identity = *identity;
    if (pairing) {
        hub->pairing = *pairing;
    }
    hub->hooks = *hooks;
    hub->backoff_ms = HUB_BACKOFF_MIN_MS;
}

void hub_start(hub_t *hub)
{
    if (hub->link == HUB_LINK_DOWN && !hub->pairing_mode) {
        connect_session(hub);
    }
}

void hub_start_pairing(hub_t *hub)
{
    if (hub->link != HUB_LINK_DOWN && hub->link != HUB_LINK_WAITING) {
        hub->hooks.disconnect(hub->hooks.ctx);
    }
    reset_link(hub);
    hub->pairing_mode = true;
    hub->link = HUB_LINK_CONNECTING;
    hub->link_deadline = now_ms(hub) + HUB_PAIR_TIMEOUT_MS;
    hub->hooks.connect(hub->hooks.ctx, NULL, 0);
}

void hub_forget_pairing(hub_t *hub)
{
    if (hub->link != HUB_LINK_DOWN && hub->link != HUB_LINK_WAITING) {
        hub->hooks.disconnect(hub->hooks.ctx);
    }
    reset_link(hub);
    hub->pairing_mode = false;
    sl_wipe(&hub->pairing, sizeof(hub->pairing));
    hub->hooks.save_pairing(hub->hooks.ctx, &hub->pairing);
}

static bool send_msg(hub_t *hub, const lp_msg_t *msg)
{
    uint8_t plain[32];
    uint8_t frame[LP_MAX_FRAME];
    size_t plain_len = lp_encode(msg, plain, sizeof(plain));
    size_t ct_len = 0;
    bool ok = plain_len &&
              sl_session_encrypt(&hub->sess, plain, plain_len, frame + 1, sizeof(frame) - 1, &ct_len) == SL_OK;
    sl_wipe(plain, sizeof(plain));
    if (!ok) {
        return false;
    }
    frame[0] = LP_FRAME_DATA;
    return hub->hooks.send(hub->hooks.ctx, frame, ct_len + 1);
}

/* TIME_SET when the clock is known, PING otherwise. Re-arms the keepalive. */
static bool send_keepalive(hub_t *hub)
{
    lp_msg_t msg = {.type = LP_MSG_PING};
    uint32_t unix_s = 0;
    int16_t offset = 0;
    if (hub->hooks.wall_time && hub->hooks.wall_time(hub->hooks.ctx, &unix_s, &offset) &&
        offset >= LP_UTC_OFFSET_MIN && offset <= LP_UTC_OFFSET_MAX) {
        msg = (lp_msg_t){.type = LP_MSG_TIME_SET, .time_set = {.unix_s = unix_s, .utc_offset_min = offset}};
    }
    hub->keepalive_at = now_ms(hub) + HUB_KEEPALIVE_MS;
    return send_msg(hub, &msg);
}

void hub_send_time(hub_t *hub)
{
    if (hub->link == HUB_LINK_READY && !send_keepalive(hub)) {
        fail_link(hub);
    }
}

uint8_t hub_request(hub_t *hub, lp_op_t op, const uint8_t *pin, size_t pin_len)
{
    if (hub->link != HUB_LINK_READY || hub->awaiting || pin_len > LP_PIN_FIELD_LEN || (pin_len && !pin)) {
        return 0;
    }
    hub->seq = (uint8_t)(hub->seq + 1 == 0 ? 1 : hub->seq + 1);
    lp_msg_t req = {.type = LP_MSG_ACCESS_REQ,
                    .access_req = {.seq = hub->seq, .op = (uint8_t)op, .pin_len = (uint8_t)pin_len}};
    if (pin_len) {
        memcpy(req.access_req.pin, pin, pin_len);
    }
    bool ok = send_msg(hub, &req);
    lp_msg_wipe(&req);
    if (!ok) {
        /* PIN digits that do not encode, or the transport refused the write. */
        return 0;
    }
    hub->awaiting = true;
    hub->result_deadline = now_ms(hub) + HUB_RESULT_TIMEOUT_MS;
    return hub->seq;
}

void hub_on_connected(hub_t *hub, const uint8_t *addr, size_t addr_len)
{
    if (hub->link != HUB_LINK_CONNECTING) {
        hub->hooks.disconnect(hub->hooks.ctx);
        return;
    }
    hub->conn_addr_len = (uint8_t)(addr_len > HUB_ADDR_MAX ? HUB_ADDR_MAX : addr_len);
    memcpy(hub->conn_addr, addr, hub->conn_addr_len);

    uint8_t plain[32];
    uint8_t frame[LP_MAX_FRAME];
    size_t plain_len = 0;
    size_t out_len = 0;
    sl_err_t err = SL_OK;
    if (hub->pairing_mode) {
        err = sl_handshake_init(&hub->hs, SL_PATTERN_XX, SL_INITIATOR, (const uint8_t *)LP_PROLOGUE, LP_PROLOGUE_LEN,
                                &hub->identity, NULL, rng, hub);
        frame[0] = LP_FRAME_XX1;
    } else {
        lp_msg_t hello = {.type = LP_MSG_HELLO, .hello = {.proto = LP_PROTO_VERSION, .role = LP_ROLE_HUB}};
        plain_len = lp_encode(&hello, plain, sizeof(plain));
        err = sl_handshake_init(&hub->hs, SL_PATTERN_IK, SL_INITIATOR, (const uint8_t *)LP_PROLOGUE, LP_PROLOGUE_LEN,
                                &hub->identity, hub->pairing.lock_pub, rng, hub);
        frame[0] = LP_FRAME_IK1;
    }
    if (err != SL_OK || sl_handshake_write(&hub->hs, plain, plain_len, frame + 1, sizeof(frame) - 1, &out_len) != SL_OK ||
        !hub->hooks.send(hub->hooks.ctx, frame, out_len + 1)) {
        fail_link(hub);
        return;
    }
    hub->link = HUB_LINK_HANDSHAKE;
    hub->link_deadline = now_ms(hub) + HUB_LINK_TIMEOUT_MS;
}

void hub_on_disconnected(hub_t *hub)
{
    if (hub->link == HUB_LINK_DOWN || hub->link == HUB_LINK_WAITING) {
        return;
    }
    if (hub->pairing_mode) {
        hub->pairing_mode = false;
        if (hub->hooks.paired) {
            hub->hooks.paired(hub->hooks.ctx, false);
        }
    }
    reset_link(hub); /* transport is already gone */
    if (hub->pairing.paired) {
        schedule_retry(hub);
    }
}

static void on_ik2(hub_t *hub, const uint8_t *body, size_t len)
{
    uint8_t plain[32];
    size_t plain_len = 0;
    lp_msg_t ack;
    if (sl_handshake_read(&hub->hs, body, len, plain, sizeof(plain), &plain_len) != SL_OK ||
        !lp_decode(plain, plain_len, &ack) || ack.type != LP_MSG_HELLO_ACK ||
        sl_handshake_split(&hub->hs, &hub->sess) != SL_OK) {
        fail_link(hub);
        return;
    }
    hub->link = HUB_LINK_READY;
    hub->link_deadline = 0;
    hub->backoff_ms = HUB_BACKOFF_MIN_MS;
    link_changed(hub, true);
    if (hub->link == HUB_LINK_READY && !send_keepalive(hub)) {
        fail_link(hub);
    }
}

static void on_xx2(hub_t *hub, const uint8_t *body, size_t len)
{
    uint8_t plain[32];
    uint8_t frame[LP_MAX_FRAME];
    size_t plain_len = 0;
    size_t out_len = 0;
    if (sl_handshake_read(&hub->hs, body, len, plain, sizeof(plain), &plain_len) != SL_OK || plain_len != 0) {
        fail_link(hub);
        return;
    }
    memcpy(hub->lock_pub_candidate, sl_handshake_remote_static(&hub->hs), SL_KEY_LEN);
    lp_msg_t hello = {.type = LP_MSG_HELLO, .hello = {.proto = LP_PROTO_VERSION, .role = LP_ROLE_HUB}};
    plain_len = lp_encode(&hello, plain, sizeof(plain));
    frame[0] = LP_FRAME_XX3;
    if (sl_handshake_write(&hub->hs, plain, plain_len, frame + 1, sizeof(frame) - 1, &out_len) != SL_OK ||
        sl_handshake_split(&hub->hs, &hub->sess) != SL_OK || !hub->hooks.send(hub->hooks.ctx, frame, out_len + 1)) {
        fail_link(hub);
        return;
    }
    hub->link = HUB_LINK_READY; /* waiting for PAIR_RESULT */
    hub->link_deadline = now_ms(hub) + HUB_LINK_TIMEOUT_MS;
}

static void on_pair_result(hub_t *hub, bool accepted)
{
    hub->pairing_mode = false;
    hub->link_deadline = 0;
    if (accepted) {
        hub->pairing.paired = true;
        memcpy(hub->pairing.lock_pub, hub->lock_pub_candidate, SL_KEY_LEN);
        memcpy(hub->pairing.lock_addr, hub->conn_addr, hub->conn_addr_len);
        hub->pairing.lock_addr_len = hub->conn_addr_len;
        hub->hooks.save_pairing(hub->hooks.ctx, &hub->pairing);
    }
    if (hub->hooks.paired) {
        hub->hooks.paired(hub->hooks.ctx, accepted);
    }
    if (!accepted) {
        fail_link(hub);
        return;
    }
    /* This XX session is already authenticated: keep it as the session. */
    hub->backoff_ms = HUB_BACKOFF_MIN_MS;
    link_changed(hub, true);
    if (hub->link == HUB_LINK_READY && !send_keepalive(hub)) {
        fail_link(hub);
    }
}

static void on_data(hub_t *hub, const uint8_t *body, size_t len)
{
    uint8_t plain[64];
    size_t plain_len = 0;
    lp_msg_t msg;
    if (sl_session_decrypt(&hub->sess, body, len, plain, sizeof(plain), &plain_len) != SL_OK) {
        fail_link(hub);
        return;
    }
    bool ok = lp_decode(plain, plain_len, &msg);
    sl_wipe(plain, sizeof(plain));
    if (!ok) {
        return; /* authenticated but unknown: ignore for forward compatibility */
    }
    if (hub->pairing_mode) {
        if (msg.type == LP_MSG_PAIR_RESULT) {
            on_pair_result(hub, msg.pair_result.accepted);
        }
        return;
    }
    switch (msg.type) {
    case LP_MSG_ACCESS_RESULT:
        if (hub->awaiting && msg.access_result.seq == hub->seq) {
            finish_request(hub, msg.access_result.result, msg.access_result.bolt);
        }
        break;
    case LP_MSG_LOCK_STATE:
        if (hub->hooks.lock_state) {
            hub->hooks.lock_state(hub->hooks.ctx, &msg);
        }
        break;
    default:
        break;
    }
}

void hub_on_frame(hub_t *hub, const uint8_t *frame, size_t len)
{
    if (len < 1) {
        fail_link(hub);
        return;
    }
    uint8_t type = frame[0];
    if (hub->link == HUB_LINK_HANDSHAKE && !hub->pairing_mode && type == LP_FRAME_IK2) {
        on_ik2(hub, frame + 1, len - 1);
    } else if (hub->link == HUB_LINK_HANDSHAKE && hub->pairing_mode && type == LP_FRAME_XX2) {
        on_xx2(hub, frame + 1, len - 1);
    } else if (hub->link == HUB_LINK_READY && type == LP_FRAME_DATA) {
        on_data(hub, frame + 1, len - 1);
    } else {
        fail_link(hub);
    }
}

static bool due(uint64_t deadline, uint64_t t) { return deadline && t >= deadline; }

void hub_tick(hub_t *hub)
{
    uint64_t t = now_ms(hub);
    if (due(hub->link_deadline, t)) {
        fail_link(hub);
        return;
    }
    if (due(hub->result_deadline, t)) {
        /* The lock went quiet mid-request: assume the session is dead. */
        fail_link(hub);
        return;
    }
    if (hub->link == HUB_LINK_WAITING && due(hub->retry_at, t)) {
        connect_session(hub);
        return;
    }
    if (hub->link == HUB_LINK_READY && due(hub->keepalive_at, t) && !send_keepalive(hub)) {
        fail_link(hub);
    }
}

uint64_t hub_next_deadline_ms(const hub_t *hub)
{
    uint64_t d = UINT64_MAX;
    const uint64_t all[] = {hub->link_deadline, hub->result_deadline, hub->keepalive_at, hub->retry_at};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (all[i] && all[i] < d) {
            d = all[i];
        }
    }
    return d;
}

bool hub_ready(const hub_t *hub) { return hub->link == HUB_LINK_READY && !hub->pairing_mode; }
