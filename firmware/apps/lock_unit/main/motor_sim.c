#include "motor_sim.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lock_task.h"
#include "sdkconfig.h"

static const char *TAG = "motor";
static esp_timer_handle_t s_timer;
static volatile bool s_fail_next;

static void on_done(void *arg)
{
    (void)arg;
    bool ok = !s_fail_next;
    s_fail_next = false;
    lock_task_post_motor_done(ok);
}

void motor_sim_init(void)
{
    const esp_timer_create_args_t args = {.callback = on_done, .name = "motor_sim"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
}

void motor_sim_start(void *ctx, lc_op_t op)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s (simulated, %d ms)", op == LC_OP_LOCK ? "locking" : "unlocking",
             CONFIG_LOCHNESS_SIM_MOTOR_MS);
    esp_timer_stop(s_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_timer, (uint64_t)CONFIG_LOCHNESS_SIM_MOTOR_MS * 1000));
}

void motor_sim_stop(void *ctx)
{
    (void)ctx;
    esp_timer_stop(s_timer);
    ESP_LOGW(TAG, "stopped");
}

void motor_sim_fail_next(void) { s_fail_next = true; }
