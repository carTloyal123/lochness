/*
 * Wrong-code rate limiting with exponential backoff.
 *
 * After cfg.limit consecutive wrong codes the keypad is locked out for
 * base_lockout_s; each further lockout without an intervening success doubles
 * the duration up to max_lockout_s.
 *
 * The state must be persisted after every failure, before the result is
 * reported, so pulling the batteries cannot reset the counter. Monotonic time
 * restarts at boot, so lc_rl_on_boot() re-arms any active lockout at its full
 * duration (conservative: rebooting never shortens a lockout).
 */
#pragma once

#include "lock_core/lc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t limit;          /* Matter WrongCodeEntryLimit, 1..255 */
    uint32_t base_lockout_s; /* Matter UserCodeTemporaryDisableTime, >= 1 */
    uint32_t max_lockout_s;
} lc_ratelimit_cfg_t;

typedef struct {
    uint8_t wrong_count;   /* consecutive failures since last lockout/success */
    uint8_t lockout_level; /* number of lockouts since last success */
    bool locked;
    uint64_t locked_until_ms; /* monotonic; only meaningful when locked */
} lc_ratelimit_t;

void lc_rl_init(lc_ratelimit_t *rl);
void lc_rl_on_boot(lc_ratelimit_t *rl, const lc_ratelimit_cfg_t *cfg, uint64_t now_ms);
bool lc_rl_is_locked(lc_ratelimit_t *rl, uint64_t now_ms);
/* Returns true if this failure started a lockout. */
bool lc_rl_on_failure(lc_ratelimit_t *rl, const lc_ratelimit_cfg_t *cfg, uint64_t now_ms);
void lc_rl_on_success(lc_ratelimit_t *rl);
/* Lockout duration for a given level (1 = first lockout). */
uint32_t lc_rl_lockout_s(const lc_ratelimit_cfg_t *cfg, uint8_t level);

#ifdef __cplusplus
}
#endif
