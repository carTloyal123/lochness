#include "input.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keypad_task.h"
#include "sdkconfig.h"

static const char *TAG = "input";

#define SCAN_MS 10
#define DEBOUNCE_SCANS 3 /* 30 ms stable before a press counts */
#define PAIR_HOLD_MS 3000

#if CONFIG_KEYPAD_MATRIX
static const gpio_num_t ROWS[4] = {CONFIG_KEYPAD_ROW0_GPIO, CONFIG_KEYPAD_ROW1_GPIO, CONFIG_KEYPAD_ROW2_GPIO,
                                   CONFIG_KEYPAD_ROW3_GPIO};
static const gpio_num_t COLS[3] = {CONFIG_KEYPAD_COL0_GPIO, CONFIG_KEYPAD_COL1_GPIO, CONFIG_KEYPAD_COL2_GPIO};
static const char LAYOUT[4][3] = {{'1', '2', '3'}, {'4', '5', '6'}, {'7', '8', '9'}, {'*', '0', '#'}};
#endif

typedef struct {
    uint8_t stable_count;
    bool pressed;
} debounce_t;

/* Returns true on a debounced press edge. */
static bool debounce(debounce_t *d, bool raw)
{
    if (raw == d->pressed) {
        d->stable_count = 0;
        return false;
    }
    if (++d->stable_count < DEBOUNCE_SCANS) {
        return false;
    }
    d->stable_count = 0;
    d->pressed = raw;
    return raw;
}

static void input_pin(gpio_num_t pin)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&io));
}

static void input_task(void *arg)
{
    (void)arg;
#if CONFIG_KEYPAD_MATRIX
    static debounce_t keys[4][3];
#endif
#if CONFIG_KEYPAD_LOCK_BUTTON_GPIO >= 0
    static debounce_t lock_btn;
#endif
#if CONFIG_KEYPAD_PAIR_BUTTON_GPIO >= 0
    static debounce_t pair_btn;
    uint32_t pair_held_ms = 0;
    bool pair_fired = false;
#endif
    /* TODO(power): replace polling with GPIO wake from light/deep sleep. */
    for (;;) {
#if CONFIG_KEYPAD_MATRIX
        for (int r = 0; r < 4; r++) {
            gpio_set_level(ROWS[r], 0);
            esp_rom_delay_us(5);
            for (int c = 0; c < 3; c++) {
                if (debounce(&keys[r][c], gpio_get_level(COLS[c]) == 0)) {
                    keypad_task_post_key(LAYOUT[r][c]);
                }
            }
            gpio_set_level(ROWS[r], 1);
        }
#endif
#if CONFIG_KEYPAD_LOCK_BUTTON_GPIO >= 0
        if (debounce(&lock_btn, gpio_get_level(CONFIG_KEYPAD_LOCK_BUTTON_GPIO) == 0)) {
            keypad_task_post_key(KP_KEY_LOCK);
        }
#endif
#if CONFIG_KEYPAD_PAIR_BUTTON_GPIO >= 0
        debounce(&pair_btn, gpio_get_level(CONFIG_KEYPAD_PAIR_BUTTON_GPIO) == 0);
        if (pair_btn.pressed) {
            pair_held_ms += SCAN_MS;
            if (pair_held_ms >= PAIR_HOLD_MS && !pair_fired) {
                pair_fired = true;
                ESP_LOGI(TAG, "pair button held: pairing");
                keypad_task_post_pair();
            }
        } else {
            pair_held_ms = 0;
            pair_fired = false;
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(SCAN_MS));
    }
}

void input_start(void)
{
    bool any = false;
#if CONFIG_KEYPAD_MATRIX
    for (int r = 0; r < 4; r++) {
        gpio_config_t io = {.pin_bit_mask = 1ULL << ROWS[r], .mode = GPIO_MODE_OUTPUT_OD,
                            .pull_up_en = GPIO_PULLUP_ENABLE};
        ESP_ERROR_CHECK(gpio_config(&io));
        gpio_set_level(ROWS[r], 1);
    }
    for (int c = 0; c < 3; c++) {
        input_pin(COLS[c]);
    }
    any = true;
#endif
#if CONFIG_KEYPAD_LOCK_BUTTON_GPIO >= 0
    input_pin(CONFIG_KEYPAD_LOCK_BUTTON_GPIO);
    any = true;
#endif
#if CONFIG_KEYPAD_PAIR_BUTTON_GPIO >= 0
    input_pin(CONFIG_KEYPAD_PAIR_BUTTON_GPIO);
    any = true;
#endif
    if (!any) {
        ESP_LOGI(TAG, "no input hardware configured; use the console 'keys' command");
        return;
    }
    (void)input_pin;
    xTaskCreate(input_task, "input", 3072, NULL, 6, NULL);
}
