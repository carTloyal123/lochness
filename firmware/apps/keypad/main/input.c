#include "input.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keypad_core/kp.h"
#include "keypad_task.h"
#include "link_proto/lp.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *const TAG = "input";

enum {
    SCAN_MS = 10,
    DEBOUNCE_SCANS = 3, /* 30 ms stable before a press counts */
    PAIR_HOLD_MS = 3000,
    ROW_COUNT = 4,
    COL_COUNT = 3,
    ROW_SETTLE_US = 5,
    INPUT_TASK_STACK = 3072,
    INPUT_TASK_PRIO = 6,
};

#if CONFIG_KEYPAD_MATRIX
static const gpio_num_t ROWS[ROW_COUNT] = {CONFIG_KEYPAD_ROW0_GPIO, CONFIG_KEYPAD_ROW1_GPIO, CONFIG_KEYPAD_ROW2_GPIO,
                                   CONFIG_KEYPAD_ROW3_GPIO};
static const gpio_num_t COLS[COL_COUNT] = {CONFIG_KEYPAD_COL0_GPIO, CONFIG_KEYPAD_COL1_GPIO, CONFIG_KEYPAD_COL2_GPIO};
static const char LAYOUT[ROW_COUNT][COL_COUNT] = {{'1', '2', '3'}, {'4', '5', '6'}, {'7', '8', '9'}, {'*', '0', '#'}};
#endif

typedef struct {
    uint8_t stable_count;
    bool pressed;
} debounce_t;

#if CONFIG_KEYPAD_PAIR_BUTTON_GPIO >= 0
#define BENCH_PIN CONFIG_KEYPAD_BENCH_PIN

/* The bench PIN, if set and valid: 1 to LP_PIN_FIELD_LEN digits. */
static bool bench_pin_valid(void)
{
    size_t len = strlen(BENCH_PIN);
    if (len == 0) {
        return false;
    }
    if (len > LP_PIN_FIELD_LEN || strspn(BENCH_PIN, "0123456789") != len) {
        ESP_LOGE(TAG, "bench PIN must be 1-%d digits; short press does nothing", LP_PIN_FIELD_LEN);
        return false;
    }
    return true;
}

/* Short press of the pair button: type the bench PIN and toggle (unlock if
 * locked, lock if unlocked). The keypad task sees ordinary keys, so the lock
 * applies its usual PIN rules. */
static void bench_toggle(void)
{
    ESP_LOGI(TAG, "pair button pressed: bench toggle");
    keypad_task_post_key(KP_KEY_CLEAR); /* drop any half-typed digits */
    for (const char *k = BENCH_PIN; *k; k++) {
        keypad_task_post_key(*k);
    }
    keypad_task_post_key(KP_KEY_TOGGLE);
}
#endif

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
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (unsigned)pin, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static void input_task(void *arg)
{
    (void)arg;
#if CONFIG_KEYPAD_MATRIX
    static debounce_t keys[ROW_COUNT][COL_COUNT];
#endif
#if CONFIG_KEYPAD_LOCK_BUTTON_GPIO >= 0
    static debounce_t lock_btn;
#endif
#if CONFIG_KEYPAD_PAIR_BUTTON_GPIO >= 0
    static debounce_t pair_btn;
    uint32_t pair_held_ms = 0;
    bool pair_fired = false;
    const bool bench_toggle_on = bench_pin_valid();
#endif
    /* TODO(power): replace polling with GPIO wake from light/deep sleep. */
    for (;;) {
#if CONFIG_KEYPAD_MATRIX
        for (int r = 0; r < ROW_COUNT; r++) {
            gpio_set_level(ROWS[r], 0);
            esp_rom_delay_us(ROW_SETTLE_US);
            for (int c = 0; c < COL_COUNT; c++) {
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
            /* Released before the pairing hold: a short press. */
            if (pair_held_ms > 0 && !pair_fired && bench_toggle_on) {
                bench_toggle();
            }
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
    for (int r = 0; r < ROW_COUNT; r++) {
        gpio_config_t cfg = {.pin_bit_mask = 1ULL << (unsigned)ROWS[r], .mode = GPIO_MODE_OUTPUT_OD,
                             .pull_up_en = GPIO_PULLUP_ENABLE};
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(ROWS[r], 1);
    }
    for (int c = 0; c < COL_COUNT; c++) {
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
    xTaskCreate(input_task, "input", INPUT_TASK_STACK, NULL, INPUT_TASK_PRIO, NULL);
}
