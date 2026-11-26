/*
 * The bolt motor on an H-bridge with two logic inputs and an enable:
 * TB6612FNG (bench: AIN1, AIN2, STBY; PWMA tied high) or DRV8833 (dev kit:
 * AIN1, AIN2, nSLEEP). Both decode the inputs the same way:
 *
 *   IN1 IN2   PWM on one input sets the speed (drive / coast)
 *    H   L    forward          L  L   coast
 *    L   H    reverse          H  H   brake (outputs shorted low)
 *
 * A move runs until the bolt switch for its target position reports, then
 * brakes for CONFIG_LOCHNESS_MOTOR_BRAKE_MS and lets go (coast, driver
 * disabled). If the switch never reports, lock_core's motor timeout calls
 * motor_stop() and reports the bolt jammed.
 */
#include "sdkconfig.h"

#if CONFIG_LOCHNESS_MOTOR_HBRIDGE

#include "motor.h"

#if CONFIG_LOCHNESS_BOLT_LOCKED_GPIO < 0 || CONFIG_LOCHNESS_BOLT_UNLOCKED_GPIO < 0
#error "The H-bridge motor driver stops on the bolt switches: set both bolt switch GPIOs (menuconfig)"
#endif

#include <stdint.h>

#include "bolt_sense.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lock_core/lc_types.h"
#include "lock_task.h"
#include "soc/clk_tree_defs.h"

#define IN1_PIN CONFIG_LOCHNESS_MOTOR_IN1_GPIO
#define IN2_PIN CONFIG_LOCHNESS_MOTOR_IN2_GPIO
#define EN_PIN CONFIG_LOCHNESS_MOTOR_EN_GPIO
#if CONFIG_LOCHNESS_MOTOR_REVERSE
#define LOCK_ON_IN1 false
#else
#define LOCK_ON_IN1 true
#endif

enum {
    PWM_HZ = 20000, /* above hearing; the TB6612 takes up to 100 kHz, the DRV8833 50 kHz */
    PWM_BITS = 10,
    PWM_FULL = 1 << PWM_BITS, /* LEDC: a duty of 2^bits holds the output high */
    PWM_DUTY = PWM_FULL * CONFIG_LOCHNESS_MOTOR_DUTY_PCT / 100,
};

#define PWM_MODE LEDC_LOW_SPEED_MODE
#define PWM_TIMER LEDC_TIMER_0
#define CH_IN1 LEDC_CHANNEL_0
#define CH_IN2 LEDC_CHANNEL_1

static const char *const TAG = "motor";

/* `moving` and `target` are shared by the lock task (start, stop), the timer
 * task (bolt switches) and the esp_timer task (release): all under `lock`. */
static struct {
    SemaphoreHandle_t lock;
    esp_timer_handle_t release;
    bool moving;
    bolt_sense_t target;
    int64_t started_us;
} s_motor; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): module state

static void set_inputs(uint32_t in1, uint32_t in2)
{
    ESP_ERROR_CHECK(ledc_set_duty(PWM_MODE, CH_IN1, in1));
    ESP_ERROR_CHECK(ledc_update_duty(PWM_MODE, CH_IN1));
    ESP_ERROR_CHECK(ledc_set_duty(PWM_MODE, CH_IN2, in2));
    ESP_ERROR_CHECK(ledc_update_duty(PWM_MODE, CH_IN2));
}

static void set_enable(bool enable)
{
#if EN_PIN >= 0
    gpio_set_level((gpio_num_t)EN_PIN, enable ? 1 : 0);
#else
    (void)enable;
#endif
}

/* Callers hold s_motor.lock. */
static void drive(lc_op_t op)
{
    bool on_in1 = (op == LC_OP_LOCK) == LOCK_ON_IN1;
    set_enable(true);
    set_inputs(on_in1 ? PWM_DUTY : 0, on_in1 ? 0 : PWM_DUTY);
}

static void brake_then_release(void)
{
    set_inputs(PWM_FULL, PWM_FULL);
    esp_timer_stop(s_motor.release);
    ESP_ERROR_CHECK(esp_timer_start_once(s_motor.release, (uint64_t)CONFIG_LOCHNESS_MOTOR_BRAKE_MS * 1000));
}

static void on_release(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_motor.lock, portMAX_DELAY);
    if (!s_motor.moving) { /* a new move may have started since the brake */
        set_inputs(0, 0);
        set_enable(false);
    }
    xSemaphoreGive(s_motor.lock);
}

static uint32_t elapsed_ms(void) { return (uint32_t)((esp_timer_get_time() - s_motor.started_us) / 1000); }

void motor_init(void)
{
    s_motor.lock = xSemaphoreCreateMutex();
    configASSERT(s_motor.lock);
    const esp_timer_create_args_t args = {.callback = on_release, .name = "motor_brake"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_motor.release));

#if EN_PIN >= 0
    const gpio_config_t en_cfg = {.pin_bit_mask = 1ULL << EN_PIN, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)EN_PIN, 0)); /* standby before it becomes an output */
    ESP_ERROR_CHECK(gpio_config(&en_cfg));
#endif
    const ledc_timer_config_t timer = {
        .speed_mode = PWM_MODE,
        .duty_resolution = PWM_BITS,
        .timer_num = PWM_TIMER,
        .freq_hz = PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const int pins[] = {IN1_PIN, IN2_PIN};
    const ledc_channel_t channels[] = {CH_IN1, CH_IN2};
    for (int i = 0; i < 2; i++) {
        const ledc_channel_config_t chan = {
            .gpio_num = pins[i],
            .speed_mode = PWM_MODE,
            .channel = channels[i],
            .timer_sel = PWM_TIMER,
            .duty = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&chan));
    }
    ESP_LOGI(TAG, "H-bridge on GPIO %d/%d, enable %d, %d%% duty%s", IN1_PIN, IN2_PIN, EN_PIN,
             CONFIG_LOCHNESS_MOTOR_DUTY_PCT, LOCK_ON_IN1 ? "" : ", reversed");
}

void motor_start(void *ctx, lc_op_t op)
{
    (void)ctx;
    const char *verb = op == LC_OP_LOCK ? "locking" : "unlocking";
    bolt_sense_t target = op == LC_OP_LOCK ? BOLT_SENSE_LOCKED : BOLT_SENSE_UNLOCKED;
    xSemaphoreTake(s_motor.lock, portMAX_DELAY);
    if (bolt_sense_read() == target) { /* e.g. lock_core didn't know the position */
        xSemaphoreGive(s_motor.lock);
        ESP_LOGI(TAG, "%s: bolt already there", verb);
        lock_task_post_motor_done(true);
        return;
    }
    esp_timer_stop(s_motor.release);
    s_motor.moving = true;
    s_motor.target = target;
    s_motor.started_us = esp_timer_get_time();
    drive(op);
    xSemaphoreGive(s_motor.lock);
    ESP_LOGI(TAG, "%s", verb);
}

void motor_stop(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_motor.lock, portMAX_DELAY);
    bool was_moving = s_motor.moving;
    s_motor.moving = false;
    brake_then_release();
    xSemaphoreGive(s_motor.lock);
    if (was_moving) {
        ESP_LOGW(TAG, "stopped after %lu ms without reaching the %s switch", (unsigned long)elapsed_ms(),
                 bolt_sense_name(s_motor.target));
    }
}

void motor_on_bolt(bolt_sense_t pos)
{
    xSemaphoreTake(s_motor.lock, portMAX_DELAY);
    if (!s_motor.moving || (pos != s_motor.target && pos != BOLT_SENSE_FAULT)) {
        xSemaphoreGive(s_motor.lock);
        return;
    }
    s_motor.moving = false;
    brake_then_release();
    uint32_t ms = elapsed_ms();
    xSemaphoreGive(s_motor.lock);
    if (pos == BOLT_SENSE_FAULT) {
        ESP_LOGE(TAG, "both bolt switches active: stopped after %lu ms", (unsigned long)ms);
        lock_task_post_motor_done(false);
        return;
    }
    ESP_LOGI(TAG, "reached %s in %lu ms", bolt_sense_name(pos), (unsigned long)ms);
    lock_task_post_motor_done(true);
}

bool motor_fail_next(void) { return false; }

const char *motor_driver_name(void) { return "H-bridge"; }

#endif
