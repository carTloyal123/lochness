#include "fake_platform.h"
#include "lock_core/lc_lock.h"
#include "lock_core/lc_types.h"

#include <stdint.h>
#include <string.h>

static uint64_t f_now(void *ctx) { return ((fake_t *)ctx)->now_ms; }

static bool f_wall(void *ctx, lc_wall_time_t *out)
{
    fake_t *fake = ctx;
    if (fake->wall_known) {
        *out = fake->wall;
    }
    return fake->wall_known;
}

enum { DIGEST_WORD = sizeof(uint32_t) };
static const uint32_t FNV_OFFSET_BASIS = 2166136261U;
static const uint32_t FNV_PRIME = 16777619U;
/* Odd multiplier, so the fake RNG cycles through every byte value. */
static const unsigned RNG_STEP = 37U;

/* Not cryptographic: FNV-1a stretched to 32 bytes. Good enough to exercise
 * salting and matching logic. */
static lc_err_t f_digest(void *ctx, const uint8_t salt[LC_SALT_LEN], const uint8_t *pin,
                         size_t pin_len, uint8_t out[LC_DIGEST_LEN])
{
    fake_t *fake = ctx;
    if (fake->digest_fail) {
        return LC_ERR_PLATFORM;
    }
    for (size_t block = 0; block < LC_DIGEST_LEN / DIGEST_WORD; block++) {
        uint32_t hash = FNV_OFFSET_BASIS ^ (uint32_t)block;
        for (size_t i = 0; i < LC_SALT_LEN; i++) {
            hash = (hash ^ salt[i]) * FNV_PRIME;
        }
        for (size_t i = 0; i < pin_len; i++) {
            hash = (hash ^ pin[i]) * FNV_PRIME;
        }
        memcpy(&out[block * DIGEST_WORD], &hash, DIGEST_WORD);
    }
    return LC_OK;
}

static lc_err_t f_random(void *ctx, uint8_t *buf, size_t len)
{
    fake_t *fake = ctx;
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(++fake->rng_counter * RNG_STEP);
    }
    return LC_OK;
}

static void f_motor_start(void *ctx, lc_op_t op)
{
    fake_t *fake = ctx;
    fake->motor_starts++;
    fake->last_motor_op = op;
}

static void f_motor_stop(void *ctx) { ((fake_t *)ctx)->motor_stops++; }

static void f_notify(void *ctx, const lc_event_t *ev)
{
    fake_t *fake = ctx;
    if (fake->notify_count < (int)(sizeof(fake->events) / sizeof(fake->events[0]))) {
        fake->events[fake->notify_count] = *ev;
    }
    fake->notify_count++;
}

static void f_persist(void *ctx, lc_persist_t what) { ((fake_t *)ctx)->persist_counts[what]++; }

void fake_reset(fake_t *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->now_ms = 1000;
}

lc_platform_t fake_platform(fake_t *fake)
{
    return (lc_platform_t){
        .ctx = fake, .now_ms = f_now, .wall_time = f_wall, .pin_digest = f_digest, .random = f_random};
}

lc_hooks_t fake_hooks(fake_t *fake)
{
    return (lc_hooks_t){.ctx = fake,
                        .motor_start = f_motor_start,
                        .motor_stop = f_motor_stop,
                        .notify = f_notify,
                        .persist = f_persist};
}

const lc_event_t *fake_last_event(const fake_t *fake)
{
    if (fake->notify_count == 0 || fake->notify_count > (int)(sizeof(fake->events) / sizeof(fake->events[0]))) {
        return NULL;
    }
    return &fake->events[fake->notify_count - 1];
}
