#include "secure_link/sl_noise.h"

#include <string.h>

typedef enum { T_END = 0, T_E, T_S, T_EE, T_ES, T_SE, T_SS } token_t;

#define MAX_MSGS 3
#define MAX_TOKENS 5

static const uint8_t IK_TOKENS[MAX_MSGS][MAX_TOKENS] = {
    {T_E, T_ES, T_S, T_SS, T_END},
    {T_E, T_EE, T_SE, T_END},
    {T_END},
};
static const uint8_t XX_TOKENS[MAX_MSGS][MAX_TOKENS] = {
    {T_E, T_END},
    {T_E, T_EE, T_S, T_ES, T_END},
    {T_S, T_SE, T_END},
};

static uint8_t msg_count(const sl_handshake_t *hs) { return hs->pattern == SL_PATTERN_IK ? 2 : 3; }

static const uint8_t *tokens(const sl_handshake_t *hs)
{
    return (hs->pattern == SL_PATTERN_IK ? IK_TOKENS : XX_TOKENS)[hs->msg_index];
}

/* ---- SymmetricState ------------------------------------------------------- */

static void mix_hash(sl_handshake_t *hs, const uint8_t *data, size_t len)
{
    sl_sha256_t ctx;
    sl_sha256_init(&ctx);
    sl_sha256_update(&ctx, hs->h, SL_HASH_LEN);
    sl_sha256_update(&ctx, data, len);
    sl_sha256_final(&ctx, hs->h);
}

static void mix_key(sl_handshake_t *hs, const uint8_t *ikm, size_t len)
{
    uint8_t k[SL_HASH_LEN];
    sl_hkdf(hs->ck, ikm, len, hs->ck, k, NULL);
    memcpy(hs->cs.k, k, SL_KEY_LEN);
    hs->cs.has_key = true;
    hs->cs.n = 0;
    sl_wipe(k, sizeof(k));
}

static sl_err_t encrypt_and_hash(sl_handshake_t *hs, const uint8_t *pt, size_t len, uint8_t *out)
{
    size_t out_len = len;
    if (hs->cs.has_key) {
        if (hs->cs.n == UINT64_MAX) {
            return SL_ERR_NONCE;
        }
        sl_aead_encrypt(hs->cs.k, hs->cs.n++, hs->h, SL_HASH_LEN, pt, len, out);
        out_len += SL_TAG_LEN;
    } else {
        memmove(out, pt, len);
    }
    mix_hash(hs, out, out_len);
    return SL_OK;
}

static sl_err_t decrypt_and_hash(sl_handshake_t *hs, const uint8_t *in, size_t in_len, uint8_t *out)
{
    uint8_t h_before[SL_HASH_LEN];
    memcpy(h_before, hs->h, SL_HASH_LEN);
    mix_hash(hs, in, in_len); /* hash the ciphertext; decrypt with the previous h as AD */
    if (!hs->cs.has_key) {
        memmove(out, in, in_len);
        return SL_OK;
    }
    if (hs->cs.n == UINT64_MAX) {
        return SL_ERR_NONCE;
    }
    if (!sl_aead_decrypt(hs->cs.k, hs->cs.n, h_before, SL_HASH_LEN, in, in_len, out)) {
        return SL_ERR_DECRYPT;
    }
    hs->cs.n++;
    return SL_OK;
}

/* ---- HandshakeState ------------------------------------------------------- */

sl_err_t sl_handshake_init(sl_handshake_t *hs, sl_pattern_t pattern, sl_role_t role,
                           const uint8_t *prologue, size_t prologue_len, const sl_keypair_t *s,
                           const uint8_t *rs, sl_rng_fn rng, void *rng_ctx)
{
    memset(hs, 0, sizeof(*hs));
    if ((pattern != SL_PATTERN_IK && pattern != SL_PATTERN_XX) || !s ||
        (pattern == SL_PATTERN_IK && role == SL_INITIATOR && !rs)) {
        return SL_ERR_ARG;
    }
    hs->pattern = pattern;
    hs->role = role;
    hs->rng = rng;
    hs->rng_ctx = rng_ctx;
    hs->s = *s;
    hs->has_s = true;

    /* Both protocol names are exactly SL_HASH_LEN bytes, so h = name. */
    const char *name = pattern == SL_PATTERN_IK ? "Noise_IK_25519_ChaChaPoly_SHA256"
                                                : "Noise_XX_25519_ChaChaPoly_SHA256";
    memcpy(hs->h, name, SL_HASH_LEN);
    memcpy(hs->ck, hs->h, SL_HASH_LEN);
    mix_hash(hs, prologue, prologue_len);

    if (pattern == SL_PATTERN_IK) {
        /* Pre-message "<- s": the responder's static key. */
        if (role == SL_INITIATOR) {
            memcpy(hs->rs, rs, SL_KEY_LEN);
            hs->has_rs = true;
            mix_hash(hs, hs->rs, SL_KEY_LEN);
        } else {
            mix_hash(hs, hs->s.pub, SL_KEY_LEN);
        }
    }
    return SL_OK;
}

void sl_handshake_set_ephemeral(sl_handshake_t *hs, const uint8_t priv[SL_KEY_LEN])
{
    memcpy(hs->e.priv, priv, SL_KEY_LEN);
    sl_keypair_from_private(&hs->e);
    hs->has_e = true;
}

bool sl_handshake_is_my_turn(const sl_handshake_t *hs)
{
    bool initiator_turn = (hs->msg_index % 2) == 0;
    return !hs->failed && hs->msg_index < msg_count(hs) &&
           initiator_turn == (hs->role == SL_INITIATOR);
}

bool sl_handshake_done(const sl_handshake_t *hs) { return !hs->failed && hs->msg_index == msg_count(hs); }

const uint8_t *sl_handshake_remote_static(const sl_handshake_t *hs) { return hs->has_rs ? hs->rs : NULL; }

size_t sl_handshake_msg_len(const sl_handshake_t *hs, size_t payload_len)
{
    if (hs->msg_index >= msg_count(hs)) {
        return 0;
    }
    size_t len = 0;
    bool has_key = hs->cs.has_key;
    for (const uint8_t *t = tokens(hs); *t != T_END; t++) {
        if (*t == T_E) {
            len += SL_KEY_LEN;
        } else if (*t == T_S) {
            len += SL_KEY_LEN + (has_key ? SL_TAG_LEN : 0);
        } else {
            has_key = true; /* every DH token mixes a key */
        }
    }
    return len + payload_len + (has_key ? SL_TAG_LEN : 0);
}

/* Perform the DH for a token from this side's point of view. */
static sl_err_t dh_token(sl_handshake_t *hs, token_t t)
{
    const sl_keypair_t *local;
    const uint8_t *remote;
    bool init = hs->role == SL_INITIATOR;
    bool ok_keys;
    switch (t) {
    case T_EE:
        local = &hs->e, remote = hs->re, ok_keys = hs->has_e && hs->has_re;
        break;
    case T_ES:
        local = init ? &hs->e : &hs->s;
        remote = init ? hs->rs : hs->re;
        ok_keys = init ? hs->has_e && hs->has_rs : hs->has_s && hs->has_re;
        break;
    case T_SE:
        local = init ? &hs->s : &hs->e;
        remote = init ? hs->re : hs->rs;
        ok_keys = init ? hs->has_s && hs->has_re : hs->has_e && hs->has_rs;
        break;
    case T_SS:
        local = &hs->s, remote = hs->rs, ok_keys = hs->has_s && hs->has_rs;
        break;
    default:
        return SL_ERR_STATE;
    }
    if (!ok_keys) {
        return SL_ERR_STATE;
    }
    uint8_t shared[SL_KEY_LEN];
    bool ok = sl_dh(local->priv, remote, shared);
    if (ok) {
        mix_key(hs, shared, sizeof(shared));
    }
    sl_wipe(shared, sizeof(shared));
    return ok ? SL_OK : SL_ERR_DH;
}

static sl_err_t fail(sl_handshake_t *hs, sl_err_t err)
{
    hs->failed = true;
    return err;
}

sl_err_t sl_handshake_write(sl_handshake_t *hs, const uint8_t *payload, size_t payload_len,
                            uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!sl_handshake_is_my_turn(hs)) {
        return SL_ERR_STATE;
    }
    if (sl_handshake_msg_len(hs, payload_len) > out_cap) {
        return SL_ERR_BUF;
    }
    size_t pos = 0;
    sl_err_t err;
    for (const uint8_t *t = tokens(hs); *t != T_END; t++) {
        switch (*t) {
        case T_E:
            if (!hs->has_e) {
                if (!hs->rng || !hs->rng(hs->rng_ctx, hs->e.priv, SL_KEY_LEN)) {
                    return fail(hs, SL_ERR_RNG);
                }
                sl_keypair_from_private(&hs->e);
                hs->has_e = true;
            }
            memcpy(out + pos, hs->e.pub, SL_KEY_LEN);
            mix_hash(hs, hs->e.pub, SL_KEY_LEN);
            pos += SL_KEY_LEN;
            break;
        case T_S:
            err = encrypt_and_hash(hs, hs->s.pub, SL_KEY_LEN, out + pos);
            if (err != SL_OK) {
                return fail(hs, err);
            }
            pos += SL_KEY_LEN + (hs->cs.has_key ? SL_TAG_LEN : 0);
            break;
        default:
            err = dh_token(hs, (token_t)*t);
            if (err != SL_OK) {
                return fail(hs, err);
            }
        }
    }
    err = encrypt_and_hash(hs, payload, payload_len, out + pos);
    if (err != SL_OK) {
        return fail(hs, err);
    }
    pos += payload_len + (hs->cs.has_key ? SL_TAG_LEN : 0);
    *out_len = pos;
    hs->msg_index++;
    return SL_OK;
}

sl_err_t sl_handshake_read(sl_handshake_t *hs, const uint8_t *msg, size_t msg_len, uint8_t *payload,
                           size_t payload_cap, size_t *payload_len)
{
    if (hs->failed || hs->msg_index >= msg_count(hs) || sl_handshake_is_my_turn(hs)) {
        return SL_ERR_STATE;
    }
    size_t overhead = sl_handshake_msg_len(hs, 0);
    if (msg_len < overhead || msg_len - overhead > payload_cap) {
        return fail(hs, SL_ERR_BUF);
    }
    size_t pos = 0;
    sl_err_t err;
    for (const uint8_t *t = tokens(hs); *t != T_END; t++) {
        switch (*t) {
        case T_E:
            memcpy(hs->re, msg + pos, SL_KEY_LEN);
            hs->has_re = true;
            mix_hash(hs, hs->re, SL_KEY_LEN);
            pos += SL_KEY_LEN;
            break;
        case T_S: {
            size_t n = SL_KEY_LEN + (hs->cs.has_key ? SL_TAG_LEN : 0);
            err = decrypt_and_hash(hs, msg + pos, n, hs->rs);
            if (err != SL_OK) {
                return fail(hs, err);
            }
            hs->has_rs = true;
            pos += n;
            break;
        }
        default:
            err = dh_token(hs, (token_t)*t);
            if (err != SL_OK) {
                return fail(hs, err);
            }
        }
    }
    err = decrypt_and_hash(hs, msg + pos, msg_len - pos, payload);
    if (err != SL_OK) {
        return fail(hs, err);
    }
    *payload_len = msg_len - pos - (hs->cs.has_key ? SL_TAG_LEN : 0);
    hs->msg_index++;
    return SL_OK;
}

sl_err_t sl_handshake_split(sl_handshake_t *hs, sl_session_t *out)
{
    if (!sl_handshake_done(hs)) {
        return SL_ERR_STATE;
    }
    uint8_t k1[SL_HASH_LEN], k2[SL_HASH_LEN];
    sl_hkdf(hs->ck, NULL, 0, k1, k2, NULL);
    memset(out, 0, sizeof(*out));
    bool init = hs->role == SL_INITIATOR;
    memcpy(out->tx.k, init ? k1 : k2, SL_KEY_LEN);
    memcpy(out->rx.k, init ? k2 : k1, SL_KEY_LEN);
    out->tx.has_key = out->rx.has_key = true;
    memcpy(out->handshake_hash, hs->h, SL_HASH_LEN);
    sl_wipe(k1, sizeof(k1));
    sl_wipe(k2, sizeof(k2));
    sl_handshake_wipe(hs);
    return SL_OK;
}

void sl_handshake_wipe(sl_handshake_t *hs) { sl_wipe(hs, sizeof(*hs)); }

sl_err_t sl_session_encrypt(sl_session_t *s, const uint8_t *pt, size_t len, uint8_t *out,
                            size_t out_cap, size_t *out_len)
{
    if (!s->tx.has_key) {
        return SL_ERR_STATE;
    }
    if (len + SL_TAG_LEN > out_cap) {
        return SL_ERR_BUF;
    }
    if (s->tx.n == UINT64_MAX) {
        return SL_ERR_NONCE;
    }
    sl_aead_encrypt(s->tx.k, s->tx.n++, NULL, 0, pt, len, out);
    *out_len = len + SL_TAG_LEN;
    return SL_OK;
}

sl_err_t sl_session_decrypt(sl_session_t *s, const uint8_t *in, size_t in_len, uint8_t *out,
                            size_t out_cap, size_t *out_len)
{
    if (!s->rx.has_key) {
        return SL_ERR_STATE;
    }
    if (in_len < SL_TAG_LEN || in_len - SL_TAG_LEN > out_cap) {
        return SL_ERR_BUF;
    }
    if (s->rx.n == UINT64_MAX) {
        return SL_ERR_NONCE;
    }
    /* Implicit nonces: the link is reliable and ordered, so any replayed,
     * dropped or reordered message fails authentication. */
    if (!sl_aead_decrypt(s->rx.k, s->rx.n, NULL, 0, in, in_len, out)) {
        return SL_ERR_DECRYPT;
    }
    s->rx.n++;
    *out_len = in_len - SL_TAG_LEN;
    return SL_OK;
}

void sl_session_wipe(sl_session_t *s) { sl_wipe(s, sizeof(*s)); }
