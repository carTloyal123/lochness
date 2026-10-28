#include <stdint.h>
#include <string.h>

#include "monocypher.h"
#include "secure_link/sl_crypto.h"

enum {
    HMAC_BLOCK_LEN = 64,   /* SHA-256 block size */
    AEAD_NONCE_LEN = 12,   /* RFC 8439 IETF nonce */
    NONCE_ZERO_PREFIX = 4, /* Noise: 32 zero bits, then the 64-bit counter little-endian */
    NONCE_COUNTER_LEN = 8,
};
static const unsigned HMAC_IPAD = 0x36U; /* RFC 2104 */
static const unsigned HMAC_OPAD = 0x5CU; /* RFC 2104 */

void sl_wipe(void *p, size_t len) { crypto_wipe(p, len); }

bool sl_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    unsigned diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (unsigned)a[i] ^ (unsigned)b[i];
    }
    return diff == 0;
}

void sl_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                    uint8_t out[SL_HASH_LEN])
{
    uint8_t block_key[HMAC_BLOCK_LEN] = {0};
    if (key_len > sizeof(block_key)) {
        sl_sha256(key, key_len, block_key);
    } else {
        memcpy(block_key, key, key_len);
    }
    uint8_t pad[HMAC_BLOCK_LEN];
    sl_sha256_t ctx;
    uint8_t inner[SL_HASH_LEN];

    for (size_t i = 0; i < HMAC_BLOCK_LEN; i++) {
        pad[i] = (uint8_t)((unsigned)block_key[i] ^ HMAC_IPAD);
    }
    sl_sha256_init(&ctx);
    sl_sha256_update(&ctx, pad, sizeof(pad));
    sl_sha256_update(&ctx, data, len);
    sl_sha256_final(&ctx, inner);

    for (size_t i = 0; i < HMAC_BLOCK_LEN; i++) {
        pad[i] = (uint8_t)((unsigned)block_key[i] ^ HMAC_OPAD);
    }
    sl_sha256_init(&ctx);
    sl_sha256_update(&ctx, pad, sizeof(pad));
    sl_sha256_update(&ctx, inner, sizeof(inner));
    sl_sha256_final(&ctx, out);

    sl_wipe(block_key, sizeof(block_key));
    sl_wipe(pad, sizeof(pad));
    sl_wipe(inner, sizeof(inner));
}

void sl_hkdf(const uint8_t ck[SL_HASH_LEN], const uint8_t *ikm, size_t ikm_len,
             uint8_t out1[SL_HASH_LEN], uint8_t out2[SL_HASH_LEN], uint8_t out3[SL_HASH_LEN])
{
    uint8_t temp[SL_HASH_LEN];
    uint8_t buf[SL_HASH_LEN + 1];
    sl_hmac_sha256(ck, SL_HASH_LEN, ikm, ikm_len, temp);

    buf[0] = 0x01;
    sl_hmac_sha256(temp, sizeof(temp), buf, 1, out1);

    memcpy(buf, out1, SL_HASH_LEN);
    buf[SL_HASH_LEN] = 0x02;
    sl_hmac_sha256(temp, sizeof(temp), buf, sizeof(buf), out2);

    if (out3) {
        memcpy(buf, out2, SL_HASH_LEN);
        buf[SL_HASH_LEN] = 0x03;
        sl_hmac_sha256(temp, sizeof(temp), buf, sizeof(buf), out3);
    }
    sl_wipe(temp, sizeof(temp));
    sl_wipe(buf, sizeof(buf));
}

void sl_keypair_from_private(sl_keypair_t *kp) { crypto_x25519_public_key(kp->pub, kp->priv); }

bool sl_dh(const uint8_t priv[SL_KEY_LEN], const uint8_t pub[SL_KEY_LEN], uint8_t out[SL_KEY_LEN])
{
    static const uint8_t ZERO[SL_KEY_LEN] = {0};
    crypto_x25519(out, priv, pub);
    return !sl_ct_equal(out, ZERO, SL_KEY_LEN);
}

static void nonce_bytes(uint64_t counter, uint8_t out[AEAD_NONCE_LEN])
{
    memset(out, 0, NONCE_ZERO_PREFIX);
    for (unsigned i = 0; i < NONCE_COUNTER_LEN; i++) {
        out[NONCE_ZERO_PREFIX + i] = (uint8_t)(counter >> (8U * i));
    }
}

void sl_aead_encrypt(const uint8_t key[SL_KEY_LEN], uint64_t nonce, const uint8_t *ad, size_t ad_len,
                     const uint8_t *plaintext, size_t len, uint8_t *out)
{
    uint8_t nonce_buf[AEAD_NONCE_LEN];
    nonce_bytes(nonce, nonce_buf);
    /* A fresh context per message: Monocypher ratchets the key after each
     * write, so only the first message of a context is plain RFC 8439. */
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, nonce_buf);
    crypto_aead_write(&ctx, out, out + len, ad, ad_len, plaintext, len);
    crypto_wipe(&ctx, sizeof(ctx));
}

bool sl_aead_decrypt(const uint8_t key[SL_KEY_LEN], uint64_t nonce, const uint8_t *ad, size_t ad_len,
                     const uint8_t *ciphertext, size_t ciphertext_len, uint8_t *out)
{
    if (ciphertext_len < SL_TAG_LEN) {
        return false;
    }
    size_t len = ciphertext_len - SL_TAG_LEN;
    uint8_t nonce_buf[AEAD_NONCE_LEN];
    nonce_bytes(nonce, nonce_buf);
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, nonce_buf);
    int rc = crypto_aead_read(&ctx, out, ciphertext + len, ad, ad_len, ciphertext, len);
    crypto_wipe(&ctx, sizeof(ctx));
    return rc == 0;
}
