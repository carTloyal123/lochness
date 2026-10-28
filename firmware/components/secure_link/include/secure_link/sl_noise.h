/*
 * Noise Protocol Framework (revision 34) handshakes for lochness links:
 *
 *   Noise_IK_25519_ChaChaPoly_SHA256  normal sessions: the initiator already
 *       knows the responder's static key (from pairing). One round trip,
 *       mutual authentication, initiator identity encrypted on the air.
 *   Noise_XX_25519_ChaChaPoly_SHA256  pairing: static keys are exchanged.
 *
 * Payloads in IK message 1 are replayable by an attacker (no responder
 * ephemeral contribution yet): never put actions there.
 *
 * Any error leaves the handshake unusable; the caller must abort it.
 */
#pragma once

#include "secure_link/sl_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SL_OK = 0,
    SL_ERR_ARG,
    SL_ERR_STATE,   /* call out of order, or handshake already failed */
    SL_ERR_BUF,     /* output buffer too small or input malformed */
    SL_ERR_DECRYPT, /* authentication failed */
    SL_ERR_DH,      /* low-order public key */
    SL_ERR_NONCE,   /* nonce space exhausted */
    SL_ERR_RNG,
} sl_err_t;

typedef enum { SL_PATTERN_IK, SL_PATTERN_XX } sl_pattern_t;
typedef enum { SL_INITIATOR, SL_RESPONDER } sl_role_t;

/* Fill buf with cryptographically secure random bytes; return false on failure. */
typedef bool (*sl_rng_fn)(void *ctx, uint8_t *buf, size_t len);

typedef struct {
    uint8_t k[SL_KEY_LEN];
    bool has_key;
    uint64_t n;
} sl_cipher_t;

typedef struct {
    sl_pattern_t pattern;
    sl_role_t role;
    uint8_t ck[SL_HASH_LEN];
    uint8_t h[SL_HASH_LEN];
    sl_cipher_t cs;
    sl_keypair_t s, e;
    uint8_t rs[SL_KEY_LEN], re[SL_KEY_LEN];
    bool has_s, has_e, has_rs, has_re;
    uint8_t msg_index;
    bool failed;
    sl_rng_fn rng;
    void *rng_ctx;
} sl_handshake_t;

typedef struct {
    sl_cipher_t tx, rx;
    uint8_t handshake_hash[SL_HASH_LEN]; /* channel binding; basis for pairing codes */
} sl_session_t;

/*
 * s: local static keypair (required for IK and XX).
 * rs: responder's static public key; required for an IK initiator, else NULL.
 */
sl_err_t sl_handshake_init(sl_handshake_t *hs, sl_pattern_t pattern, sl_role_t role,
                           const uint8_t *prologue, size_t prologue_len, const sl_keypair_t *s,
                           const uint8_t *rs, sl_rng_fn rng, void *rng_ctx);
/* Use a fixed ephemeral private key (test vectors only). */
void sl_handshake_set_ephemeral(sl_handshake_t *hs, const uint8_t priv[SL_KEY_LEN]);

sl_err_t sl_handshake_write(sl_handshake_t *hs, const uint8_t *payload, size_t payload_len,
                            uint8_t *out, size_t out_cap, size_t *out_len);
sl_err_t sl_handshake_read(sl_handshake_t *hs, const uint8_t *msg, size_t msg_len, uint8_t *payload,
                           size_t payload_cap, size_t *payload_len);

bool sl_handshake_is_my_turn(const sl_handshake_t *hs);
bool sl_handshake_done(const sl_handshake_t *hs);
/* Remote static key, once received (or given for IK). NULL if unknown. */
const uint8_t *sl_handshake_remote_static(const sl_handshake_t *hs);
/* Size of the next message for a given payload length. */
size_t sl_handshake_msg_len(const sl_handshake_t *hs, size_t payload_len);

/* Derive transport keys and wipe the handshake. Only valid once done. */
sl_err_t sl_handshake_split(sl_handshake_t *hs, sl_session_t *out);
void sl_handshake_wipe(sl_handshake_t *hs);

/* Transport messages: out = ciphertext || tag (len + SL_TAG_LEN). */
sl_err_t sl_session_encrypt(sl_session_t *s, const uint8_t *pt, size_t len, uint8_t *out,
                            size_t out_cap, size_t *out_len);
sl_err_t sl_session_decrypt(sl_session_t *s, const uint8_t *in, size_t in_len, uint8_t *out,
                            size_t out_cap, size_t *out_len);
void sl_session_wipe(sl_session_t *s);

#ifdef __cplusplus
}
#endif
