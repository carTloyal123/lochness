/*
 * Crypto primitives for secure_link: SHA-256, HMAC-SHA256, the Noise HKDF,
 * X25519, and ChaCha20-Poly1305 (IETF). Portable C; X25519 and the AEAD come
 * from Monocypher.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL_HASH_LEN 32
#define SL_KEY_LEN 32
#define SL_TAG_LEN 16

typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t buf[64];
    size_t buf_len;
} sl_sha256_t;

void sl_sha256_init(sl_sha256_t *ctx);
void sl_sha256_update(sl_sha256_t *ctx, const void *data, size_t len);
void sl_sha256_final(sl_sha256_t *ctx, uint8_t out[SL_HASH_LEN]);
void sl_sha256(const void *data, size_t len, uint8_t out[SL_HASH_LEN]);

void sl_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                    uint8_t out[SL_HASH_LEN]);

/* Noise HKDF(chaining_key, ikm) producing two or three outputs (out3 may be NULL). */
void sl_hkdf(const uint8_t ck[SL_HASH_LEN], const uint8_t *ikm, size_t ikm_len,
             uint8_t out1[SL_HASH_LEN], uint8_t out2[SL_HASH_LEN], uint8_t out3[SL_HASH_LEN]);

typedef struct {
    uint8_t priv[SL_KEY_LEN];
    uint8_t pub[SL_KEY_LEN];
} sl_keypair_t;

/* Derive the public key for keypair->priv. */
void sl_keypair_from_private(sl_keypair_t *kp);
/* X25519. Returns false if the result is all zeros (low-order peer key). */
bool sl_dh(const uint8_t priv[SL_KEY_LEN], const uint8_t pub[SL_KEY_LEN], uint8_t out[SL_KEY_LEN]);

/* ChaCha20-Poly1305: out = ciphertext || tag (len + SL_TAG_LEN bytes). */
void sl_aead_encrypt(const uint8_t key[SL_KEY_LEN], uint64_t nonce, const uint8_t *ad, size_t ad_len,
                     const uint8_t *pt, size_t len, uint8_t *out);
/* in = ciphertext || tag. Returns false on authentication failure. */
bool sl_aead_decrypt(const uint8_t key[SL_KEY_LEN], uint64_t nonce, const uint8_t *ad, size_t ad_len,
                     const uint8_t *in, size_t in_len, uint8_t *out);

bool sl_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);
void sl_wipe(void *p, size_t len);

#ifdef __cplusplus
}
#endif
