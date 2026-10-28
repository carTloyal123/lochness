#include <string.h>

#include "monocypher.h"
#include "secure_link/sl_crypto.h"

void sl_wipe(void *p, size_t len) { crypto_wipe(p, len); }

bool sl_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++) {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}

void sl_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                    uint8_t out[SL_HASH_LEN])
{
    uint8_t k[64] = {0};
    if (key_len > sizeof(k)) {
        sl_sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t pad[64];
    sl_sha256_t ctx;
    uint8_t inner[SL_HASH_LEN];

    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x36;
    }
    sl_sha256_init(&ctx);
    sl_sha256_update(&ctx, pad, sizeof(pad));
    sl_sha256_update(&ctx, data, len);
    sl_sha256_final(&ctx, inner);

    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x5c;
    }
    sl_sha256_init(&ctx);
    sl_sha256_update(&ctx, pad, sizeof(pad));
    sl_sha256_update(&ctx, inner, sizeof(inner));
    sl_sha256_final(&ctx, out);

    sl_wipe(k, sizeof(k));
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
    static const uint8_t zero[SL_KEY_LEN] = {0};
    crypto_x25519(out, priv, pub);
    return !sl_ct_equal(out, zero, SL_KEY_LEN);
}

static void nonce_bytes(uint64_t n, uint8_t out[12])
{
    memset(out, 0, 4);
    for (int i = 0; i < 8; i++) {
        out[4 + i] = (uint8_t)(n >> (8 * i));
    }
}

void sl_aead_encrypt(const uint8_t key[SL_KEY_LEN], uint64_t nonce, const uint8_t *ad, size_t ad_len,
                     const uint8_t *pt, size_t len, uint8_t *out)
{
    uint8_t iv[12];
    nonce_bytes(nonce, iv);
    /* A fresh context per message: Monocypher ratchets the key after each
     * write, so only the first message of a context is plain RFC 8439. */
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, iv);
    crypto_aead_write(&ctx, out, out + len, ad, ad_len, pt, len);
    crypto_wipe(&ctx, sizeof(ctx));
}

bool sl_aead_decrypt(const uint8_t key[SL_KEY_LEN], uint64_t nonce, const uint8_t *ad, size_t ad_len,
                     const uint8_t *in, size_t in_len, uint8_t *out)
{
    if (in_len < SL_TAG_LEN) {
        return false;
    }
    size_t len = in_len - SL_TAG_LEN;
    uint8_t iv[12];
    nonce_bytes(nonce, iv);
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, iv);
    int rc = crypto_aead_read(&ctx, out, in + len, ad, ad_len, in, len);
    crypto_wipe(&ctx, sizeof(ctx));
    return rc == 0;
}
