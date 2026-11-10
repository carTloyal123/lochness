#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "link_proto/lp.h"
#include "sdkconfig.h"

#if CONFIG_LOCHNESS_STATUS_LED_GPIO >= 0

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "led_strip_types.h"

static const char *const TAG = "led";

enum {
    TICK_US = 100000, /* blink phase: 100 ms */
    RMT_RESOLUTION_HZ = 10000000,
    LEVEL = CONFIG_LOCHNESS_STATUS_LED_BRIGHTNESS,
    PAIR_BLINK_TICKS = 5, /* blue for 500 ms of every second */
};

typedef struct {
    uint8_t r, g, b;
} rgb_t;

/* `bolt` and `pairing` are written by the lock task, everything else only
 * by the timer callback. */
static struct {
    led_strip_handle_t strip;
    esp_timer_handle_t timer;
    volatile uint8_t bolt;
    volatile bool pairing;
    uint32_t tick;
    rgb_t shown;
    bool shown_valid;
} s_led; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): module state

static rgb_t bolt_color(uint8_t bolt, uint32_t tick)
{
    switch (bolt) {
    case LP_BOLT_LOCKED: return (rgb_t){LEVEL, 0, 0};
    case LP_BOLT_UNLOCKED: return (rgb_t){0, LEVEL, 0};
    case LP_BOLT_MOVING: return (rgb_t){LEVEL, LEVEL / 3, 0};
    case LP_BOLT_JAMMED: return (tick % 2U) ? (rgb_t){LEVEL, 0, 0} : (rgb_t){0, 0, 0};
    default: return (rgb_t){LEVEL / 3, LEVEL / 3, LEVEL / 3};
    }
}

static void on_tick(void *arg)
{
    (void)arg;
    s_led.tick++;
    rgb_t c = bolt_color(s_led.bolt, s_led.tick);
    if (s_led.pairing && (s_led.tick / PAIR_BLINK_TICKS) % 2U) {
        c = (rgb_t){0, 0, LEVEL};
    }
    if (s_led.shown_valid && c.r == s_led.shown.r && c.g == s_led.shown.g && c.b == s_led.shown.b) {
        return;
    }
    if (led_strip_set_pixel(s_led.strip, 0, c.r, c.g, c.b) == ESP_OK && led_strip_refresh(s_led.strip) == ESP_OK) {
        s_led.shown = c;
        s_led.shown_valid = true;
    }
}

void status_led_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = CONFIG_LOCHNESS_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {.resolution_hz = RMT_RESOLUTION_HZ};
    if (led_strip_new_rmt_device(&cfg, &rmt, &s_led.strip) != ESP_OK) {
        ESP_LOGW(TAG, "no LED on GPIO %d", CONFIG_LOCHNESS_STATUS_LED_GPIO);
        return;
    }
    s_led.bolt = LP_BOLT_UNKNOWN;
    const esp_timer_create_args_t args = {.callback = on_tick, .name = "status_led"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_led.timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_led.timer, TICK_US));
}

void status_led_show(lp_bolt_t bolt, bool pairing)
{
    s_led.bolt = (uint8_t)bolt;
    s_led.pairing = pairing;
}

#else

void status_led_init(void) {}
void status_led_show(lp_bolt_t bolt, bool pairing)
{
    (void)bolt;
    (void)pairing;
}

#endif
