#include "lock_core/lc_ratelimit.h"
#include "test.h"

#include <stdint.h>

static const lc_ratelimit_cfg_t CFG = {.limit = 3, .base_lockout_s = 60, .max_lockout_s = 300};
/* Arbitrary monotonic times for the second burst and for the pre-reboot burst. */
static const uint64_t SECOND_BURST_MS = 100000;
static const uint64_t BEFORE_REBOOT_MS = 500000;
/* Monotonic clock after a battery pull: restarts near zero. */
static const uint64_t BOOT_CLOCK_MS = 10;

TEST(test_rl_lockout_after_limit)
{
    lc_ratelimit_t rl;
    lc_rl_init(&rl);
    CHECK(!lc_rl_on_failure(&rl, &CFG, 0));
    CHECK(!lc_rl_on_failure(&rl, &CFG, 0));
    CHECK(lc_rl_on_failure(&rl, &CFG, 0));
    CHECK(lc_rl_is_locked(&rl, 59999));
    CHECK(!lc_rl_is_locked(&rl, 60000));
}

TEST(test_rl_exponential_backoff_capped)
{
    CHECK_EQ(lc_rl_lockout_s(&CFG, 1), 60);
    CHECK_EQ(lc_rl_lockout_s(&CFG, 2), 120);
    CHECK_EQ(lc_rl_lockout_s(&CFG, 3), 240);
    CHECK_EQ(lc_rl_lockout_s(&CFG, 4), 300);
    CHECK_EQ(lc_rl_lockout_s(&CFG, 255), 300);

    lc_ratelimit_t rl;
    lc_rl_init(&rl);
    for (int i = 0; i < 3; i++) {
        lc_rl_on_failure(&rl, &CFG, 0);
    }
    for (int i = 0; i < 3; i++) {
        lc_rl_on_failure(&rl, &CFG, SECOND_BURST_MS);
    }
    CHECK_EQ(rl.lockout_level, 2);
    CHECK(lc_rl_is_locked(&rl, SECOND_BURST_MS + 119999));
    CHECK(!lc_rl_is_locked(&rl, SECOND_BURST_MS + 120000));
}

TEST(test_rl_success_resets)
{
    lc_ratelimit_t rl;
    lc_rl_init(&rl);
    lc_rl_on_failure(&rl, &CFG, 0);
    lc_rl_on_failure(&rl, &CFG, 0);
    lc_rl_on_success(&rl);
    CHECK(!lc_rl_on_failure(&rl, &CFG, 0));
    CHECK_EQ(rl.lockout_level, 0);
}

TEST(test_rl_reboot_rearms_full_lockout)
{
    lc_ratelimit_t rl;
    lc_rl_init(&rl);
    for (int i = 0; i < 3; i++) {
        lc_rl_on_failure(&rl, &CFG, BEFORE_REBOOT_MS);
    }
    /* Battery pulled; monotonic clock restarts near zero. */
    lc_rl_on_boot(&rl, &CFG, BOOT_CLOCK_MS);
    CHECK(lc_rl_is_locked(&rl, BOOT_CLOCK_MS + 59999));
    CHECK(!lc_rl_is_locked(&rl, BOOT_CLOCK_MS + 60000));
}

TEST(test_rl_wrong_count_survives_reboot)
{
    lc_ratelimit_t rl;
    lc_rl_init(&rl);
    lc_rl_on_failure(&rl, &CFG, 0);
    lc_rl_on_failure(&rl, &CFG, 0);
    lc_rl_on_boot(&rl, &CFG, 0);
    CHECK(lc_rl_on_failure(&rl, &CFG, 0));
}
