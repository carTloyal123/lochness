#include "lock_link/ll.h"

#include <string.h>

static uint64_t now_ms(const ll_t *ll) { return ll->hooks.now_ms(ll->hooks.ctx); }

static bool rng(void *ctx, uint8_t *buf, size_t len)
{
    ll_t *ll = ctx;
    return ll->hooks.random(ll->hooks.ctx, buf, len);
}

void ll_init(ll_t *ll, const sl_keypair_t *identity, const ll_hooks_t *hooks)
{
    memset(ll, 0, sizeof(*ll));
    ll->identity = *identity;
    ll->hooks = *hooks;
}

void ll_open_pairing(ll_t *ll, lp_role_t role, uint32_t seconds)
{
    ll->pairing_open = true;
    ll->pairing_role = (uint8_t)role;
    ll->pairing_until_ms = now_ms(ll) + (uint64_t)seconds * 1000u;
}

void ll_close_pairing(ll_t *ll) { ll->pairing_open = false; }

bool ll_pairing_open(ll_t *ll)
{
    if (ll->pairing_open && now_ms(ll) >= ll->pairing_until_ms) {
        ll->pairing_open = false;
    }
    return ll->pairing_open;
}

static ll_conn_t *find_conn(ll_t *ll, uint16_t handle)
{
    for (int i = 0; i < LL_MAX_CONNS; i++) {
        if (ll->conns[i].state != LL_CONN_FREE && ll->conns[i].handle == handle) {
            return &ll->conns[i];
        }
    }
    return NULL;
}

static void forget(ll_conn_t *c) { sl_wipe(c, sizeof(*c)); }

static void drop(ll_t *ll, ll_conn_t *c)
{
    uint16_t handle = c->handle;
    forget(c);
    ll->hooks.disconnect(ll->hooks.ctx, handle);
}

static uint8_t find_peer(const ll_t *ll, const uint8_t pub[SL_KEY_LEN])
{
    uint8_t found = LL_NO_PEER;
    for (uint8_t i = 0; i < LL_MAX_PEERS; i++) {
        if (ll->peers.peers[i].in_use && sl_ct_equal(ll->peers.peers[i].static_pub, pub, SL_KEY_LEN)) {
            found = i;
        }
    }
    return found;
}

static bool send_frame(ll_t *ll, ll_conn_t *c, uint8_t type, const uint8_t *body, size_t len)
{
    uint8_t frame[LP_MAX_FRAME];
    if (len + 1 > sizeof(frame)) {
        return false;
    }
    frame[0] = type;
    memcpy(frame + 1, body, len);
    return ll->hooks.send(ll->hooks.ctx, c->handle, frame, len + 1);
}

static bool send_msg(ll_t *ll, ll_conn_t *c, const lp_msg_t *msg)
{
    uint8_t pt[32], frame[LP_MAX_FRAME];
    size_t pt_len = lp_encode(msg, pt, sizeof(pt)), ct_len;
    bool ok = pt_len &&
              sl_session_encrypt(&c->sess, pt, pt_len, frame + 1, sizeof(frame) - 1, &ct_len) == SL_OK;
    sl_wipe(pt, sizeof(pt));
    if (!ok) {
        return false;
    }
    frame[0] = LP_FRAME_DATA;
    return ll->hooks.send(ll->hooks.ctx, c->handle, frame, ct_len + 1);
}

static void establish(ll_t *ll, ll_conn_t *c, uint8_t peer)
{
    c->peer = peer;
    c->role = ll->peers.peers[peer].role;
    c->state = LL_CONN_ESTABLISHED;
    c->deadline_ms = now_ms(ll) + LL_IDLE_TIMEOUT_MS;
}

static void handle_ik1(ll_t *ll, ll_conn_t *c, const uint8_t *body, size_t len)
{
    uint8_t pt[32], out[LP_MAX_FRAME];
    size_t pt_len, out_len;
    lp_msg_t hello = {0};

    if (sl_handshake_init(&c->hs, SL_PATTERN_IK, SL_RESPONDER, (const uint8_t *)LP_PROLOGUE,
                          LP_PROLOGUE_LEN, &ll->identity, NULL, rng, ll) != SL_OK ||
        sl_handshake_read(&c->hs, body, len, pt, sizeof(pt), &pt_len) != SL_OK) {
        drop(ll, c);
        return;
    }
    uint8_t peer = find_peer(ll, sl_handshake_remote_static(&c->hs));
    if (peer == LL_NO_PEER || !lp_decode(pt, pt_len, &hello) || hello.type != LP_MSG_HELLO ||
        hello.hello.role != ll->peers.peers[peer].role) {
        drop(ll, c);
        return;
    }

    lp_msg_t ack = {.type = LP_MSG_HELLO_ACK,
                    .hello_ack = {.proto = LP_PROTO_VERSION, .bolt = (uint8_t)ll->hooks.bolt(ll->hooks.ctx)}};
    pt_len = lp_encode(&ack, pt, sizeof(pt));
    if (sl_handshake_write(&c->hs, pt, pt_len, out, sizeof(out), &out_len) != SL_OK ||
        sl_handshake_split(&c->hs, &c->sess) != SL_OK || !send_frame(ll, c, LP_FRAME_IK2, out, out_len)) {
        drop(ll, c);
        return;
    }
    establish(ll, c, peer);
    if (hello.hello.role == LP_ROLE_KEYPAD && ll->hooks.keypad_status) {
        ll->hooks.keypad_status(ll->hooks.ctx, peer, hello.hello.battery_mv, 0);
    }
}

static void handle_xx1(ll_t *ll, ll_conn_t *c, const uint8_t *body, size_t len)
{
    uint8_t pt[8], out[LP_MAX_FRAME];
    size_t pt_len, out_len;
    if (!ll_pairing_open(ll) ||
        sl_handshake_init(&c->hs, SL_PATTERN_XX, SL_RESPONDER, (const uint8_t *)LP_PROLOGUE,
                          LP_PROLOGUE_LEN, &ll->identity, NULL, rng, ll) != SL_OK ||
        sl_handshake_read(&c->hs, body, len, pt, sizeof(pt), &pt_len) != SL_OK || pt_len != 0 ||
        sl_handshake_write(&c->hs, NULL, 0, out, sizeof(out), &out_len) != SL_OK ||
        !send_frame(ll, c, LP_FRAME_XX2, out, out_len)) {
        drop(ll, c);
        return;
    }
    c->state = LL_CONN_AWAIT_XX3;
    c->deadline_ms = now_ms(ll) + LL_HANDSHAKE_TIMEOUT_MS;
}

static void handle_xx3(ll_t *ll, ll_conn_t *c, const uint8_t *body, size_t len)
{
    uint8_t pt[32], pub[SL_KEY_LEN];
    size_t pt_len;
    lp_msg_t hello;
    if (sl_handshake_read(&c->hs, body, len, pt, sizeof(pt), &pt_len) != SL_OK ||
        !lp_decode(pt, pt_len, &hello) || hello.type != LP_MSG_HELLO || !ll_pairing_open(ll) ||
        hello.hello.role != ll->pairing_role) {
        drop(ll, c);
        return;
    }
    memcpy(pub, sl_handshake_remote_static(&c->hs), SL_KEY_LEN);
    if (sl_handshake_split(&c->hs, &c->sess) != SL_OK) {
        drop(ll, c);
        return;
    }

    uint8_t slot = find_peer(ll, pub);
    for (uint8_t i = 0; slot == LL_NO_PEER && i < LL_MAX_PEERS; i++) {
        if (!ll->peers.peers[i].in_use) {
            slot = i;
        }
    }
    lp_msg_t result = {.type = LP_MSG_PAIR_RESULT, .pair_result = {.accepted = slot != LL_NO_PEER}};
    if (slot == LL_NO_PEER) {
        send_msg(ll, c, &result);
        drop(ll, c);
        return;
    }

    ll_peer_t *p = &ll->peers.peers[slot];
    p->in_use = true;
    p->role = hello.hello.role;
    memcpy(p->static_pub, pub, SL_KEY_LEN);
    ll->pairing_open = false; /* one pairing per window */
    if (ll->hooks.peers_changed) {
        ll->hooks.peers_changed(ll->hooks.ctx);
    }
    if (ll->hooks.paired) {
        ll->hooks.paired(ll->hooks.ctx, slot, (lp_role_t)p->role, c->sess.handshake_hash);
    }
    establish(ll, c, slot);
    if (!send_msg(ll, c, &result)) {
        drop(ll, c);
    }
}

static void handle_data(ll_t *ll, ll_conn_t *c, const uint8_t *body, size_t len)
{
    uint8_t pt[64];
    size_t pt_len;
    lp_msg_t msg;
    if (sl_session_decrypt(&c->sess, body, len, pt, sizeof(pt), &pt_len) != SL_OK) {
        drop(ll, c);
        return;
    }
    c->deadline_ms = now_ms(ll) + LL_IDLE_TIMEOUT_MS;
    bool ok = lp_decode(pt, pt_len, &msg);
    sl_wipe(pt, sizeof(pt));
    if (!ok) {
        return; /* authenticated but unknown/malformed: ignore for forward compatibility */
    }

    switch (msg.type) {
    case LP_MSG_ACCESS_REQ: {
        ll_access_result_t r = ll->hooks.access(
            ll->hooks.ctx, (lp_role_t)c->role, (lp_op_t)msg.access_req.op,
            msg.access_req.pin_len ? msg.access_req.pin : NULL, msg.access_req.pin_len);
        lp_msg_t reply = {.type = LP_MSG_ACCESS_RESULT,
                          .access_result = {.seq = msg.access_req.seq,
                                            .result = (uint8_t)r.result,
                                            .bolt = (uint8_t)r.bolt,
                                            .lockout_s = r.lockout_s}};
        lp_msg_wipe(&msg);
        if (!send_msg(ll, c, &reply)) {
            drop(ll, c);
        }
        break;
    }
    case LP_MSG_KEYPAD_STATUS:
        if (c->role == LP_ROLE_KEYPAD && ll->hooks.keypad_status) {
            ll->hooks.keypad_status(ll->hooks.ctx, c->peer, msg.keypad_status.battery_mv,
                                    msg.keypad_status.flags);
        }
        break;
    default:
        break; /* lock-originated types are not accepted from peers */
    }
}

void ll_on_connect(ll_t *ll, uint16_t handle)
{
    for (int i = 0; i < LL_MAX_CONNS; i++) {
        ll_conn_t *c = &ll->conns[i];
        if (c->state == LL_CONN_FREE) {
            forget(c);
            c->state = LL_CONN_AWAIT_HELLO;
            c->handle = handle;
            c->peer = LL_NO_PEER;
            c->deadline_ms = now_ms(ll) + LL_HANDSHAKE_TIMEOUT_MS;
            return;
        }
    }
    ll->hooks.disconnect(ll->hooks.ctx, handle);
}

void ll_on_disconnect(ll_t *ll, uint16_t handle)
{
    ll_conn_t *c = find_conn(ll, handle);
    if (c) {
        forget(c);
    }
}

void ll_on_frame(ll_t *ll, uint16_t handle, const uint8_t *frame, size_t len)
{
    ll_conn_t *c = find_conn(ll, handle);
    if (!c) {
        return;
    }
    if (len < 1 || len > LP_MAX_FRAME) {
        drop(ll, c);
        return;
    }
    uint8_t type = frame[0];
    const uint8_t *body = frame + 1;
    size_t body_len = len - 1;

    if (c->state == LL_CONN_AWAIT_HELLO && type == LP_FRAME_IK1) {
        handle_ik1(ll, c, body, body_len);
    } else if (c->state == LL_CONN_AWAIT_HELLO && type == LP_FRAME_XX1) {
        handle_xx1(ll, c, body, body_len);
    } else if (c->state == LL_CONN_AWAIT_XX3 && type == LP_FRAME_XX3) {
        handle_xx3(ll, c, body, body_len);
    } else if (c->state == LL_CONN_ESTABLISHED && type == LP_FRAME_DATA) {
        handle_data(ll, c, body, body_len);
    } else {
        drop(ll, c);
    }
}

void ll_tick(ll_t *ll)
{
    uint64_t t = now_ms(ll);
    for (int i = 0; i < LL_MAX_CONNS; i++) {
        if (ll->conns[i].state != LL_CONN_FREE && t >= ll->conns[i].deadline_ms) {
            drop(ll, &ll->conns[i]);
        }
    }
    ll_pairing_open(ll);
}

uint64_t ll_next_deadline_ms(const ll_t *ll)
{
    uint64_t d = ll->pairing_open ? ll->pairing_until_ms : UINT64_MAX;
    for (int i = 0; i < LL_MAX_CONNS; i++) {
        if (ll->conns[i].state != LL_CONN_FREE && ll->conns[i].deadline_ms < d) {
            d = ll->conns[i].deadline_ms;
        }
    }
    return d;
}

bool ll_peer_remove(ll_t *ll, uint8_t index)
{
    if (index >= LL_MAX_PEERS || !ll->peers.peers[index].in_use) {
        return false;
    }
    for (int i = 0; i < LL_MAX_CONNS; i++) {
        if (ll->conns[i].state != LL_CONN_FREE && ll->conns[i].peer == index) {
            drop(ll, &ll->conns[i]);
        }
    }
    sl_wipe(&ll->peers.peers[index], sizeof(ll_peer_t));
    if (ll->hooks.peers_changed) {
        ll->hooks.peers_changed(ll->hooks.ctx);
    }
    return true;
}

uint8_t ll_peer_count(const ll_t *ll)
{
    uint8_t n = 0;
    for (int i = 0; i < LL_MAX_PEERS; i++) {
        n += ll->peers.peers[i].in_use;
    }
    return n;
}
