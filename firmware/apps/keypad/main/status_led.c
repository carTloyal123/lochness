#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#if CONFIG_KEYPAD_STATUS_LED_GPIO >= 0

#include "esp_err.h"
#include "esp_timer.h"
#include "ln_rgb.h"

enum {
    TICK_US = 100000, /* 100 ms */
    LEVEL = CONFIG_KEYPAD_STATUS_LED_BRIGHTNESS,
    PAIR_BLINK_TICKS = 5, /* blue for 500 ms of every second */
};

/* Flags written by the keypad task and main; the rest only by the timer. */
static struct {
    esp_timer_handle_t timer;
    volatile bool ready;
    volatile bool paired;
    volatile bool pairing;
    uint32_t tick;
} s_led; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): module state

static ln_rgb_t color(uint32_t tick)
{
    if (!s_led.ready) {
        return (ln_rgb_t){LEVEL / 3, LEVEL / 3, LEVEL / 3};
    }
    if (s_led.pairing) {
        return (tick / PAIR_BLINK_TICKS) % 2U ? (ln_rgb_t){0, 0, LEVEL} : (ln_rgb_t){0, 0, 0};
    }
    return s_led.paired ? (ln_rgb_t){0, LEVEL, 0} : (ln_rgb_t){LEVEL, LEVEL / 3, 0};
}

static void on_tick(void *arg)
{
    (void)arg;
    ln_rgb_set(color(++s_led.tick));
}

void status_led_init(void)
{
    if (!ln_rgb_init(CONFIG_KEYPAD_STATUS_LED_GPIO)) {
        return;
    }
    ln_rgb_set(color(0));
    const esp_timer_create_args_t args = {.callback = on_tick, .name = "status_led"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_led.timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_led.timer, TICK_US));
}

void status_led_ready(void) { s_led.ready = true; }

void status_led_show(bool paired, bool pairing)
{
    s_led.paired = paired;
    s_led.pairing = pairing;
}

#else

void status_led_init(void) {}
void status_led_ready(void) {}
void status_led_show(bool paired, bool pairing)
{
    (void)paired;
    (void)pairing;
}

#endif
