#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

static int g_failures; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): harness tally

void test_check(bool ok, const char *file, int line, const char *expr)
{
    if (!ok) {
        (void)fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, expr);
        g_failures++;
    }
}

void test_check_eq(long long actual, long long expected, const char *file, int line,
                   const char *actual_expr, const char *expected_expr)
{
    if (actual != expected) {
        (void)fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld != %lld)\n", file, line, actual_expr,
                      expected_expr, actual, expected);
        g_failures++;
    }
}

int test_failure_count(void) { return g_failures; }

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
    X(test_invalid_requests)                                                                       \
    X(test_sha256_vectors)                                                                         \
    X(test_hmac_rfc4231)                                                                           \
    X(test_dh_rejects_low_order)                                                                   \
    X(test_noise_ik_vector)                                                                        \
    X(test_noise_xx_vector)                                                                        \
    X(test_ik_wrong_responder_key_fails)                                                           \
    X(test_prologue_mismatch_fails)                                                                \
    X(test_session_rejects_replay_and_tamper)                                                      \
    X(test_handshake_out_of_turn)                                                                  \
    X(test_handshake_truncated_message) \
    X(test_lp_roundtrip) \
    X(test_lp_rejects_malformed) \
    X(test_lp_fixed_sizes) \
    X(test_link_pairing_requires_window) \
    X(test_link_pair_then_unlock) \
    X(test_link_wrong_pin_and_lockout) \
    X(test_link_one_touch_lock) \
    X(test_link_unpaired_keypad_rejected) \
    X(test_link_removed_peer_rejected) \
    X(test_link_replay_rejected) \
    X(test_link_ciphertext_length_hides_pin) \
    X(test_link_wrong_role_window) \
    X(test_link_pairing_window_expires) \
    X(test_link_lock_unreachable) \
    X(test_link_entry_timeout_clears_digits) \
    X(test_link_lock_idle_timeout) \
    X(test_link_too_long_pin_rejected_locally) \
    X(test_link_low_battery_status_sent) \
    X(test_link_resend_after_link_drop) \
    X(test_link_resend_only_once) \
    X(test_lp_time_and_state_roundtrip) \
    X(test_phone_gets_snapshot_on_connect) \
    X(test_keypad_gets_no_state) \
    X(test_phone_sets_time_keypad_cannot) \
    X(test_phone_receives_events) \
    X(test_log_entries_carry_boot)

TESTS
#undef X

int main(void)
{
#define X(name) {#name, name},
    static const test_case_t CASES[] = {TESTS};
#undef X
    int failed_cases = 0;
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        int before = test_failure_count();
        CASES[i].fn();
        bool ok = test_failure_count() == before;
        printf("%s %s\n", ok ? "PASS" : "FAIL", CASES[i].name);
        failed_cases += !ok;
    }
    printf("\n%d/%zu passed\n", (int)(sizeof(CASES) / sizeof(CASES[0])) - failed_cases,
           sizeof(CASES) / sizeof(CASES[0]));
    return failed_cases ? 1 : 0;
}
