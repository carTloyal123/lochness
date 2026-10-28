#include <stdint.h>
#include <string.h>

#include "noise_vectors.h"
#include "secure_link/sl_crypto.h"
#include "secure_link/sl_noise.h"
#include "test.h"

static const unsigned ASCII_LOWERCASE_BIT = 0x20U;
static const unsigned HEX_DIGIT_A = 10U;

static uint8_t nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    return (uint8_t)((((unsigned)c | ASCII_LOWERCASE_BIT) - 'a') + HEX_DIGIT_A);
}

static void hex(const char *s, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(((unsigned)nibble(s[2 * i]) << 4U) | (unsigned)nibble(s[(2 * i) + 1]));
    }
}

static bool eq_hex(const uint8_t *got, const char *want)
{
    uint8_t want_bytes[64];
    size_t n = strlen(want) / 2;
    hex(want, want_bytes, n);
    return memcmp(got, want_bytes, n) == 0;
}

TEST(test_sha256_vectors)
{
    uint8_t out[32];
    sl_sha256("", 0, out);
    CHECK(eq_hex(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sl_sha256("abc", 3, out);
    CHECK(eq_hex(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sl_sha256(msg, strlen(msg), out);
    CHECK(eq_hex(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    sl_sha256_t ctx;
    sl_sha256_init(&ctx);
    /* One million 'a's, fed in 1000-byte chunks. */
    uint8_t chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    for (int i = 0; i < 1000; i++) {
        sl_sha256_update(&ctx, chunk, sizeof(chunk));
    }
    sl_sha256_final(&ctx, out);
    CHECK(eq_hex(out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(test_hmac_rfc4231)
{
    /* Key sizes and fill bytes are from RFC 4231 test cases 1 and 6. */
    enum { CASE1_KEY_LEN = 20, CASE1_KEY_BYTE = 0x0b, CASE6_KEY_LEN = 131, CASE6_KEY_BYTE = 0xaa };
    uint8_t out[32];
    uint8_t key[CASE1_KEY_LEN];
    memset(key, CASE1_KEY_BYTE, sizeof(key));
    sl_hmac_sha256(key, sizeof(key), (const uint8_t *)"Hi There", 8, out);
    CHECK(eq_hex(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    const char *jefe_msg = "what do ya want for nothing?";
    sl_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)jefe_msg, strlen(jefe_msg), out);
    CHECK(eq_hex(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    /* Case 6: key longer than the block size. */
    uint8_t long_key[CASE6_KEY_LEN];
    memset(long_key, CASE6_KEY_BYTE, sizeof(long_key));
    const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
    sl_hmac_sha256(long_key, sizeof(long_key), (const uint8_t *)msg, strlen(msg), out);
    CHECK(eq_hex(out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

TEST(test_dh_rejects_low_order)
{
    enum { PRIV_FILL = 7 }; /* any valid private key */
    sl_keypair_t kp;
    memset(kp.priv, PRIV_FILL, sizeof(kp.priv));
    sl_keypair_from_private(&kp);
    uint8_t zero[32] = {0};
    uint8_t out[32];
    CHECK(!sl_dh(kp.priv, zero, out));
}

/* Run a cacophony vector through both sides and compare every byte. */
static void run_vector(const nv_vector_t *v, sl_pattern_t pattern)
{
    sl_keypair_t init_static = {0};
    sl_keypair_t resp_static = {0};
    memcpy(init_static.priv, v->init_static, 32);
    memcpy(resp_static.priv, v->resp_static, 32);
    sl_keypair_from_private(&init_static);
    sl_keypair_from_private(&resp_static);

    sl_handshake_t ini;
    sl_handshake_t res;
    CHECK_EQ(sl_handshake_init(&ini, pattern, SL_INITIATOR, v->prologue, v->prologue_len, &init_static,
                               pattern == SL_PATTERN_IK ? resp_static.pub : NULL, NULL, NULL),
             SL_OK);
    CHECK_EQ(sl_handshake_init(&res, pattern, SL_RESPONDER, v->prologue, v->prologue_len, &resp_static,
                               NULL, NULL, NULL),
             SL_OK);
    sl_handshake_set_ephemeral(&ini, v->init_ephemeral);
    sl_handshake_set_ephemeral(&res, v->resp_ephemeral);

    size_t hs_msgs = pattern == SL_PATTERN_IK ? 2 : 3;
    sl_session_t sess_init;
    sl_session_t sess_resp;
    uint8_t buf[512];
    uint8_t plain[512];
    size_t n = 0;
    size_t plain_len = 0;
    for (size_t i = 0; i < v->msg_count; i++) {
        bool from_init = (i % 2) == 0;
        const nv_msg_t *msg = &v->msgs[i];
        if (i < hs_msgs) {
            sl_handshake_t *writer = from_init ? &ini : &res;
            sl_handshake_t *reader = from_init ? &res : &ini;
            CHECK_EQ(sl_handshake_write(writer, msg->payload, msg->payload_len, buf, sizeof(buf), &n),
                     SL_OK);
            CHECK_EQ(n, msg->ct_len);
            CHECK(memcmp(buf, msg->ct, msg->ct_len) == 0);
            CHECK_EQ(sl_handshake_read(reader, buf, n, plain, sizeof(plain), &plain_len), SL_OK);
            CHECK_EQ(plain_len, msg->payload_len);
            CHECK(memcmp(plain, msg->payload, plain_len) == 0);
            if (i == hs_msgs - 1) {
                CHECK(sl_handshake_done(&ini) && sl_handshake_done(&res));
                CHECK(memcmp(sl_handshake_remote_static(&res), init_static.pub, 32) == 0);
                CHECK(memcmp(sl_handshake_remote_static(&ini), resp_static.pub, 32) == 0);
                CHECK_EQ(sl_handshake_split(&ini, &sess_init), SL_OK);
                CHECK_EQ(sl_handshake_split(&res, &sess_resp), SL_OK);
                CHECK(memcmp(sess_init.handshake_hash, v->handshake_hash, 32) == 0);
                CHECK(memcmp(sess_resp.handshake_hash, v->handshake_hash, 32) == 0);
            }
        } else {
            sl_session_t *writer = from_init ? &sess_init : &sess_resp;
            sl_session_t *reader = from_init ? &sess_resp : &sess_init;
            CHECK_EQ(sl_session_encrypt(writer, msg->payload, msg->payload_len, buf, sizeof(buf), &n),
                     SL_OK);
            CHECK_EQ(n, msg->ct_len);
            CHECK(memcmp(buf, msg->ct, msg->ct_len) == 0);
            CHECK_EQ(sl_session_decrypt(reader, buf, n, plain, sizeof(plain), &plain_len), SL_OK);
            CHECK(plain_len == msg->payload_len && memcmp(plain, msg->payload, plain_len) == 0);
        }
    }
}

TEST(test_noise_ik_vector) { run_vector(&nv_ik, SL_PATTERN_IK); }
TEST(test_noise_xx_vector) { run_vector(&nv_xx, SL_PATTERN_XX); }

/* Deterministic, not random: odd multiples of a counter. */
static const unsigned RNG_STEP = 101U;

static bool counter_rng(void *ctx, uint8_t *buf, size_t len)
{
    uint8_t *counter = ctx;
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(++*counter * RNG_STEP);
    }
    return true;
}

static void keypair(sl_keypair_t *kp, uint8_t seed)
{
    memset(kp->priv, seed, sizeof(kp->priv));
    sl_keypair_from_private(kp);
}

TEST(test_ik_wrong_responder_key_fails)
{
    sl_keypair_t alice;
    sl_keypair_t bob;
    sl_keypair_t wrong;
    keypair(&alice, 1);
    keypair(&bob, 2);
    keypair(&wrong, 3);
    uint8_t rng = 0;
    sl_handshake_t ini;
    sl_handshake_t res;
    sl_handshake_init(&ini, SL_PATTERN_IK, SL_INITIATOR, NULL, 0, &alice, wrong.pub, counter_rng, &rng);
    sl_handshake_init(&res, SL_PATTERN_IK, SL_RESPONDER, NULL, 0, &bob, NULL, counter_rng, &rng);
    uint8_t buf[256];
    uint8_t plain[64];
    size_t n = 0;
    size_t plain_len = 0;
    CHECK_EQ(sl_handshake_write(&ini, NULL, 0, buf, sizeof(buf), &n), SL_OK);
    CHECK_EQ(sl_handshake_read(&res, buf, n, plain, sizeof(plain), &plain_len), SL_ERR_DECRYPT);
    CHECK_EQ(sl_handshake_read(&res, buf, n, plain, sizeof(plain), &plain_len), SL_ERR_STATE); /* stays failed */
}

TEST(test_prologue_mismatch_fails)
{
    sl_keypair_t alice;
    sl_keypair_t bob;
    keypair(&alice, 1);
    keypair(&bob, 2);
    uint8_t rng = 0;
    sl_handshake_t ini;
    sl_handshake_t res;
    sl_handshake_init(&ini, SL_PATTERN_IK, SL_INITIATOR, (const uint8_t *)"v1", 2, &alice, bob.pub, counter_rng, &rng);
    sl_handshake_init(&res, SL_PATTERN_IK, SL_RESPONDER, (const uint8_t *)"v2", 2, &bob, NULL, counter_rng, &rng);
    uint8_t buf[256];
    uint8_t plain[64];
    size_t n = 0;
    size_t plain_len = 0;
    sl_handshake_write(&ini, NULL, 0, buf, sizeof(buf), &n);
    CHECK_EQ(sl_handshake_read(&res, buf, n, plain, sizeof(plain), &plain_len), SL_ERR_DECRYPT);
}

TEST(test_session_rejects_replay_and_tamper)
{
    sl_keypair_t alice;
    sl_keypair_t bob;
    keypair(&alice, 1);
    keypair(&bob, 2);
    uint8_t rng = 0;
    sl_handshake_t ini;
    sl_handshake_t res;
    sl_handshake_init(&ini, SL_PATTERN_IK, SL_INITIATOR, NULL, 0, &alice, bob.pub, counter_rng, &rng);
    sl_handshake_init(&res, SL_PATTERN_IK, SL_RESPONDER, NULL, 0, &bob, NULL, counter_rng, &rng);
    uint8_t buf[256];
    uint8_t plain[64];
    size_t n = 0;
    size_t plain_len = 0;
    sl_handshake_write(&ini, NULL, 0, buf, sizeof(buf), &n);
    sl_handshake_read(&res, buf, n, plain, sizeof(plain), &plain_len);
    sl_handshake_write(&res, NULL, 0, buf, sizeof(buf), &n);
    sl_handshake_read(&ini, buf, n, plain, sizeof(plain), &plain_len);
    sl_session_t sess_init;
    sl_session_t sess_resp;
    CHECK_EQ(sl_handshake_split(&ini, &sess_init), SL_OK);
    CHECK_EQ(sl_handshake_split(&res, &sess_resp), SL_OK);

    uint8_t first[64];
    size_t first_len = 0;
    sl_session_encrypt(&sess_init, (const uint8_t *)"unlock", strlen("unlock"), first, sizeof(first),
                       &first_len);
    CHECK_EQ(sl_session_decrypt(&sess_resp, first, first_len, plain, sizeof(plain), &plain_len), SL_OK);
    CHECK_EQ(sl_session_decrypt(&sess_resp, first, first_len, plain, sizeof(plain), &plain_len),
             SL_ERR_DECRYPT); /* replay */

    sl_session_encrypt(&sess_init, (const uint8_t *)"lock", 4, buf, sizeof(buf), &n);
    buf[0] = (uint8_t)(buf[0] ^ 1U);
    CHECK_EQ(sl_session_decrypt(&sess_resp, buf, n, plain, sizeof(plain), &plain_len), SL_ERR_DECRYPT);
}

TEST(test_handshake_out_of_turn)
{
    sl_keypair_t alice;
    sl_keypair_t bob;
    keypair(&alice, 1);
    keypair(&bob, 2);
    sl_handshake_t res;
    sl_handshake_init(&res, SL_PATTERN_XX, SL_RESPONDER, NULL, 0, &bob, NULL, NULL, NULL);
    uint8_t buf[256];
    size_t n = 0;
    CHECK_EQ(sl_handshake_write(&res, NULL, 0, buf, sizeof(buf), &n), SL_ERR_STATE);
    sl_handshake_t ini;
    CHECK_EQ(sl_handshake_init(&ini, SL_PATTERN_IK, SL_INITIATOR, NULL, 0, &alice, NULL, NULL, NULL),
             SL_ERR_ARG); /* IK initiator needs rs */
}

TEST(test_handshake_truncated_message)
{
    sl_keypair_t alice;
    sl_keypair_t bob;
    keypair(&alice, 1);
    keypair(&bob, 2);
    uint8_t rng = 0;
    sl_handshake_t ini;
    sl_handshake_t res;
    sl_handshake_init(&ini, SL_PATTERN_IK, SL_INITIATOR, NULL, 0, &alice, bob.pub, counter_rng, &rng);
    sl_handshake_init(&res, SL_PATTERN_IK, SL_RESPONDER, NULL, 0, &bob, NULL, counter_rng, &rng);
    uint8_t buf[256];
    uint8_t plain[64];
    size_t n = 0;
    size_t plain_len = 0;
    sl_handshake_write(&ini, NULL, 0, buf, sizeof(buf), &n);
    enum { TRUNCATED_LEN = 40 }; /* shorter than IK1 (32-byte ephemeral + 48-byte encrypted static + tag) */
    CHECK_EQ(sl_handshake_read(&res, buf, TRUNCATED_LEN, plain, sizeof(plain), &plain_len), SL_ERR_BUF);
}
