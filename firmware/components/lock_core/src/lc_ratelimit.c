#include "lock_core/lc_ratelimit.h"

#include <string.h>

void lc_rl_init(lc_ratelimit_t *rl) { memset(rl, 0, sizeof(*rl)); }

uint32_t lc_rl_lockout_s(const lc_ratelimit_cfg_t *cfg, uint8_t level)
{
    uint32_t base = cfg->base_lockout_s ? cfg->base_lockout_s : 1;
    uint32_t max = cfg->max_lockout_s > base ? cfg->max_lockout_s : base;
    uint32_t d = base;
    for (uint8_t i = 1; i < level && d < max; i++) {
        d = d > max / 2 ? max : d * 2;
    }
    return d > max ? max : d;
}

void lc_rl_on_boot(lc_ratelimit_t *rl, const lc_ratelimit_cfg_t *cfg, uint64_t now_ms)
{
    if (rl->locked) {
        rl->locked_until_ms = now_ms + (uint64_t)lc_rl_lockout_s(cfg, rl->lockout_level) * 1000u;
    }
}

bool lc_rl_is_locked(lc_ratelimit_t *rl, uint64_t now_ms)
{
    if (rl->locked && now_ms >= rl->locked_until_ms) {
        rl->locked = false;
    }
    return rl->locked;
}

bool lc_rl_on_failure(lc_ratelimit_t *rl, const lc_ratelimit_cfg_t *cfg, uint64_t now_ms)
{
    uint8_t limit = cfg->limit ? cfg->limit : 1;
    if (rl->wrong_count < UINT8_MAX) {
        rl->wrong_count++;
    }
    if (rl->wrong_count < limit) {
        return false;
    }
    rl->wrong_count = 0;
    if (rl->lockout_level < UINT8_MAX) {
        rl->lockout_level++;
    }
    rl->locked = true;
    rl->locked_until_ms = now_ms + (uint64_t)lc_rl_lockout_s(cfg, rl->lockout_level) * 1000u;
    return true;
}

void lc_rl_on_success(lc_ratelimit_t *rl) { lc_rl_init(rl); }
