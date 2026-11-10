#include "ln_rgb.h"

#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "led_strip_types.h"

static const char *const TAG = "rgb";

enum { RMT_RESOLUTION_HZ = 10000000 };

static struct {
    led_strip_handle_t strip;
    ln_rgb_t shown;
    bool shown_valid;
} s_rgb; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): module state

bool ln_rgb_init(int gpio)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {.resolution_hz = RMT_RESOLUTION_HZ};
    if (led_strip_new_rmt_device(&cfg, &rmt, &s_rgb.strip) != ESP_OK) {
        ESP_LOGW(TAG, "no LED on GPIO %d", gpio);
        s_rgb.strip = NULL;
        return false;
    }
    return true;
}

void ln_rgb_set(ln_rgb_t color)
{
    if (!s_rgb.strip ||
        (s_rgb.shown_valid && color.r == s_rgb.shown.r && color.g == s_rgb.shown.g && color.b == s_rgb.shown.b)) {
        return;
    }
    if (led_strip_set_pixel(s_rgb.strip, 0, color.r, color.g, color.b) == ESP_OK &&
        led_strip_refresh(s_rgb.strip) == ESP_OK) {
        s_rgb.shown = color;
        s_rgb.shown_valid = true;
    }
}
