#include "bolt_sense.h"

#include "lock_core/lc_lock.h"
#include "sdkconfig.h"

#if CONFIG_LOCHNESS_BOLT_LOCKED_GPIO >= 0 && CONFIG_LOCHNESS_BOLT_UNLOCKED_GPIO >= 0

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#define LOCKED_PIN ((gpio_num_t)CONFIG_LOCHNESS_BOLT_LOCKED_GPIO)
#define UNLOCKED_PIN ((gpio_num_t)CONFIG_LOCHNESS_BOLT_UNLOCKED_GPIO)

/* Hall switches have hysteresis and don't bounce; this only rides out noise.
 * Kept short: the motor stops when the switch reports, so the delay is
 * overshoot. */
#define DEBOUNCE_MS 5

static const char *const TAG = "bolt";

static struct {
    TimerHandle_t debounce;
    bolt_sense_cb_t on_change;
    bolt_sense_t last;
} s_bolt; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): module state

bolt_sense_t bolt_sense_read(void)
{
    bool at_locked = gpio_get_level(LOCKED_PIN) == 0;
    bool at_unlocked = gpio_get_level(UNLOCKED_PIN) == 0;
    if (at_locked && at_unlocked) {
        return BOLT_SENSE_FAULT;
    }
    if (at_locked) {
        return BOLT_SENSE_LOCKED;
    }
    return at_unlocked ? BOLT_SENSE_UNLOCKED : BOLT_SENSE_BETWEEN;
}

bool bolt_sense_levels(int *locked_level, int *unlocked_level)
{
    *locked_level = gpio_get_level(LOCKED_PIN);
    *unlocked_level = gpio_get_level(UNLOCKED_PIN);
    return true;
}

static void debounce_expired(TimerHandle_t t)
{
    (void)t;
    bolt_sense_t pos = bolt_sense_read();
    if (pos == s_bolt.last) {
        return;
    }
    s_bolt.last = pos;
    if (pos == BOLT_SENSE_FAULT) {
        ESP_LOGW(TAG, "both bolt switches see a magnet: check the wiring and magnet");
    }
    if (s_bolt.on_change) {
        s_bolt.on_change(pos);
    }
}

static void IRAM_ATTR bolt_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xTimerResetFromISR(s_bolt.debounce, &woken);
    portYIELD_FROM_ISR(woken);
}

void bolt_sense_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LOCKED_PIN) | (1ULL << UNLOCKED_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* A3144 is open collector; harmless for the DRV5032 */
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    s_bolt.debounce = xTimerCreate("bolt", pdMS_TO_TICKS(DEBOUNCE_MS), pdFALSE, NULL, debounce_expired);
    configASSERT(s_bolt.debounce);
    s_bolt.last = bolt_sense_read();
    ESP_LOGI(TAG, "bolt switches on GPIO %d (locked) and %d (unlocked): %s", LOCKED_PIN, UNLOCKED_PIN,
             bolt_sense_name(s_bolt.last));
}

void bolt_sense_start(bolt_sense_cb_t on_change)
{
    s_bolt.on_change = on_change;
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* already installed is fine */
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(LOCKED_PIN, bolt_isr, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(UNLOCKED_PIN, bolt_isr, NULL));
    /* Catch a change between init and now. */
    xTimerReset(s_bolt.debounce, 0);
}

#else

void bolt_sense_init(void) {}
void bolt_sense_start(bolt_sense_cb_t on_change) { (void)on_change; }
bolt_sense_t bolt_sense_read(void) { return BOLT_SENSE_NONE; }
bool bolt_sense_levels(int *locked_level, int *unlocked_level)
{
    *locked_level = -1;
    *unlocked_level = -1;
    return false;
}

#endif

lc_bolt_t bolt_sense_to_lc(bolt_sense_t pos)
{
    switch (pos) {
    case BOLT_SENSE_LOCKED: return LC_BOLT_LOCKED;
    case BOLT_SENSE_UNLOCKED: return LC_BOLT_UNLOCKED;
    default: return LC_BOLT_UNKNOWN;
    }
}

const char *bolt_sense_name(bolt_sense_t pos)
{
    switch (pos) {
    case BOLT_SENSE_NONE: return "no switches";
    case BOLT_SENSE_LOCKED: return "locked";
    case BOLT_SENSE_UNLOCKED: return "unlocked";
    case BOLT_SENSE_BETWEEN: return "between";
    case BOLT_SENSE_FAULT: return "FAULT (both switches)";
    }
    return "?";
}
