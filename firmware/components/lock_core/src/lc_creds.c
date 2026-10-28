#include "lock_core/lc_creds.h"
#include "lock_core/lc_types.h"

#include <stdint.h>
#include <string.h>

static const unsigned DAYS_MASK_ALL = 0x7FU; /* bits 0..6 = Sunday..Saturday */
enum {
    MINUTES_PER_DAY = 1440,
    LAST_WEEKDAY = 6, /* Saturday */
};

static bool user_index_valid(uint16_t i) { return i >= 1 && i <= LC_MAX_USERS; }
static bool cred_index_valid(uint16_t i) { return i >= 1 && i <= LC_MAX_CREDENTIALS; }

static uint8_t ct_diff(const uint8_t *a, const uint8_t *b, size_t len)
{
    unsigned diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (unsigned)a[i] ^ (unsigned)b[i];
    }
    return (uint8_t)diff;
}

static void wipe(void *p, size_t len)
{
    volatile uint8_t *v = p;
    while (len--) {
        *v++ = 0;
    }
}

void lc_creds_init(lc_creds_t *c) { memset(c, 0, sizeof(*c)); }

lc_err_t lc_user_set(lc_creds_t *c, uint16_t user_index, lc_user_type_t type, const char *name)
{
    if (!user_index_valid(user_index) || type > LC_USER_NON_ACCESS) {
        return LC_ERR_INVALID_ARG;
    }
    lc_user_t *user = &c->users[user_index - 1];
    bool existed = user->in_use;
    user->in_use = true;
    if (!existed) {
        user->enabled = true;
        user->schedule_count = 0;
    }
    user->type = type;
    memset(user->name, 0, sizeof(user->name));
    if (name) {
        strncpy(user->name, name, LC_USER_NAME_MAX);
    }
    return LC_OK;
}

lc_err_t lc_user_set_enabled(lc_creds_t *c, uint16_t user_index, bool enabled)
{
    if (!user_index_valid(user_index)) {
        return LC_ERR_INVALID_ARG;
    }
    lc_user_t *user = &c->users[user_index - 1];
    if (!user->in_use) {
        return LC_ERR_NOT_FOUND;
    }
    user->enabled = enabled;
    return LC_OK;
}

lc_err_t lc_user_set_schedules(lc_creds_t *c, uint16_t user_index,
                               const lc_weekday_schedule_t *schedules, uint8_t count)
{
    if (!user_index_valid(user_index) || count > LC_MAX_WEEKDAY_SCHEDULES ||
        (count && !schedules)) {
        return LC_ERR_INVALID_ARG;
    }
    for (uint8_t i = 0; i < count; i++) {
        const lc_weekday_schedule_t *s = &schedules[i];
        unsigned days = s->days_mask;
        if ((days & DAYS_MASK_ALL) == 0 || (days & ~DAYS_MASK_ALL) != 0 || s->start_min >= s->end_min ||
            s->end_min > MINUTES_PER_DAY) {
            return LC_ERR_INVALID_ARG;
        }
    }
    lc_user_t *user = &c->users[user_index - 1];
    if (!user->in_use) {
        return LC_ERR_NOT_FOUND;
    }
    memset(user->schedules, 0, sizeof(user->schedules));
    if (count) {
        memcpy(user->schedules, schedules, count * sizeof(*schedules));
    }
    user->schedule_count = count;
    return LC_OK;
}

lc_err_t lc_user_clear(lc_creds_t *c, uint16_t user_index)
{
    if (!user_index_valid(user_index)) {
        return LC_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < LC_MAX_CREDENTIALS; i++) {
        if (c->creds[i].in_use && c->creds[i].user_index == user_index) {
            wipe(&c->creds[i], sizeof(c->creds[i]));
        }
    }
    wipe(&c->users[user_index - 1], sizeof(c->users[0]));
    return LC_OK;
}

const lc_user_t *lc_user_get(const lc_creds_t *c, uint16_t user_index)
{
    if (!user_index_valid(user_index) || !c->users[user_index - 1].in_use) {
        return NULL;
    }
    return &c->users[user_index - 1];
}

uint16_t lc_user_next_free(const lc_creds_t *c)
{
    for (uint16_t i = 0; i < LC_MAX_USERS; i++) {
        if (!c->users[i].in_use) {
            return i + 1;
        }
    }
    return LC_INDEX_NONE;
}

bool lc_pin_valid(const uint8_t *pin, size_t len)
{
    if (!pin || len < LC_PIN_MIN_LEN || len > LC_PIN_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            return false;
        }
    }
    return true;
}

lc_err_t lc_pin_set(lc_creds_t *c, const lc_platform_t *plat, uint16_t cred_index,
                    uint16_t user_index, const uint8_t *pin, size_t len)
{
    if (!cred_index_valid(cred_index) || !user_index_valid(user_index) || !lc_pin_valid(pin, len)) {
        return LC_ERR_INVALID_ARG;
    }
    if (!c->users[user_index - 1].in_use) {
        return LC_ERR_NOT_FOUND;
    }

    uint16_t existing = LC_INDEX_NONE;
    lc_err_t err = lc_pin_match(c, plat, pin, len, &existing);
    if (err == LC_OK && existing != cred_index) {
        return LC_ERR_DUPLICATE;
    }
    if (err != LC_OK && err != LC_ERR_DENIED) {
        return err;
    }

    lc_credential_t next = {.in_use = true, .user_index = user_index};
    if (plat->random(plat->ctx, next.salt, sizeof(next.salt)) != LC_OK ||
        plat->pin_digest(plat->ctx, next.salt, pin, len, next.digest) != LC_OK) {
        wipe(&next, sizeof(next));
        return LC_ERR_PLATFORM;
    }
    c->creds[cred_index - 1] = next;
    wipe(&next, sizeof(next));
    return LC_OK;
}

lc_err_t lc_pin_clear(lc_creds_t *c, uint16_t cred_index)
{
    if (!cred_index_valid(cred_index)) {
        return LC_ERR_INVALID_ARG;
    }
    wipe(&c->creds[cred_index - 1], sizeof(c->creds[0]));
    return LC_OK;
}

uint16_t lc_cred_next_free(const lc_creds_t *c)
{
    for (uint16_t i = 0; i < LC_MAX_CREDENTIALS; i++) {
        if (!c->creds[i].in_use) {
            return i + 1;
        }
    }
    return LC_INDEX_NONE;
}

lc_err_t lc_pin_match(const lc_creds_t *c, const lc_platform_t *plat, const uint8_t *pin,
                      size_t len, uint16_t *cred_index_out)
{
    if (!lc_pin_valid(pin, len)) {
        return LC_ERR_DENIED;
    }
    uint16_t found = LC_INDEX_NONE;
    bool platform_failed = false;
    uint8_t digest[LC_DIGEST_LEN];

    /* Digest every slot, used or not, so timing is independent of where (or
     * whether) the PIN matches. */
    for (uint16_t i = 0; i < LC_MAX_CREDENTIALS; i++) {
        const lc_credential_t *cred = &c->creds[i];
        if (plat->pin_digest(plat->ctx, cred->salt, pin, len, digest) != LC_OK) {
            platform_failed = true;
            continue;
        }
        uint8_t diff = ct_diff(digest, cred->digest, LC_DIGEST_LEN);
        if (diff == 0 && cred->in_use && found == LC_INDEX_NONE) {
            found = i + 1;
        }
    }
    wipe(digest, sizeof(digest));

    if (found != LC_INDEX_NONE) {
        if (cred_index_out) {
            *cred_index_out = found;
        }
        return LC_OK;
    }
    return platform_failed ? LC_ERR_PLATFORM : LC_ERR_DENIED;
}

static bool in_schedule(const lc_weekday_schedule_t *s, const lc_wall_time_t *t)
{
    return (s->days_mask & (1U << t->weekday)) && t->minute_of_day >= s->start_min &&
           t->minute_of_day < s->end_min;
}

lc_err_t lc_user_access_check(const lc_creds_t *c, uint16_t user_index, const lc_wall_time_t *now)
{
    const lc_user_t *user = lc_user_get(c, user_index);
    if (!user || !user->enabled || user->type == LC_USER_NON_ACCESS) {
        return LC_ERR_DENIED;
    }
    if (user->type != LC_USER_WEEKDAY_SCHEDULE) {
        return LC_OK;
    }
    if (!now || now->weekday > LAST_WEEKDAY) {
        return LC_ERR_SCHEDULE;
    }
    for (uint8_t i = 0; i < user->schedule_count; i++) {
        if (in_schedule(&user->schedules[i], now)) {
            return LC_OK;
        }
    }
    return LC_ERR_SCHEDULE;
}
