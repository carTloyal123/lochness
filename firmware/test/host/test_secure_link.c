#include <string.h>

#include "noise_vectors.h"
#include "secure_link/sl_noise.h"
#include "test.h"

static void hex(const char *s, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static bool eq_hex(const uint8_t *got, const char *want)
{
    uint8_t w[64];
    size_t n = strlen(want) / 2;
    hex(want, w, n);
    return memcmp(got, w, n) == 0;
}

TEST(test_sha256_vectors)
{
    uint8_t out[32];
    sl_sha256("", 0, out);
    CHECK(eq_hex(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sl_sha256("abc", 3, out);
    CHECK(eq_hex(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sl_sha256(m, strlen(m), out);
    CHECK(eq_hex(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    sl_sha256_t ctx;
    sl_sha256_init(&ctx);
    uint8_t a[1000];
    memset(a, 'a', sizeof(a));
    for (int i = 0; i < 1000; i++) {
        sl_sha256_update(&ctx, a, sizeof(a));
    }
    sl_sha256_final(&ctx, out);
    CHECK(eq_hex(out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(test_hmac_rfc4231)
{
    uint8_t out[32];
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    sl_hmac_sha256(key, sizeof(key), (const uint8_t *)"Hi There", 8, out);
    CHECK(eq_hex(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    sl_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, out);
    CHECK(eq_hex(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    /* Case 6: key longer than the block size. */
    uint8_t long_key[131];
    memset(long_key, 0xaa, sizeof(long_key));
    const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
    sl_hmac_sha256(long_key, sizeof(long_key), (const uint8_t *)msg, strlen(msg), out);
    CHECK(eq_hex(out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

TEST(test_dh_rejects_low_order)
{
    sl_keypair_t kp;
    memset(kp.priv, 7, sizeof(kp.priv));
    sl_keypair_from_private(&kp);
    uint8_t zero[32] = {0}, out[32];
    CHECK(!sl_dh(kp.priv, zero, out));
}

/* Run a cacophony vector through both sides and compare every byte. */
static void run_vector(const nv_vector_t *v, sl_pattern_t pattern)
{
    sl_keypair_t is = {0}, rs = {0};
    memcpy(is.priv, v->init_static, 32);
    memcpy(rs.priv, v->resp_static, 32);
    sl_keypair_from_private(&is);
    sl_keypair_from_private(&rs);

    sl_handshake_t ini, res;
    CHECK_EQ(sl_handshake_init(&ini, pattern, SL_INITIATOR, v->prologue, v->prologue_len, &is,
                               pattern == SL_PATTERN_IK ? rs.pub : NULL, NULL, NULL),
             SL_OK);
    CHECK_EQ(sl_handshake_init(&res, pattern, SL_RESPONDER, v->prologue, v->prologue_len, &rs, NULL,
                               NULL, NULL),
             SL_OK);
    sl_handshake_set_ephemeral(&ini, v->init_ephemeral);
    sl_handshake_set_ephemeral(&res, v->resp_ephemeral);

    size_t hs_msgs = pattern == SL_PATTERN_IK ? 2 : 3;
    sl_session_t si, sr;
    uint8_t buf[512], pt[512];
    size_t n, pn;
    for (size_t i = 0; i < v->msg_count; i++) {
        bool from_init = (i % 2) == 0;
        const nv_msg_t *m = &v->msgs[i];
        if (i < hs_msgs) {
            sl_handshake_t *w = from_init ? &ini : &res, *r = from_init ? &res : &ini;
            CHECK_EQ(sl_handshake_write(w, m->payload, m->payload_len, buf, sizeof(buf), &n), SL_OK);
            CHECK_EQ(n, m->ct_len);
            CHECK(memcmp(buf, m->ct, m->ct_len) == 0);
            CHECK_EQ(sl_handshake_read(r, buf, n, pt, sizeof(pt), &pn), SL_OK);
            CHECK_EQ(pn, m->payload_len);
            CHECK(memcmp(pt, m->payload, pn) == 0);
            if (i == hs_msgs - 1) {
                CHECK(sl_handshake_done(&ini) && sl_handshake_done(&res));
                CHECK(memcmp(sl_handshake_remote_static(&res), is.pub, 32) == 0);
                CHECK(memcmp(sl_handshake_remote_static(&ini), rs.pub, 32) == 0);
                CHECK_EQ(sl_handshake_split(&ini, &si), SL_OK);
                CHECK_EQ(sl_handshake_split(&res, &sr), SL_OK);
                CHECK(memcmp(si.handshake_hash, v->handshake_hash, 32) == 0);
                CHECK(memcmp(sr.handshake_hash, v->handshake_hash, 32) == 0);
            }
        } else {
            sl_session_t *w = from_init ? &si : &sr, *r = from_init ? &sr : &si;
            CHECK_EQ(sl_session_encrypt(w, m->payload, m->payload_len, buf, sizeof(buf), &n), SL_OK);
            CHECK_EQ(n, m->ct_len);
            CHECK(memcmp(buf, m->ct, m->ct_len) == 0);
            CHECK_EQ(sl_session_decrypt(r, buf, n, pt, sizeof(pt), &pn), SL_OK);
            CHECK(pn == m->payload_len && memcmp(pt, m->payload, pn) == 0);
        }
    }
}

TEST(test_noise_ik_vector) { run_vector(&nv_ik, SL_PATTERN_IK); }
TEST(test_noise_xx_vector) { run_vector(&nv_xx, SL_PATTERN_XX); }

static bool counter_rng(void *ctx, uint8_t *buf, size_t len)
{
    uint8_t *c = ctx;
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(++*c * 101u);
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
    sl_keypair_t a, b, wrong;
    keypair(&a, 1);
    keypair(&b, 2);
    keypair(&wrong, 3);
    uint8_t rng = 0;
    sl_handshake_t ini, res;
    sl_handshake_init(&ini, SL_PATTERN_IK, SL_INITIATOR, NULL, 0, &a, wrong.pub, counter_rng, &rng);
    sl_handshake_init(&res, SL_PATTERN_IK, SL_RESPONDER, NULL, 0, &b, NULL, counter_rng, &rng);
    uint8_t buf[256], pt[64];
    size_t n, pn;
    CHECK_EQ(sl_handshake_write(&ini, NULL, 0, buf, sizeof(buf), &n), SL_OK);
    CHECK_EQ(sl_handshake_read(&res, buf, n, pt, sizeof(pt), &pn), SL_ERR_DECRYPT);
    CHECK_EQ(sl_handshake_read(&res, buf, n, pt, sizeof(pt), &pn), SL_ERR_STATE); /* stays failed */
}

TEST(test_prologue_mismatch_fails)
{
    sl_keypair_t a, b;
    keypair(&a, 1);
    keypair(&b, 2);
    uint8_t rng = 0;
    sl_handshake_t ini, res;
    sl_handshake_init(&ini, SL_PATTERN_IK, SL_INITIATOR, (const uint8_t *)"v1", 2, &a, b.pub, counter_rng, &rng);
    sl_handshake_init(&res, SL_PATTERN_IK, SL_RESPONDER, (const uint8_t *)"v2", 2, &b, NULL, counter_rng, &rng);
    uint8_t buf[256], pt[64];
    size_t n, pn;
    sl_handshake_write(&ini, NULL, 0, buf, sizeof(buf), &n);
    CHECK_EQ(sl_handshake_read(&res, buf, n, pt, sizeof(pt), &pn), SL_ERR_DECRYPT);
}

TEST(test_session_rejects_replay_and_tamper)
{
    sl_keypair_t a, b;
    keypair(&a, 1);
    keypair(&b, 2);
    uint8_t rng = 0;
    sl_handshake_t ini, res;
    sl_handshake_init(&ini, SL_PATTERN_IK, SL_INITIATOR, NULL, 0, &a, b.pub, counter_rng, &rng);
    sl_handshake_init(&res, SL_PATTERN_IK, SL_RESPONDER, NULL, 0, &b, NULL, counter_rng, &rng);
    uint8_t buf[256], pt[64];
    size_t n, pn;
    sl_handshake_write(&ini, NULL, 0, buf, sizeof(buf), &n);
    sl_handshake_read(&res, buf, n, pt, sizeof(pt), &pn);
    sl_handshake_write(&res, NULL, 0, buf, sizeof(buf), &n);
    sl_handshake_read(&ini, buf, n, pt, sizeof(pt), &pn);
    sl_session_t si, sr;
    CHECK_EQ(sl_handshake_split(&ini, &si), SL_OK);
    CHECK_EQ(sl_handshake_split(&res, &sr), SL_OK);

    uint8_t m1[64];
    size_t m1_len;
    sl_session_encrypt(&si, (const uint8_t *)"unlock", 6, m1, sizeof(m1), &m1_len);
    CHECK_EQ(sl_session_decrypt(&sr, m1, m1_len, pt, sizeof(pt), &pn), SL_OK);
    CHECK_EQ(sl_session_decrypt(&sr, m1, m1_len, pt, sizeof(pt), &pn), SL_ERR_DECRYPT); /* replay */

    sl_session_encrypt(&si, (const uint8_t *)"lock", 4, buf, sizeof(buf), &n);
    buf[0] ^= 1;
    CHECK_EQ(sl_session_decrypt(&sr, buf, n, pt, sizeof(pt), &pn), SL_ERR_DECRYPT);
}

TEST(test_handshake_out_of_turn)
{
    sl_keypair_t a, b;
    keypair(&a, 1);
    keypair(&b, 2);
    sl_handshake_t res;
    sl_handshake_init(&res, SL_PATTERN_XX, SL_RESPONDER, NULL, 0, &b, NULL, NULL, NULL);
    uint8_t buf[256];
    size_t n;
    CHECK_EQ(sl_handshake_write(&res, NULL, 0, buf, sizeof(buf), &n), SL_ERR_STATE);
    sl_handshake_t ini;
    CHECK_EQ(sl_handshake_init(&ini, SL_PATTERN_IK, SL_INITIATOR, NULL, 0, &a, NULL, NULL, NULL),
             SL_ERR_ARG); /* IK initiator needs rs */
}

TEST(test_handshake_truncated_message)
{
    sl_keypair_t a, b;
    keypair(&a, 1);
    keypair(&b, 2);
    uint8_t rng = 0;
    sl_handshake_t ini, res;
    sl_handshake_init(&ini, SL_PATTERN_IK, SL_INITIATOR, NULL, 0, &a, b.pub, counter_rng, &rng);
    sl_handshake_init(&res, SL_PATTERN_IK, SL_RESPONDER, NULL, 0, &b, NULL, counter_rng, &rng);
    uint8_t buf[256], pt[64];
    size_t n, pn;
    sl_handshake_write(&ini, NULL, 0, buf, sizeof(buf), &n);
    CHECK_EQ(sl_handshake_read(&res, buf, 40, pt, sizeof(pt), &pn), SL_ERR_BUF);
}
