#include <string.h>

#include "fake_platform.h"
#include "lock_core/lc_lock.h"
#include "test.h"

#define PIN(s) (const uint8_t *)(s), strlen(s)

static fake_t f;
static lc_t lc;

/* Locked, door closed, Alice (user 1) has PIN 1234. */
static void setup(lc_config_t cfg)
{
    fake_reset(&f);
    lc_platform_t plat = fake_platform(&f);
    lc_hooks_t hooks = fake_hooks(&f);
    lc_init(&lc, &plat, &hooks, &cfg);
    lc_user_set(&lc.creds, 1, LC_USER_UNRESTRICTED, "Alice");
    lc_pin_set(&lc.creds, &lc.plat, 1, 1, PIN("1234"));
    lc_boot(&lc, LC_BOLT_LOCKED, LC_DOOR_CLOSED);
}

static lc_config_t cfg_attributed(void)
{
    lc_config_t cfg = lc_config_default();
    cfg.private_events = false;
    return cfg;
}

TEST(test_keypad_unlock_happy_path)
{
    setup(cfg_attributed());
    uint16_t user = 0;
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), &user), LC_OK);
    CHECK_EQ(user, 1);
    CHECK_EQ(lc.bolt, LC_BOLT_UNLOCKING);
    CHECK_EQ(f.motor_starts, 1);
    CHECK_EQ(f.last_motor_op, LC_OP_UNLOCK);
    CHECK_EQ(lc_request(&lc, LC_OP_LOCK, LC_SOURCE_KEYPAD, NULL, 0, NULL), LC_ERR_BUSY);

    lc_on_motor_done(&lc, true);
    CHECK_EQ(lc.bolt, LC_BOLT_UNLOCKED);
    const lc_event_t *ev = fake_last_event(&f);
    CHECK(ev != NULL);
    CHECK_EQ(ev->type, LC_EVENT_UNLOCKED);
    CHECK_EQ(ev->source, LC_SOURCE_KEYPAD);
    CHECK_EQ(ev->user_index, 1);
    CHECK_EQ(lc.log.count, 1);
}

TEST(test_keypad_unlock_requires_pin)
{
    setup(cfg_attributed());
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, NULL, 0, NULL), LC_ERR_DENIED);
    CHECK_EQ(f.motor_starts, 0);
}

TEST(test_one_touch_lock)
{
    lc_config_t cfg = cfg_attributed();
    setup(cfg);
    lc.bolt = LC_BOLT_UNLOCKED;
    CHECK_EQ(lc_request(&lc, LC_OP_LOCK, LC_SOURCE_KEYPAD, NULL, 0, NULL), LC_OK);
    CHECK_EQ(f.motor_starts, 1);

    cfg.one_touch_lock = false;
    setup(cfg);
    lc.bolt = LC_BOLT_UNLOCKED;
    CHECK_EQ(lc_request(&lc, LC_OP_LOCK, LC_SOURCE_KEYPAD, NULL, 0, NULL), LC_ERR_DENIED);
}

TEST(test_wrong_codes_lock_out_and_persist)
{
    setup(cfg_attributed()); /* limit 5, 60 s */
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("0000"), NULL), LC_ERR_DENIED);
    }
    CHECK_EQ(f.persist_counts[LC_PERSIST_RATELIMIT], 4);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("0000"), NULL), LC_ERR_DENIED);
    CHECK_EQ(fake_last_event(&f)->type, LC_EVENT_LOCKOUT_STARTED);

    /* Correct PIN is not even evaluated while locked out. */
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_ERR_LOCKED_OUT);
    CHECK_EQ(f.motor_starts, 0);

    f.now_ms += 60000;
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_OK);
    CHECK_EQ(lc.rl.lockout_level, 0);
}

TEST(test_lockout_applies_to_remote_pin)
{
    lc_config_t cfg = cfg_attributed();
    cfg.require_pin_for_remote = true;
    setup(cfg);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_ERR_DENIED);
    for (int i = 0; i < 5; i++) {
        lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, PIN("9999"), NULL);
    }
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_ERR_LOCKED_OUT);
}

TEST(test_remote_unlock_cap)
{
    lc_config_t cfg = cfg_attributed();
    cfg.remote_unlock_allowed = false;
    setup(cfg);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_ERR_MODE);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, PIN("1234"), NULL), LC_ERR_MODE);
    CHECK_EQ(f.motor_starts, 0);
    /* Remote lock is still fine. */
    lc.bolt = LC_BOLT_UNLOCKED;
    CHECK_EQ(lc_request(&lc, LC_OP_LOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_OK);
}

TEST(test_operating_modes)
{
    setup(cfg_attributed());
    lc_set_mode(&lc, LC_MODE_VACATION);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_ERR_MODE);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_OK);

    setup(cfg_attributed());
    lc_set_mode(&lc, LC_MODE_PRIVACY);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_ERR_MODE);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_ERR_MODE);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_APP, NULL, 0, NULL), LC_OK);

    setup(cfg_attributed());
    lc_set_mode(&lc, LC_MODE_NO_REMOTE);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_ERR_MODE);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_OK);
}

TEST(test_mode_denial_does_not_count_as_wrong_code)
{
    setup(cfg_attributed());
    lc_set_mode(&lc, LC_MODE_VACATION);
    for (int i = 0; i < 10; i++) {
        lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("0000"), NULL);
    }
    CHECK(!lc.rl.locked);
}

TEST(test_schedule_user)
{
    setup(cfg_attributed());
    lc_user_set(&lc.creds, 2, LC_USER_WEEKDAY_SCHEDULE, "Walker");
    lc_weekday_schedule_t s = {.days_mask = 0x3E, .start_min = 12 * 60, .end_min = 13 * 60};
    lc_user_set_schedules(&lc.creds, 2, &s, 1);
    lc_pin_set(&lc.creds, &lc.plat, 2, 2, PIN("5555"));

    /* Time unknown: fail closed. */
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("5555"), NULL), LC_ERR_SCHEDULE);
    f.wall_known = true;
    f.wall = (lc_wall_time_t){.unix_s = 1800000000, .weekday = 3, .minute_of_day = 11 * 60};
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("5555"), NULL), LC_ERR_SCHEDULE);
    CHECK_EQ(fake_last_event(&f)->type, LC_EVENT_ACCESS_DENIED);
    f.wall.minute_of_day = 12 * 60 + 30;
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("5555"), NULL), LC_OK);
    CHECK(!lc.rl.locked);
    CHECK_EQ(lc.rl.wrong_count, 0);
}

TEST(test_disposable_user_consumed)
{
    setup(cfg_attributed());
    lc_user_set(&lc.creds, 2, LC_USER_DISPOSABLE, "Courier");
    lc_pin_set(&lc.creds, &lc.plat, 2, 2, PIN("7777"));
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("7777"), NULL), LC_OK);
    CHECK(lc_user_get(&lc.creds, 2) == NULL);
    CHECK_EQ(f.persist_counts[LC_PERSIST_CREDS], 1);
    lc_on_motor_done(&lc, true);
    lc_request(&lc, LC_OP_LOCK, LC_SOURCE_KEYPAD, NULL, 0, NULL);
    lc_on_motor_done(&lc, true);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("7777"), NULL), LC_ERR_DENIED);
}

TEST(test_disposable_not_consumed_when_door_open)
{
    setup(cfg_attributed());
    lc_user_set(&lc.creds, 2, LC_USER_DISPOSABLE, NULL);
    lc_pin_set(&lc.creds, &lc.plat, 2, 2, PIN("7777"));
    lc.bolt = LC_BOLT_UNLOCKED;
    lc_on_door(&lc, LC_DOOR_OPEN);
    CHECK_EQ(lc_request(&lc, LC_OP_LOCK, LC_SOURCE_KEYPAD, PIN("7777"), NULL), LC_ERR_DOOR_OPEN);
    CHECK(lc_user_get(&lc.creds, 2) != NULL);
}

TEST(test_private_events_strip_user)
{
    setup(lc_config_default()); /* private_events on by default */
    lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL);
    lc_on_motor_done(&lc, true);
    CHECK_EQ(fake_last_event(&f)->user_index, LC_INDEX_NONE);
    /* The local log still attributes. */
    CHECK_EQ(lc_log_get(&lc.log, lc.log.count - 1)->user_index, 1);
}

TEST(test_jam_and_motor_timeout)
{
    setup(cfg_attributed());
    lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_APP, NULL, 0, NULL);
    lc_on_motor_done(&lc, false);
    CHECK_EQ(lc.bolt, LC_BOLT_JAMMED);
    CHECK_EQ(fake_last_event(&f)->type, LC_EVENT_JAMMED);

    /* Manual turn clears the jam. */
    lc_on_bolt_position(&lc, LC_BOLT_LOCKED);
    CHECK_EQ(lc.bolt, LC_BOLT_LOCKED);

    lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_APP, NULL, 0, NULL);
    CHECK_EQ(lc_next_deadline_ms(&lc), f.now_ms + 5000);
    f.now_ms += 4999;
    lc_tick(&lc);
    CHECK_EQ(lc.bolt, LC_BOLT_UNLOCKING);
    f.now_ms += 1;
    lc_tick(&lc);
    CHECK_EQ(lc.bolt, LC_BOLT_JAMMED);
    CHECK_EQ(f.motor_stops, 1);
}

TEST(test_manual_operation_detected)
{
    setup(cfg_attributed());
    lc_on_bolt_position(&lc, LC_BOLT_UNLOCKED);
    CHECK_EQ(lc.bolt, LC_BOLT_UNLOCKED);
    CHECK_EQ(fake_last_event(&f)->type, LC_EVENT_UNLOCKED);
    CHECK_EQ(fake_last_event(&f)->source, LC_SOURCE_MANUAL);
    int n = f.notify_count;
    lc_on_bolt_position(&lc, LC_BOLT_UNLOCKED); /* no change, no event */
    CHECK_EQ(f.notify_count, n);
}

TEST(test_auto_relock)
{
    lc_config_t cfg = cfg_attributed();
    cfg.auto_relock_s = 30;
    setup(cfg);
    lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL);
    lc_on_motor_done(&lc, true);
    uint64_t unlocked_at = f.now_ms;
    CHECK_EQ(lc_next_deadline_ms(&lc), unlocked_at + 30000);

    /* Door opened: never throw the bolt into an open door. */
    f.now_ms += 10000;
    lc_on_door(&lc, LC_DOOR_OPEN);
    CHECK_EQ(lc_next_deadline_ms(&lc), UINT64_MAX);
    f.now_ms += 60000;
    lc_tick(&lc);
    CHECK_EQ(lc.bolt, LC_BOLT_UNLOCKED);

    /* Countdown restarts from door close. */
    lc_on_door(&lc, LC_DOOR_CLOSED);
    uint64_t closed_at = f.now_ms;
    f.now_ms = closed_at + 29999;
    lc_tick(&lc);
    CHECK_EQ(lc.bolt, LC_BOLT_UNLOCKED);
    f.now_ms = closed_at + 30000;
    lc_tick(&lc);
    CHECK_EQ(lc.bolt, LC_BOLT_LOCKING);
    lc_on_motor_done(&lc, true);
    CHECK_EQ(fake_last_event(&f)->source, LC_SOURCE_AUTO);
}

TEST(test_passage_mode_suspends_relock)
{
    lc_config_t cfg = cfg_attributed();
    cfg.auto_relock_s = 30;
    setup(cfg);
    lc_set_mode(&lc, LC_MODE_PASSAGE);
    lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_APP, NULL, 0, NULL);
    lc_on_motor_done(&lc, true);
    f.now_ms += 100000;
    lc_tick(&lc);
    CHECK_EQ(lc.bolt, LC_BOLT_UNLOCKED);

    lc_set_mode(&lc, LC_MODE_NORMAL);
    CHECK_EQ(lc_next_deadline_ms(&lc), f.now_ms + 30000);
}

TEST(test_already_in_state_no_motor)
{
    setup(cfg_attributed());
    CHECK_EQ(lc_request(&lc, LC_OP_LOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_OK);
    CHECK_EQ(f.motor_starts, 0);
}

TEST(test_reboot_during_lockout)
{
    setup(cfg_attributed());
    for (int i = 0; i < 5; i++) {
        lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("0000"), NULL);
    }
    lc_ratelimit_t saved = lc.rl;
    lc_creds_t saved_creds = lc.creds;

    /* Simulated battery pull: fresh lc, clock restarts, state restored. */
    fake_reset(&f);
    f.now_ms = 5;
    lc_platform_t plat = fake_platform(&f);
    lc_hooks_t hooks = fake_hooks(&f);
    lc_config_t cfg = cfg_attributed();
    lc_init(&lc, &plat, &hooks, &cfg);
    lc.rl = saved;
    lc.creds = saved_creds;
    lc_boot(&lc, LC_BOLT_LOCKED, LC_DOOR_CLOSED);

    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_ERR_LOCKED_OUT);
    f.now_ms += 60000;
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_OK);
}

TEST(test_invalid_requests)
{
    setup(cfg_attributed());
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_AUTO, NULL, 0, NULL), LC_ERR_INVALID_ARG);
    CHECK_EQ(lc_request(&lc, LC_OP_UNLOCK, LC_SOURCE_MANUAL, NULL, 0, NULL), LC_ERR_INVALID_ARG);
}
