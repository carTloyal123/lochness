#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "link_proto/lp.h"
#include "sdkconfig.h"

#if CONFIG_LOCHNESS_STATUS_LED_GPIO >= 0

#include "esp_err.h"
#include "esp_timer.h"
#include "ln_rgb.h"

enum {
    TICK_US = 100000, /* blink phase: 100 ms */
    LEVEL = CONFIG_LOCHNESS_STATUS_LED_BRIGHTNESS,
    PAIR_BLINK_TICKS = 5,  /* blue for 500 ms of every second */
    READY_BLINK_TICKS = 2, /* green 200 ms on, 200 ms off */
    READY_BLINKS = 3,
};

/* `bolt`, `pairing` and `ready_ticks` are written by other tasks; the rest
 * only by the timer callback. */
static struct {
    esp_timer_handle_t timer;
    volatile uint8_t bolt;
    volatile bool pairing;
    volatile uint32_t ready_ticks; /* left in the ready flash */
    uint32_t tick;
} s_led; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): module state

static ln_rgb_t bolt_color(uint8_t bolt, uint32_t tick)
{
    switch (bolt) {
    case LP_BOLT_LOCKED: return (ln_rgb_t){LEVEL, 0, 0};
    case LP_BOLT_UNLOCKED: return (ln_rgb_t){0, LEVEL, 0};
    case LP_BOLT_MOVING: return (ln_rgb_t){LEVEL, LEVEL / 3, 0};
    case LP_BOLT_JAMMED: return (tick % 2U) ? (ln_rgb_t){LEVEL, 0, 0} : (ln_rgb_t){0, 0, 0};
    default: return (ln_rgb_t){LEVEL / 3, LEVEL / 3, LEVEL / 3};
    }
}

static void on_tick(void *arg)
{
    (void)arg;
    s_led.tick++;
    ln_rgb_t c = bolt_color(s_led.bolt, s_led.tick);
    if (s_led.ready_ticks > 0) {
        s_led.ready_ticks--;
        /* Counts down, so the first phase after the start is "on". */
        c = (s_led.ready_ticks / READY_BLINK_TICKS) % 2U ? (ln_rgb_t){0, LEVEL, 0} : (ln_rgb_t){0, 0, 0};
    } else if (s_led.pairing && (s_led.tick / PAIR_BLINK_TICKS) % 2U) {
        c = (ln_rgb_t){0, 0, LEVEL};
    }
    ln_rgb_set(c);
}

void status_led_init(void)
{
    if (!ln_rgb_init(CONFIG_LOCHNESS_STATUS_LED_GPIO)) {
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

void status_led_ready(void) { s_led.ready_ticks = 2U * READY_BLINKS * READY_BLINK_TICKS; }

#else

void status_led_init(void) {}
void status_led_show(lp_bolt_t bolt, bool pairing)
{
    (void)bolt;
    (void)pairing;
}
void status_led_ready(void) {}

#endif
