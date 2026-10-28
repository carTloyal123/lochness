#include <stdint.h>
#include <string.h>

#include "fake_platform.h"
#include "lock_core/lc_creds.h"
#include "lock_core/lc_lock.h"
#include "lock_core/lc_log.h"
#include "lock_core/lc_types.h"
#include "test.h"

#define PIN(s) (const uint8_t *)(s), strlen(s)

/* lc_config_default(): 5 wrong codes start a 60 s lockout; motor times out after 5 s. */
enum {
    WRONG_CODE_LIMIT = 5,
    LOCKOUT_MS = 60 * 1000,
    MOTOR_TIMEOUT_MS = 5000,
    RELOCK_S = 30,
    RELOCK_MS = RELOCK_S * 1000,
};

static struct {
    fake_t fake;
    lc_t lc;
} fixture; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): test fixture state

/* Locked, door closed, Alice (user 1) has PIN 1234. */
static void setup(lc_config_t cfg)
{
    fake_reset(&fixture.fake);
    lc_platform_t plat = fake_platform(&fixture.fake);
    lc_hooks_t hooks = fake_hooks(&fixture.fake);
    lc_init(&fixture.lc, &plat, &hooks, &cfg);
    lc_user_set(&fixture.lc.creds, 1, LC_USER_UNRESTRICTED, "Alice");
    lc_pin_set(&fixture.lc.creds, &fixture.lc.plat, 1, 1, PIN("1234"));
    lc_boot(&fixture.lc, LC_BOLT_LOCKED, LC_DOOR_CLOSED);
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
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), &user), LC_OK);
    CHECK_EQ(user, 1);
    CHECK_EQ(fixture.lc.bolt, LC_BOLT_UNLOCKING);
    CHECK_EQ(fixture.fake.motor_starts, 1);
    CHECK_EQ(fixture.fake.last_motor_op, LC_OP_UNLOCK);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_LOCK, LC_SOURCE_KEYPAD, NULL, 0, NULL), LC_ERR_BUSY);

    lc_on_motor_done(&fixture.lc, true);
    CHECK_EQ(fixture.lc.bolt, LC_BOLT_UNLOCKED);
    const lc_event_t *ev = fake_last_event(&fixture.fake);
    CHECK(ev != NULL);
    if (ev == NULL) {
        return;
    }
    CHECK_EQ(ev->type, LC_EVENT_UNLOCKED);
    CHECK_EQ(ev->source, LC_SOURCE_KEYPAD);
    CHECK_EQ(ev->user_index, 1);
    CHECK_EQ(fixture.lc.log.count, 1);
}

TEST(test_keypad_unlock_requires_pin)
{
    setup(cfg_attributed());
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, NULL, 0, NULL), LC_ERR_DENIED);
    CHECK_EQ(fixture.fake.motor_starts, 0);
}

TEST(test_one_touch_lock)
{
    lc_config_t cfg = cfg_attributed();
    setup(cfg);
    fixture.lc.bolt = LC_BOLT_UNLOCKED;
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_LOCK, LC_SOURCE_KEYPAD, NULL, 0, NULL), LC_OK);
    CHECK_EQ(fixture.fake.motor_starts, 1);

    cfg.one_touch_lock = false;
    setup(cfg);
    fixture.lc.bolt = LC_BOLT_UNLOCKED;
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_LOCK, LC_SOURCE_KEYPAD, NULL, 0, NULL), LC_ERR_DENIED);
}

TEST(test_wrong_codes_lock_out_and_persist)
{
    setup(cfg_attributed()); /* limit 5, 60 s */
    for (int i = 0; i < WRONG_CODE_LIMIT - 1; i++) {
        CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("0000"), NULL), LC_ERR_DENIED);
    }
    CHECK_EQ(fixture.fake.persist_counts[LC_PERSIST_RATELIMIT], WRONG_CODE_LIMIT - 1);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("0000"), NULL), LC_ERR_DENIED);
    CHECK_EQ(fake_last_event(&fixture.fake)->type, LC_EVENT_LOCKOUT_STARTED);

    /* Correct PIN is not even evaluated while locked out. */
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_ERR_LOCKED_OUT);
    CHECK_EQ(fixture.fake.motor_starts, 0);

    fixture.fake.now_ms += LOCKOUT_MS;
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_OK);
    CHECK_EQ(fixture.lc.rl.lockout_level, 0);
}

TEST(test_lockout_applies_to_remote_pin)
{
    lc_config_t cfg = cfg_attributed();
    cfg.require_pin_for_remote = true;
    setup(cfg);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_ERR_DENIED);
    for (int i = 0; i < WRONG_CODE_LIMIT; i++) {
        lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, PIN("9999"), NULL);
    }
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_ERR_LOCKED_OUT);
}

TEST(test_remote_unlock_cap)
{
    lc_config_t cfg = cfg_attributed();
    cfg.remote_unlock_allowed = false;
    setup(cfg);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_ERR_MODE);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, PIN("1234"), NULL), LC_ERR_MODE);
    CHECK_EQ(fixture.fake.motor_starts, 0);
    /* Remote lock is still fine. */
    fixture.lc.bolt = LC_BOLT_UNLOCKED;
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_LOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_OK);
}

TEST(test_operating_modes)
{
    setup(cfg_attributed());
    lc_set_mode(&fixture.lc, LC_MODE_VACATION);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_ERR_MODE);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_OK);

    setup(cfg_attributed());
    lc_set_mode(&fixture.lc, LC_MODE_PRIVACY);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_ERR_MODE);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_ERR_MODE);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_APP, NULL, 0, NULL), LC_OK);

    setup(cfg_attributed());
    lc_set_mode(&fixture.lc, LC_MODE_NO_REMOTE);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_ERR_MODE);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_OK);
}

TEST(test_mode_denial_does_not_count_as_wrong_code)
{
    setup(cfg_attributed());
    lc_set_mode(&fixture.lc, LC_MODE_VACATION);
    for (int i = 0; i < 2 * WRONG_CODE_LIMIT; i++) {
        lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("0000"), NULL);
    }
    CHECK(!fixture.lc.rl.locked);
}

TEST(test_schedule_user)
{
    setup(cfg_attributed());
    lc_user_set(&fixture.lc.creds, 2, LC_USER_WEEKDAY_SCHEDULE, "Walker");
    enum {
        MON_TO_FRI = 0x3E, /* bits 1-5 */
        WEDNESDAY = 3,
        AT_11AM = 11 * 60,
        AT_NOON = 12 * 60,
        AT_1230PM = AT_NOON + 30,
        AT_1PM = 13 * 60,
        SOME_UNIX_TIME_S = 1800000000,
    };
    lc_weekday_schedule_t s = {.days_mask = MON_TO_FRI, .start_min = AT_NOON, .end_min = AT_1PM};
    lc_user_set_schedules(&fixture.lc.creds, 2, &s, 1);
    lc_pin_set(&fixture.lc.creds, &fixture.lc.plat, 2, 2, PIN("5555"));

    /* Time unknown: fail closed. */
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("5555"), NULL), LC_ERR_SCHEDULE);
    fixture.fake.wall_known = true;
    fixture.fake.wall = (lc_wall_time_t){.unix_s = SOME_UNIX_TIME_S, .weekday = WEDNESDAY, .minute_of_day = AT_11AM};
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("5555"), NULL), LC_ERR_SCHEDULE);
    CHECK_EQ(fake_last_event(&fixture.fake)->type, LC_EVENT_ACCESS_DENIED);
    fixture.fake.wall.minute_of_day = AT_1230PM;
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("5555"), NULL), LC_OK);
    CHECK(!fixture.lc.rl.locked);
    CHECK_EQ(fixture.lc.rl.wrong_count, 0);
}

TEST(test_disposable_user_consumed)
{
    setup(cfg_attributed());
    lc_user_set(&fixture.lc.creds, 2, LC_USER_DISPOSABLE, "Courier");
    lc_pin_set(&fixture.lc.creds, &fixture.lc.plat, 2, 2, PIN("7777"));
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("7777"), NULL), LC_OK);
    CHECK(lc_user_get(&fixture.lc.creds, 2) == NULL);
    CHECK_EQ(fixture.fake.persist_counts[LC_PERSIST_CREDS], 1);
    lc_on_motor_done(&fixture.lc, true);
    lc_request(&fixture.lc, LC_OP_LOCK, LC_SOURCE_KEYPAD, NULL, 0, NULL);
    lc_on_motor_done(&fixture.lc, true);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("7777"), NULL), LC_ERR_DENIED);
}

TEST(test_disposable_not_consumed_when_door_open)
{
    setup(cfg_attributed());
    lc_user_set(&fixture.lc.creds, 2, LC_USER_DISPOSABLE, NULL);
    lc_pin_set(&fixture.lc.creds, &fixture.lc.plat, 2, 2, PIN("7777"));
    fixture.lc.bolt = LC_BOLT_UNLOCKED;
    lc_on_door(&fixture.lc, LC_DOOR_OPEN);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_LOCK, LC_SOURCE_KEYPAD, PIN("7777"), NULL), LC_ERR_DOOR_OPEN);
    CHECK(lc_user_get(&fixture.lc.creds, 2) != NULL);
}

TEST(test_private_events_strip_user)
{
    setup(lc_config_default()); /* private_events on by default */
    lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL);
    lc_on_motor_done(&fixture.lc, true);
    CHECK_EQ(fake_last_event(&fixture.fake)->user_index, LC_INDEX_NONE);
    /* The local log still attributes. */
    CHECK_EQ(lc_log_get(&fixture.lc.log, fixture.lc.log.count - 1)->user_index, 1);
}

TEST(test_jam_and_motor_timeout)
{
    setup(cfg_attributed());
    lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_APP, NULL, 0, NULL);
    lc_on_motor_done(&fixture.lc, false);
    CHECK_EQ(fixture.lc.bolt, LC_BOLT_JAMMED);
    CHECK_EQ(fake_last_event(&fixture.fake)->type, LC_EVENT_JAMMED);

    /* Manual turn clears the jam. */
    lc_on_bolt_position(&fixture.lc, LC_BOLT_LOCKED);
    CHECK_EQ(fixture.lc.bolt, LC_BOLT_LOCKED);

    lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_APP, NULL, 0, NULL);
    CHECK_EQ(lc_next_deadline_ms(&fixture.lc), fixture.fake.now_ms + MOTOR_TIMEOUT_MS);
    fixture.fake.now_ms += MOTOR_TIMEOUT_MS - 1;
    lc_tick(&fixture.lc);
    CHECK_EQ(fixture.lc.bolt, LC_BOLT_UNLOCKING);
    fixture.fake.now_ms += 1;
    lc_tick(&fixture.lc);
    CHECK_EQ(fixture.lc.bolt, LC_BOLT_JAMMED);
    CHECK_EQ(fixture.fake.motor_stops, 1);
}

TEST(test_manual_operation_detected)
{
    setup(cfg_attributed());
    lc_on_bolt_position(&fixture.lc, LC_BOLT_UNLOCKED);
    CHECK_EQ(fixture.lc.bolt, LC_BOLT_UNLOCKED);
    CHECK_EQ(fake_last_event(&fixture.fake)->type, LC_EVENT_UNLOCKED);
    CHECK_EQ(fake_last_event(&fixture.fake)->source, LC_SOURCE_MANUAL);
    int n = fixture.fake.notify_count;
    lc_on_bolt_position(&fixture.lc, LC_BOLT_UNLOCKED); /* no change, no event */
    CHECK_EQ(fixture.fake.notify_count, n);
}

TEST(test_auto_relock)
{
    lc_config_t cfg = cfg_attributed();
    cfg.auto_relock_s = RELOCK_S;
    setup(cfg);
    lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL);
    lc_on_motor_done(&fixture.lc, true);
    uint64_t unlocked_at = fixture.fake.now_ms;
    CHECK_EQ(lc_next_deadline_ms(&fixture.lc), unlocked_at + RELOCK_MS);

    /* Door opened: never throw the bolt into an open door. */
    enum { OPENED_AFTER_MS = RELOCK_MS / 3, HELD_OPEN_MS = 2 * RELOCK_MS };
    fixture.fake.now_ms += OPENED_AFTER_MS;
    lc_on_door(&fixture.lc, LC_DOOR_OPEN);
    CHECK_EQ(lc_next_deadline_ms(&fixture.lc), UINT64_MAX);
    fixture.fake.now_ms += HELD_OPEN_MS;
    lc_tick(&fixture.lc);
    CHECK_EQ(fixture.lc.bolt, LC_BOLT_UNLOCKED);

    /* Countdown restarts from door close. */
    lc_on_door(&fixture.lc, LC_DOOR_CLOSED);
    uint64_t closed_at = fixture.fake.now_ms;
    fixture.fake.now_ms = closed_at + RELOCK_MS - 1;
    lc_tick(&fixture.lc);
    CHECK_EQ(fixture.lc.bolt, LC_BOLT_UNLOCKED);
    fixture.fake.now_ms = closed_at + RELOCK_MS;
    lc_tick(&fixture.lc);
    CHECK_EQ(fixture.lc.bolt, LC_BOLT_LOCKING);
    lc_on_motor_done(&fixture.lc, true);
    CHECK_EQ(fake_last_event(&fixture.fake)->source, LC_SOURCE_AUTO);
}

TEST(test_passage_mode_suspends_relock)
{
    lc_config_t cfg = cfg_attributed();
    cfg.auto_relock_s = RELOCK_S;
    setup(cfg);
    lc_set_mode(&fixture.lc, LC_MODE_PASSAGE);
    lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_APP, NULL, 0, NULL);
    lc_on_motor_done(&fixture.lc, true);
    fixture.fake.now_ms += (uint64_t)RELOCK_MS * 3U; /* well past the relock time */
    lc_tick(&fixture.lc);
    CHECK_EQ(fixture.lc.bolt, LC_BOLT_UNLOCKED);

    lc_set_mode(&fixture.lc, LC_MODE_NORMAL);
    CHECK_EQ(lc_next_deadline_ms(&fixture.lc), fixture.fake.now_ms + RELOCK_MS);
}

TEST(test_already_in_state_no_motor)
{
    setup(cfg_attributed());
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_LOCK, LC_SOURCE_REMOTE, NULL, 0, NULL), LC_OK);
    CHECK_EQ(fixture.fake.motor_starts, 0);
}

TEST(test_reboot_during_lockout)
{
    setup(cfg_attributed());
    for (int i = 0; i < WRONG_CODE_LIMIT; i++) {
        lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("0000"), NULL);
    }
    lc_ratelimit_t saved = fixture.lc.rl;
    lc_creds_t saved_creds = fixture.lc.creds;

    /* Simulated battery pull: fresh fixture.lc, clock restarts, state restored. */
    fake_reset(&fixture.fake);
    enum { BOOT_CLOCK_MS = 5 };
    fixture.fake.now_ms = BOOT_CLOCK_MS;
    lc_platform_t plat = fake_platform(&fixture.fake);
    lc_hooks_t hooks = fake_hooks(&fixture.fake);
    lc_config_t cfg = cfg_attributed();
    lc_init(&fixture.lc, &plat, &hooks, &cfg);
    fixture.lc.rl = saved;
    fixture.lc.creds = saved_creds;
    lc_boot(&fixture.lc, LC_BOLT_LOCKED, LC_DOOR_CLOSED);

    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_ERR_LOCKED_OUT);
    fixture.fake.now_ms += LOCKOUT_MS;
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_KEYPAD, PIN("1234"), NULL), LC_OK);
}

TEST(test_invalid_requests)
{
    setup(cfg_attributed());
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_AUTO, NULL, 0, NULL), LC_ERR_INVALID_ARG);
    CHECK_EQ(lc_request(&fixture.lc, LC_OP_UNLOCK, LC_SOURCE_MANUAL, NULL, 0, NULL), LC_ERR_INVALID_ARG);
}
