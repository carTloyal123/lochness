#include "feedback.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "keypad_core/kp.h"
#include "sdkconfig.h"
#include <stddef.h>
#include <stdint.h>

static const char *const TAG = "feedback";

enum {
    BUZZER_HZ = 2700,
    BUZZER_DUTY_HALF = 512, /* 50% of the 10-bit duty range */
    QUEUE_LEN = 4,
    TASK_STACK = 2560,
    TASK_PRIO = 4,
};

typedef enum { OFF, GREEN, RED, BOTH } color_t;

typedef struct {
    color_t color;
    uint16_t on_ms;
    uint16_t off_ms;
    uint8_t repeats;
    bool beep;
} pattern_t;

/* Indexed by kp_feedback_t. Missing entries are all-zero: no light, no sound. */
static const pattern_t PATTERNS[] = {
    [KP_FB_KEY] = {.color = BOTH, .on_ms = 30, .repeats = 1, .beep = true},
    [KP_FB_CLEARED] = {.color = OFF},
    [KP_FB_UNLOCKED] = {.color = GREEN, .on_ms = 1500, .repeats = 1, .beep = true},
    [KP_FB_LOCKED] = {.color = GREEN, .on_ms = 300, .off_ms = 200, .repeats = 2, .beep = true},
    [KP_FB_DENIED] = {.color = RED, .on_ms = 600, .repeats = 1, .beep = true},
    [KP_FB_LOCKED_OUT] = {.color = RED, .on_ms = 150, .off_ms = 150, .repeats = 6, .beep = true},
    [KP_FB_REFUSED] = {.color = RED, .on_ms = 300, .off_ms = 200, .repeats = 2, .beep = true},
    [KP_FB_NO_LINK] = {.color = RED, .on_ms = 100, .off_ms = 100, .repeats = 3},
    [KP_FB_NOT_PAIRED] = {.color = BOTH, .on_ms = 100, .off_ms = 100, .repeats = 3},
    [KP_FB_PAIRING] = {.color = BOTH, .on_ms = 200, .off_ms = 200, .repeats = 3},
    [KP_FB_PAIRED] = {.color = GREEN, .on_ms = 100, .off_ms = 100, .repeats = 5, .beep = true},
    [KP_FB_PAIR_FAILED] = {.color = RED, .on_ms = 100, .off_ms = 100, .repeats = 5, .beep = true},
};

typedef struct {
    kp_feedback_t fb;
    uint16_t detail;
} item_t;

static QueueHandle_t s_queue; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): set once in feedback_start

static const char *fb_name(kp_feedback_t feedback)
{
    switch (feedback) {
    case KP_FB_KEY: return "key";
    case KP_FB_CLEARED: return "cleared";
    case KP_FB_UNLOCKED: return "UNLOCKED";
    case KP_FB_LOCKED: return "LOCKED";
    case KP_FB_DENIED: return "denied";
    case KP_FB_LOCKED_OUT: return "locked out";
    case KP_FB_REFUSED: return "refused (schedule/mode/door)";
    case KP_FB_NO_LINK: return "lock unreachable";
    case KP_FB_NOT_PAIRED: return "not paired";
    case KP_FB_PAIRING: return "pairing...";
    case KP_FB_PAIRED: return "PAIRED";
    case KP_FB_PAIR_FAILED: return "pairing failed";
    }
    return "?";
}

static pattern_t pattern_for(kp_feedback_t feedback)
{
    if ((size_t)feedback >= sizeof(PATTERNS) / sizeof(PATTERNS[0])) {
        return (pattern_t){0};
    }
    return PATTERNS[feedback];
}

static void set_leds(color_t color)
{
#if CONFIG_KEYPAD_LED_GREEN_GPIO >= 0
    gpio_set_level(CONFIG_KEYPAD_LED_GREEN_GPIO, color == GREEN || color == BOTH);
#endif
#if CONFIG_KEYPAD_LED_RED_GPIO >= 0
    gpio_set_level(CONFIG_KEYPAD_LED_RED_GPIO, color == RED || color == BOTH);
#endif
    (void)color;
}

static void set_buzzer(bool sounding)
{
#if CONFIG_KEYPAD_BUZZER_GPIO >= 0
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, sounding ? BUZZER_DUTY_HALF : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#endif
    (void)sounding;
}

/* Sleep for ms, returning early (true) as soon as newer feedback is queued:
 * the newest feedback always wins, and a long pattern must not delay it. */
static bool wait_or_newer(uint16_t ms)
{
    item_t next;
    return xQueuePeek(s_queue, &next, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static void feedback_task(void *arg)
{
    (void)arg;
    item_t item;
    for (;;) {
        xQueueReceive(s_queue, &item, portMAX_DELAY);
        if (item.fb == KP_FB_LOCKED_OUT) {
            ESP_LOGI(TAG, "%s (%u s)", fb_name(item.fb), item.detail);
        } else if (item.fb != KP_FB_KEY) {
            ESP_LOGI(TAG, "%s", fb_name(item.fb));
        }
        pattern_t pat = pattern_for(item.fb);
        bool preempted = false;
        for (int i = 0; i < pat.repeats && !preempted; i++) {
            set_leds(pat.color);
            set_buzzer(pat.beep && (i == 0 || item.fb == KP_FB_LOCKED_OUT));
            preempted = wait_or_newer(pat.on_ms);
            set_leds(OFF);
            set_buzzer(false);
            if (!preempted && pat.off_ms) {
                preempted = wait_or_newer(pat.off_ms);
            }
        }
    }
}

void feedback_start(void)
{
#if CONFIG_KEYPAD_LED_GREEN_GPIO >= 0 || CONFIG_KEYPAD_LED_RED_GPIO >= 0
    uint64_t mask = 0;
#if CONFIG_KEYPAD_LED_GREEN_GPIO >= 0
    mask |= 1ULL << CONFIG_KEYPAD_LED_GREEN_GPIO;
#endif
#if CONFIG_KEYPAD_LED_RED_GPIO >= 0
    mask |= 1ULL << CONFIG_KEYPAD_LED_RED_GPIO;
#endif
    gpio_config_t cfg = {.pin_bit_mask = mask, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&cfg));
#endif
#if CONFIG_KEYPAD_BUZZER_GPIO >= 0
    ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                                 .timer_num = LEDC_TIMER_0, .freq_hz = BUZZER_HZ, .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel = {.gpio_num = CONFIG_KEYPAD_BUZZER_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
                                     .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0};
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
#endif
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(item_t));
    configASSERT(s_queue);
    xTaskCreate(feedback_task, "feedback", TASK_STACK, NULL, TASK_PRIO, NULL);
}

void feedback_show(kp_feedback_t feedback, uint16_t detail)
{
    item_t item = {feedback, detail};
    if (xQueueSend(s_queue, &item, 0) == pdTRUE || feedback == KP_FB_KEY) {
        return; /* a dropped key click is fine */
    }
    /* Queue full of clicks or stale results: a result must never be lost,
     * so drop the backlog and show this one. */
    xQueueReset(s_queue);
    xQueueSend(s_queue, &item, 0);
}
