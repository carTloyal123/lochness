#include <string.h>

#include "fake_platform.h"
#include "lock_core/lc_creds.h"
#include "test.h"

#define PIN(s) (const uint8_t *)(s), strlen(s)

TEST(test_pin_validation)
{
    CHECK(lc_pin_valid(PIN("1234")));
    CHECK(lc_pin_valid(PIN("12345678")));
    CHECK(!lc_pin_valid(PIN("123")));
    CHECK(!lc_pin_valid(PIN("123456789")));
    CHECK(!lc_pin_valid(PIN("12a4")));
    CHECK(!lc_pin_valid(NULL, 4));
}

TEST(test_pin_set_and_match)
{
    fake_t f;
    fake_reset(&f);
    lc_platform_t plat = fake_platform(&f);
    lc_creds_t c;
    lc_creds_init(&c);

    CHECK_EQ(lc_pin_set(&c, &plat, 1, 1, PIN("1234")), LC_ERR_NOT_FOUND); /* no such user */
    CHECK_EQ(lc_user_set(&c, 1, LC_USER_UNRESTRICTED, "Alice"), LC_OK);
    CHECK_EQ(lc_user_set(&c, 2, LC_USER_UNRESTRICTED, "Bob"), LC_OK);
    CHECK_EQ(lc_pin_set(&c, &plat, 1, 1, PIN("1234")), LC_OK);
    CHECK_EQ(lc_pin_set(&c, &plat, 5, 2, PIN("987654")), LC_OK);

    uint16_t idx = 0;
    CHECK_EQ(lc_pin_match(&c, &plat, PIN("1234"), &idx), LC_OK);
    CHECK_EQ(idx, 1);
    CHECK_EQ(lc_pin_match(&c, &plat, PIN("987654"), &idx), LC_OK);
    CHECK_EQ(idx, 5);
    CHECK_EQ(lc_pin_match(&c, &plat, PIN("0000"), &idx), LC_ERR_DENIED);
    CHECK_EQ(lc_pin_match(&c, &plat, PIN("12345"), &idx), LC_ERR_DENIED); /* prefix must not match */
}

TEST(test_pin_not_stored_in_clear)
{
    fake_t f;
    fake_reset(&f);
    lc_platform_t plat = fake_platform(&f);
    lc_creds_t c;
    lc_creds_init(&c);
    lc_user_set(&c, 1, LC_USER_UNRESTRICTED, NULL);
    lc_pin_set(&c, &plat, 1, 1, PIN("24681357"));

    const uint8_t *raw = (const uint8_t *)&c;
    bool found = false;
    for (size_t i = 0; i + 8 <= sizeof(c); i++) {
        if (memcmp(raw + i, "24681357", 8) == 0) {
            found = true;
        }
    }
    CHECK(!found);
}

TEST(test_same_pin_different_salt)
{
    fake_t f;
    fake_reset(&f);
    lc_platform_t plat = fake_platform(&f);
    lc_creds_t c;
    lc_creds_init(&c);
    lc_user_set(&c, 1, LC_USER_UNRESTRICTED, NULL);
    lc_pin_set(&c, &plat, 1, 1, PIN("1111"));
    uint8_t first[LC_DIGEST_LEN];
    memcpy(first, c.creds[0].digest, sizeof(first));
    /* Re-setting the same slot re-salts. */
    CHECK_EQ(lc_pin_set(&c, &plat, 1, 1, PIN("1111")), LC_OK);
    CHECK(memcmp(first, c.creds[0].digest, sizeof(first)) != 0);
}

TEST(test_duplicate_pin_rejected)
{
    fake_t f;
    fake_reset(&f);
    lc_platform_t plat = fake_platform(&f);
    lc_creds_t c;
    lc_creds_init(&c);
    lc_user_set(&c, 1, LC_USER_UNRESTRICTED, NULL);
    lc_user_set(&c, 2, LC_USER_UNRESTRICTED, NULL);
    CHECK_EQ(lc_pin_set(&c, &plat, 1, 1, PIN("4321")), LC_OK);
    CHECK_EQ(lc_pin_set(&c, &plat, 2, 2, PIN("4321")), LC_ERR_DUPLICATE);
    CHECK(!c.creds[1].in_use);
}

TEST(test_user_clear_removes_credentials)
{
    fake_t f;
    fake_reset(&f);
    lc_platform_t plat = fake_platform(&f);
    lc_creds_t c;
    lc_creds_init(&c);
    lc_user_set(&c, 3, LC_USER_UNRESTRICTED, "Carol");
    lc_pin_set(&c, &plat, 1, 3, PIN("1357"));
    lc_pin_set(&c, &plat, 2, 3, PIN("2468"));
    CHECK_EQ(lc_user_clear(&c, 3), LC_OK);
    CHECK(lc_user_get(&c, 3) == NULL);
    CHECK_EQ(lc_pin_match(&c, &plat, PIN("1357"), NULL), LC_ERR_DENIED);
    CHECK_EQ(lc_pin_match(&c, &plat, PIN("2468"), NULL), LC_ERR_DENIED);
    CHECK_EQ(lc_cred_next_free(&c), 1);
    CHECK_EQ(lc_user_next_free(&c), 1);
}

TEST(test_name_truncated)
{
    lc_creds_t c;
    lc_creds_init(&c);
    lc_user_set(&c, 1, LC_USER_UNRESTRICTED, "AVeryLongUserName");
    CHECK_EQ(strlen(lc_user_get(&c, 1)->name), LC_USER_NAME_MAX);
}

TEST(test_schedule_access)
{
    lc_creds_t c;
    lc_creds_init(&c);
    lc_user_set(&c, 1, LC_USER_WEEKDAY_SCHEDULE, "Cleaner");
    /* Mon + Wed, 09:00-12:00 */
    lc_weekday_schedule_t s = {.days_mask = (1 << 1) | (1 << 3), .start_min = 540, .end_min = 720};
    CHECK_EQ(lc_user_set_schedules(&c, 1, &s, 1), LC_OK);

    lc_wall_time_t mon_10 = {.weekday = 1, .minute_of_day = 600};
    lc_wall_time_t mon_12 = {.weekday = 1, .minute_of_day = 720};
    lc_wall_time_t tue_10 = {.weekday = 2, .minute_of_day = 600};
    CHECK_EQ(lc_user_access_check(&c, 1, &mon_10), LC_OK);
    CHECK_EQ(lc_user_access_check(&c, 1, &mon_12), LC_ERR_SCHEDULE); /* end exclusive */
    CHECK_EQ(lc_user_access_check(&c, 1, &tue_10), LC_ERR_SCHEDULE);
    CHECK_EQ(lc_user_access_check(&c, 1, NULL), LC_ERR_SCHEDULE); /* fail closed */

    lc_weekday_schedule_t bad = {.days_mask = 0, .start_min = 0, .end_min = 10};
    CHECK_EQ(lc_user_set_schedules(&c, 1, &bad, 1), LC_ERR_INVALID_ARG);
}

TEST(test_disabled_and_non_access_users)
{
    lc_creds_t c;
    lc_creds_init(&c);
    lc_user_set(&c, 1, LC_USER_UNRESTRICTED, NULL);
    lc_user_set(&c, 2, LC_USER_NON_ACCESS, NULL);
    CHECK_EQ(lc_user_access_check(&c, 1, NULL), LC_OK);
    CHECK_EQ(lc_user_access_check(&c, 2, NULL), LC_ERR_DENIED);
    lc_user_set_enabled(&c, 1, false);
    CHECK_EQ(lc_user_access_check(&c, 1, NULL), LC_ERR_DENIED);
    CHECK_EQ(lc_user_access_check(&c, 9, NULL), LC_ERR_DENIED);
}

TEST(test_digest_failure_is_platform_error)
{
    fake_t f;
    fake_reset(&f);
    lc_platform_t plat = fake_platform(&f);
    lc_creds_t c;
    lc_creds_init(&c);
    lc_user_set(&c, 1, LC_USER_UNRESTRICTED, NULL);
    f.digest_fail = true;
    CHECK_EQ(lc_pin_set(&c, &plat, 1, 1, PIN("1234")), LC_ERR_PLATFORM);
    CHECK(!c.creds[0].in_use);
    CHECK_EQ(lc_pin_match(&c, &plat, PIN("1234"), NULL), LC_ERR_PLATFORM);
}
