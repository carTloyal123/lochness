/* SHA-256 (FIPS 180-4). */
#include <stdint.h>
#include <string.h>

#include "secure_link/sl_crypto.h"

enum {
    BLOCK_LEN = 64,
    ROUNDS = 64,
    STATE_WORDS = 8,
    LEN_FIELD_LEN = 8,                     /* message length in bits, big-endian */
    PAD_TARGET = BLOCK_LEN - LEN_FIELD_LEN, /* pad until this many bytes are buffered */
};

/*
 * The rotation amounts, shifts and constants below are taken verbatim from
 * FIPS 180-4 sections 4.1.2, 4.2.2, 5.3.3 and 6.2; naming them would only
 * obscure the comparison with the spec.
 */
/* NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers) */

/* FIPS 180-4 4.2.2: first 32 bits of the fractional parts of the cube roots of the first 64 primes. */
static const uint32_t ROUND_CONSTANTS[ROUNDS] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t word, unsigned bits) { return (word >> bits) | (word << (32U - bits)); }

/* Variable names follow FIPS 180-4 6.2.2 (W, a..h, T1, T2, sigma0/1). */
/* NOLINTBEGIN(readability-identifier-length) */
static void compress(uint32_t s[STATE_WORDS], const uint8_t block[BLOCK_LEN])
{
    uint32_t w[ROUNDS];
    for (unsigned i = 0; i < 16U; i++) {
        w[i] = (uint32_t)block[i * 4U] << 24U | (uint32_t)block[(i * 4U) + 1U] << 16U |
               (uint32_t)block[(i * 4U) + 2U] << 8U | (uint32_t)block[(i * 4U) + 3U];
    }
    for (unsigned i = 16; i < ROUNDS; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0];
    uint32_t b = s[1];
    uint32_t c = s[2];
    uint32_t d = s[3];
    uint32_t e = s[4];
    uint32_t f = s[5];
    uint32_t g = s[6];
    uint32_t h = s[7];
    for (unsigned i = 0; i < ROUNDS; i++) {
        uint32_t t1 =
            h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + ROUND_CONSTANTS[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s[0] += a;
    s[1] += b;
    s[2] += c;
    s[3] += d;
    s[4] += e;
    s[5] += f;
    s[6] += g;
    s[7] += h;
    sl_wipe(w, sizeof(w));
}
/* NOLINTEND(readability-identifier-length) */

void sl_sha256_init(sl_sha256_t *ctx)
{
    /* FIPS 180-4 5.3.3. */
    static const uint32_t INITIAL_HASH[STATE_WORDS] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(ctx->state, INITIAL_HASH, sizeof(INITIAL_HASH));
    ctx->bytes = 0;
    ctx->buf_len = 0;
}

void sl_sha256_update(sl_sha256_t *ctx, const void *data, size_t len)
{
    const uint8_t *src = data;
    ctx->bytes += len;
    while (len) {
        size_t n = BLOCK_LEN - ctx->buf_len;
        if (n > len) {
            n = len;
        }
        memcpy(ctx->buf + ctx->buf_len, src, n);
        ctx->buf_len += n;
        src += n;
        len -= n;
        if (ctx->buf_len == BLOCK_LEN) {
            compress(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

void sl_sha256_final(sl_sha256_t *ctx, uint8_t out[SL_HASH_LEN])
{
    uint64_t bits = ctx->bytes * 8;
    uint8_t pad = 0x80;
    sl_sha256_update(ctx, &pad, 1);
    pad = 0;
    while (ctx->buf_len != PAD_TARGET) {
        sl_sha256_update(ctx, &pad, 1);
    }
    uint8_t len_be[LEN_FIELD_LEN];
    for (unsigned i = 0; i < LEN_FIELD_LEN; i++) {
        len_be[i] = (uint8_t)(bits >> (56U - (8U * i)));
    }
    sl_sha256_update(ctx, len_be, LEN_FIELD_LEN);
    for (unsigned i = 0; i < STATE_WORDS; i++) {
        out[i * 4U] = (uint8_t)(ctx->state[i] >> 24U);
        out[(i * 4U) + 1U] = (uint8_t)(ctx->state[i] >> 16U);
        out[(i * 4U) + 2U] = (uint8_t)(ctx->state[i] >> 8U);
        out[(i * 4U) + 3U] = (uint8_t)ctx->state[i];
    }
    sl_wipe(ctx, sizeof(*ctx));
}
/* NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers) */

void sl_sha256(const void *data, size_t len, uint8_t out[SL_HASH_LEN])
{
    sl_sha256_t ctx;
    sl_sha256_init(&ctx);
    sl_sha256_update(&ctx, data, len);
    sl_sha256_final(&ctx, out);
}
