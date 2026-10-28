#include "feedback.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "feedback";

#define BUZZER_HZ 2700

typedef enum { OFF, GREEN, RED, BOTH } color_t;

typedef struct {
    color_t color;
    uint16_t on_ms, off_ms;
    uint8_t repeats;
    bool beep;
} pattern_t;

typedef struct {
    kp_feedback_t fb;
    uint16_t detail;
} item_t;

static QueueHandle_t s_queue;

static const char *fb_name(kp_feedback_t fb)
{
    switch (fb) {
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

static pattern_t pattern_for(kp_feedback_t fb)
{
    switch (fb) {
    case KP_FB_KEY: return (pattern_t){BOTH, 30, 0, 1, true};
    case KP_FB_CLEARED: return (pattern_t){OFF, 0, 0, 0, false};
    case KP_FB_UNLOCKED: return (pattern_t){GREEN, 1500, 0, 1, true};
    case KP_FB_LOCKED: return (pattern_t){GREEN, 300, 200, 2, true};
    case KP_FB_DENIED: return (pattern_t){RED, 600, 0, 1, true};
    case KP_FB_LOCKED_OUT: return (pattern_t){RED, 150, 150, 6, true};
    case KP_FB_REFUSED: return (pattern_t){RED, 300, 200, 2, true};
    case KP_FB_NO_LINK: return (pattern_t){RED, 100, 100, 3, false};
    case KP_FB_NOT_PAIRED: return (pattern_t){BOTH, 100, 100, 3, false};
    case KP_FB_PAIRING: return (pattern_t){BOTH, 200, 200, 3, false};
    case KP_FB_PAIRED: return (pattern_t){GREEN, 100, 100, 5, true};
    case KP_FB_PAIR_FAILED: return (pattern_t){RED, 100, 100, 5, true};
    }
    return (pattern_t){0};
}

static void set_leds(color_t c)
{
#if CONFIG_KEYPAD_LED_GREEN_GPIO >= 0
    gpio_set_level(CONFIG_KEYPAD_LED_GREEN_GPIO, c == GREEN || c == BOTH);
#endif
#if CONFIG_KEYPAD_LED_RED_GPIO >= 0
    gpio_set_level(CONFIG_KEYPAD_LED_RED_GPIO, c == RED || c == BOTH);
#endif
    (void)c;
}

static void set_buzzer(bool on)
{
#if CONFIG_KEYPAD_BUZZER_GPIO >= 0
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 512 : 0); /* 50% of 10-bit */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#endif
    (void)on;
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
        pattern_t p = pattern_for(item.fb);
        for (int i = 0; i < p.repeats; i++) {
            set_leds(p.color);
            set_buzzer(p.beep && (i == 0 || item.fb == KP_FB_LOCKED_OUT));
            vTaskDelay(pdMS_TO_TICKS(p.on_ms));
            set_leds(OFF);
            set_buzzer(false);
            if (p.off_ms) {
                vTaskDelay(pdMS_TO_TICKS(p.off_ms));
            }
            if (uxQueueMessagesWaiting(s_queue)) {
                break; /* newer feedback wins */
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
    gpio_config_t io = {.pin_bit_mask = mask, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&io));
#endif
#if CONFIG_KEYPAD_BUZZER_GPIO >= 0
    ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                                 .timer_num = LEDC_TIMER_0, .freq_hz = BUZZER_HZ, .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t ch = {.gpio_num = CONFIG_KEYPAD_BUZZER_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
                                .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0};
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
#endif
    s_queue = xQueueCreate(4, sizeof(item_t));
    configASSERT(s_queue);
    xTaskCreate(feedback_task, "feedback", 2560, NULL, 4, NULL);
}

void feedback_show(kp_feedback_t fb, uint16_t detail)
{
    item_t item = {fb, detail};
    xQueueSend(s_queue, &item, 0);
}
