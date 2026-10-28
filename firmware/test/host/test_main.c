#include "test.h"

int test_failures = 0;

#define X(name) TEST(name);
#define TESTS                                                                                      \
    X(test_pin_validation)                                                                         \
    X(test_pin_set_and_match)                                                                      \
    X(test_pin_not_stored_in_clear)                                                                \
    X(test_same_pin_different_salt)                                                                \
    X(test_duplicate_pin_rejected)                                                                 \
    X(test_user_clear_removes_credentials)                                                         \
    X(test_name_truncated)                                                                         \
    X(test_schedule_access)                                                                        \
    X(test_disabled_and_non_access_users)                                                          \
    X(test_digest_failure_is_platform_error)                                                       \
    X(test_rl_lockout_after_limit)                                                                 \
    X(test_rl_exponential_backoff_capped)                                                          \
    X(test_rl_success_resets)                                                                      \
    X(test_rl_reboot_rearms_full_lockout)                                                          \
    X(test_rl_wrong_count_survives_reboot)                                                         \
    X(test_log_order_and_wrap)                                                                     \
    X(test_log_disabled_drops)                                                                     \
    X(test_log_prune_by_age)                                                                       \
    X(test_keypad_unlock_happy_path)                                                               \
    X(test_keypad_unlock_requires_pin)                                                             \
    X(test_one_touch_lock)                                                                         \
    X(test_wrong_codes_lock_out_and_persist)                                                       \
    X(test_lockout_applies_to_remote_pin)                                                          \
    X(test_remote_unlock_cap)                                                                      \
    X(test_operating_modes)                                                                        \
    X(test_mode_denial_does_not_count_as_wrong_code)                                               \
    X(test_schedule_user)                                                                          \
    X(test_disposable_user_consumed)                                                               \
    X(test_disposable_not_consumed_when_door_open)                                                 \
    X(test_private_events_strip_user)                                                              \
    X(test_jam_and_motor_timeout)                                                                  \
    X(test_manual_operation_detected)                                                              \
    X(test_auto_relock)                                                                            \
    X(test_passage_mode_suspends_relock)                                                           \
    X(test_already_in_state_no_motor)                                                              \
    X(test_reboot_during_lockout)                                                                  \
    X(test_invalid_requests)

TESTS
#undef X

int main(void)
{
#define X(name) {#name, name},
    static const test_case_t cases[] = {TESTS};
#undef X
    int failed_cases = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int before = test_failures;
        cases[i].fn();
        bool ok = test_failures == before;
        printf("%s %s\n", ok ? "PASS" : "FAIL", cases[i].name);
        failed_cases += !ok;
    }
    printf("\n%d/%zu passed\n", (int)(sizeof(cases) / sizeof(cases[0])) - failed_cases,
           sizeof(cases) / sizeof(cases[0]));
    return failed_cases ? 1 : 0;
}
