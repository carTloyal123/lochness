#include "lock_core/lc_log.h"
#include "lock_core/lc_types.h"
#include "test.h"
#include <stdint.h>

static const lc_log_cfg_t LOG_ON = {.enabled = true, .max_age_days = 1};

static void add(lc_log_t *log, uint32_t unix_s, uint16_t user)
{
    lc_log_entry_t e = {.unix_s = unix_s, .user_index = user};
    lc_log_append(log, &LOG_ON, &e);
}

TEST(test_log_order_and_wrap)
{
    lc_log_t log;
    lc_log_init(&log);
    enum { OVERFLOW = 10 }; /* entries written past capacity */
    for (uint16_t i = 0; i < (uint16_t)(LC_LOG_CAPACITY + OVERFLOW); i++) {
        add(&log, 0, i);
    }
    CHECK_EQ(log.count, LC_LOG_CAPACITY);
    CHECK_EQ(lc_log_get(&log, 0)->user_index, OVERFLOW);
    CHECK_EQ(lc_log_get(&log, LC_LOG_CAPACITY - 1)->user_index, LC_LOG_CAPACITY + OVERFLOW - 1);
    CHECK(lc_log_get(&log, LC_LOG_CAPACITY) == NULL);
}

TEST(test_log_disabled_drops)
{
    lc_log_t log;
    lc_log_init(&log);
    lc_log_cfg_t off = {.enabled = false};
    lc_log_entry_t e = {.user_index = 1};
    lc_log_append(&log, &off, &e);
    CHECK_EQ(log.count, 0);

    add(&log, 0, 1);
    CHECK_EQ(lc_log_prune(&log, &off, 0), 1); /* disabling clears history */
    CHECK_EQ(log.count, 0);
}

TEST(test_log_prune_by_age)
{
    lc_log_t log;
    lc_log_init(&log);
    /* max_age_days is 1, so entries from day 1 are stale by day 10. */
    const uint32_t old_s = 1 * 86400;
    const uint32_t now_s = 10 * 86400;
    const uint32_t step_s = 5; /* spacing of the fresh entries */
    /* Fill past capacity so the ring has wrapped before pruning. */
    for (uint16_t i = 0; i < LC_LOG_CAPACITY - 2; i++) {
        add(&log, old_s, 1); /* old */
    }
    add(&log, 0, 2);           /* unknown time: kept */
    add(&log, old_s, 1);       /* old */
    add(&log, now_s, 3);       /* fresh */
    add(&log, now_s + step_s, 4); /* fresh, wraps */

    uint16_t removed = lc_log_prune(&log, &LOG_ON, now_s + (2 * step_s));
    CHECK_EQ(removed, LC_LOG_CAPACITY - 3);
    CHECK_EQ(log.count, 3);
    CHECK_EQ(lc_log_get(&log, 0)->user_index, 2);
    CHECK_EQ(lc_log_get(&log, 1)->user_index, 3);
    CHECK_EQ(lc_log_get(&log, 2)->user_index, 4);

    /* Appending after a prune continues from the compacted ring. */
    const uint16_t next_user = 5;
    add(&log, now_s + (4 * step_s), next_user);
    CHECK_EQ(log.count, 4);
    CHECK_EQ(lc_log_get(&log, 3)->user_index, next_user);
}
