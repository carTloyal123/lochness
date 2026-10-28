#include "motor_sim.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lock_core/lc_types.h"
#include "lock_task.h"
#include "sdkconfig.h"
#include <stdint.h>

static const char *const TAG = "motor";

static struct {
    esp_timer_handle_t timer;
    volatile bool fail_next; /* set from the console, read in the timer task */
} s_motor; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): module state

static void on_done(void *arg)
{
    (void)arg;
    bool ok = !s_motor.fail_next;
    s_motor.fail_next = false;
    lock_task_post_motor_done(ok);
}

void motor_sim_init(void)
{
    const esp_timer_create_args_t args = {.callback = on_done, .name = "motor_sim"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_motor.timer));
}

void motor_sim_start(void *ctx, lc_op_t op)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s (simulated, %d ms)", op == LC_OP_LOCK ? "locking" : "unlocking",
             CONFIG_LOCHNESS_SIM_MOTOR_MS);
    esp_timer_stop(s_motor.timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_motor.timer, (uint64_t)CONFIG_LOCHNESS_SIM_MOTOR_MS * 1000));
}

void motor_sim_stop(void *ctx)
{
    (void)ctx;
    esp_timer_stop(s_motor.timer);
    ESP_LOGW(TAG, "stopped");
}

void motor_sim_fail_next(void) { s_motor.fail_next = true; }
