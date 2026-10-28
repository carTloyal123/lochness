#include "fake_platform.h"

#include <string.h>

static uint64_t f_now(void *ctx) { return ((fake_t *)ctx)->now_ms; }

static bool f_wall(void *ctx, lc_wall_time_t *out)
{
    fake_t *f = ctx;
    if (f->wall_known) {
        *out = f->wall;
    }
    return f->wall_known;
}

/* Not cryptographic: FNV-1a stretched to 32 bytes. Good enough to exercise
 * salting and matching logic. */
static lc_err_t f_digest(void *ctx, const uint8_t salt[LC_SALT_LEN], const uint8_t *pin,
                         size_t pin_len, uint8_t out[LC_DIGEST_LEN])
{
    fake_t *f = ctx;
    if (f->digest_fail) {
        return LC_ERR_PLATFORM;
    }
    for (uint8_t block = 0; block < LC_DIGEST_LEN / 4; block++) {
        uint32_t h = 2166136261u ^ block;
        for (size_t i = 0; i < LC_SALT_LEN; i++) {
            h = (h ^ salt[i]) * 16777619u;
        }
        for (size_t i = 0; i < pin_len; i++) {
            h = (h ^ pin[i]) * 16777619u;
        }
        memcpy(&out[block * 4], &h, 4);
    }
    return LC_OK;
}

static lc_err_t f_random(void *ctx, uint8_t *buf, size_t len)
{
    fake_t *f = ctx;
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(++f->rng_counter * 37u);
    }
    return LC_OK;
}

static void f_motor_start(void *ctx, lc_op_t op)
{
    fake_t *f = ctx;
    f->motor_starts++;
    f->last_motor_op = op;
}

static void f_motor_stop(void *ctx) { ((fake_t *)ctx)->motor_stops++; }

static void f_notify(void *ctx, const lc_event_t *ev)
{
    fake_t *f = ctx;
    if (f->notify_count < (int)(sizeof(f->events) / sizeof(f->events[0]))) {
        f->events[f->notify_count] = *ev;
    }
    f->notify_count++;
}

static void f_persist(void *ctx, lc_persist_t what) { ((fake_t *)ctx)->persist_counts[what]++; }

void fake_reset(fake_t *f)
{
    memset(f, 0, sizeof(*f));
    f->now_ms = 1000;
}

lc_platform_t fake_platform(fake_t *f)
{
    return (lc_platform_t){
        .ctx = f, .now_ms = f_now, .wall_time = f_wall, .pin_digest = f_digest, .random = f_random};
}

lc_hooks_t fake_hooks(fake_t *f)
{
    return (lc_hooks_t){.ctx = f,
                        .motor_start = f_motor_start,
                        .motor_stop = f_motor_stop,
                        .notify = f_notify,
                        .persist = f_persist};
}

const lc_event_t *fake_last_event(const fake_t *f)
{
    if (f->notify_count == 0 || f->notify_count > (int)(sizeof(f->events) / sizeof(f->events[0]))) {
        return NULL;
    }
    return &f->events[f->notify_count - 1];
}
