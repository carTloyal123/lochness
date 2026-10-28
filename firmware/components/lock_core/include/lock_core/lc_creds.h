/*
 * Users and PIN credentials, modeled on the Matter Door Lock cluster.
 *
 * lc_creds_t is plain data with no pointers so the platform can persist it as
 * a single blob. PINs are never stored; only salted, hardware-keyed digests.
 */
#pragma once

#include "lock_core/lc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LC_USER_UNRESTRICTED,
    LC_USER_WEEKDAY_SCHEDULE, /* access only inside one of its week-day schedules */
    LC_USER_DISPOSABLE,       /* deleted after first successful use */
    LC_USER_NON_ACCESS,       /* recognized but never grants access */
} lc_user_type_t;

typedef struct {
    uint8_t days_mask; /* bit 0 = Sunday ... bit 6 = Saturday */
    uint16_t start_min; /* inclusive, minute of day */
    uint16_t end_min;   /* exclusive, minute of day; must be > start_min */
} lc_weekday_schedule_t;

typedef struct {
    bool in_use;
    bool enabled;
    lc_user_type_t type;
    char name[LC_USER_NAME_MAX + 1];
    uint8_t schedule_count;
    lc_weekday_schedule_t schedules[LC_MAX_WEEKDAY_SCHEDULES];
} lc_user_t;

typedef struct {
    bool in_use;
    uint16_t user_index;
    uint8_t salt[LC_SALT_LEN];
    uint8_t digest[LC_DIGEST_LEN];
} lc_credential_t;

typedef struct {
    lc_user_t users[LC_MAX_USERS];             /* users[i] is user index i + 1 */
    lc_credential_t creds[LC_MAX_CREDENTIALS]; /* creds[i] is credential index i + 1 */
} lc_creds_t;

void lc_creds_init(lc_creds_t *c);

/* Create or overwrite a user. name may be NULL; it is truncated to LC_USER_NAME_MAX. */
lc_err_t lc_user_set(lc_creds_t *c, uint16_t user_index, lc_user_type_t type, const char *name);
lc_err_t lc_user_set_enabled(lc_creds_t *c, uint16_t user_index, bool enabled);
lc_err_t lc_user_set_schedules(lc_creds_t *c, uint16_t user_index,
                               const lc_weekday_schedule_t *schedules, uint8_t count);
/* Removes the user and every credential belonging to it. */
lc_err_t lc_user_clear(lc_creds_t *c, uint16_t user_index);
const lc_user_t *lc_user_get(const lc_creds_t *c, uint16_t user_index);
/* First free user index, or LC_INDEX_NONE if full. */
uint16_t lc_user_next_free(const lc_creds_t *c);

/* PINs must be LC_PIN_MIN_LEN..LC_PIN_MAX_LEN ASCII digits. */
bool lc_pin_valid(const uint8_t *pin, size_t len);
/*
 * Store a PIN for an existing user at cred_index. Fails with LC_ERR_DUPLICATE if
 * any other credential already has the same PIN.
 */
lc_err_t lc_pin_set(lc_creds_t *c, const lc_platform_t *plat, uint16_t cred_index,
                    uint16_t user_index, const uint8_t *pin, size_t len);
lc_err_t lc_pin_clear(lc_creds_t *c, uint16_t cred_index);
/* First free credential index, or LC_INDEX_NONE if full. */
uint16_t lc_cred_next_free(const lc_creds_t *c);

/*
 * Find the credential matching pin. Evaluates every slot (no early exit) so
 * timing does not reveal which slot matched. On success writes the matching
 * credential index; returns LC_ERR_DENIED if nothing matches.
 */
lc_err_t lc_pin_match(const lc_creds_t *c, const lc_platform_t *plat, const uint8_t *pin,
                      size_t len, uint16_t *cred_index_out);

/*
 * Whether user_index may operate the lock right now. Schedule-restricted users
 * are denied (LC_ERR_SCHEDULE) when wall time is unknown: fail closed.
 */
lc_err_t lc_user_access_check(const lc_creds_t *c, uint16_t user_index,
                              const lc_wall_time_t *now /* NULL if unknown */);

#ifdef __cplusplus
}
#endif
