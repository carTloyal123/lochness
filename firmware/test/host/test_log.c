#include "lock_core/lc_log.h"
#include "test.h"

static const lc_log_cfg_t ON = {.enabled = true, .max_age_days = 1};

static void add(lc_log_t *log, uint32_t unix_s, uint16_t user)
{
    lc_log_entry_t e = {.unix_s = unix_s, .user_index = user};
    lc_log_append(log, &ON, &e);
}

TEST(test_log_order_and_wrap)
{
    lc_log_t log;
    lc_log_init(&log);
    for (uint16_t i = 0; i < LC_LOG_CAPACITY + 10; i++) {
        add(&log, 0, i);
    }
    CHECK_EQ(log.count, LC_LOG_CAPACITY);
    CHECK_EQ(lc_log_get(&log, 0)->user_index, 10);
    CHECK_EQ(lc_log_get(&log, LC_LOG_CAPACITY - 1)->user_index, LC_LOG_CAPACITY + 9);
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
    const uint32_t day = 86400;
    /* Fill past capacity so the ring has wrapped before pruning. */
    for (uint16_t i = 0; i < LC_LOG_CAPACITY - 2; i++) {
        add(&log, 1 * day, 1); /* old */
    }
    add(&log, 0, 2);           /* unknown time: kept */
    add(&log, 1 * day, 1);     /* old */
    add(&log, 10 * day, 3);    /* fresh */
    add(&log, 10 * day + 5, 4); /* fresh, wraps */

    uint16_t removed = lc_log_prune(&log, &ON, 10 * day + 10);
    CHECK_EQ(removed, LC_LOG_CAPACITY - 3);
    CHECK_EQ(log.count, 3);
    CHECK_EQ(lc_log_get(&log, 0)->user_index, 2);
    CHECK_EQ(lc_log_get(&log, 1)->user_index, 3);
    CHECK_EQ(lc_log_get(&log, 2)->user_index, 4);

    add(&log, 10 * day + 20, 5);
    CHECK_EQ(log.count, 4);
    CHECK_EQ(lc_log_get(&log, 3)->user_index, 5);
}
