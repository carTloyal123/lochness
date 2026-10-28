#include "door.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "lock_core/lc_lock.h"
#include "sdkconfig.h"

#if CONFIG_LOCHNESS_DOOR_GPIO >= 0

#define DOOR_PIN ((gpio_num_t)CONFIG_LOCHNESS_DOOR_GPIO)
#define DEBOUNCE_MS 50

static TimerHandle_t s_debounce;

static lc_door_t read_door(void)
{
    /* Reed to GND with pull-up: magnet present → contact closed → low. */
    return gpio_get_level(DOOR_PIN) == 0 ? LC_DOOR_CLOSED : LC_DOOR_OPEN;
}

static void debounce_expired(TimerHandle_t t)
{
    (void)t;
    lock_task_post_door(read_door());
}

static void IRAM_ATTR door_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xTimerResetFromISR(s_debounce, &woken);
    portYIELD_FROM_ISR(woken);
}

lc_door_t door_init(void)
{
    s_debounce = xTimerCreate("door", pdMS_TO_TICKS(DEBOUNCE_MS), pdFALSE, NULL, debounce_expired);
    configASSERT(s_debounce);
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << DOOR_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(DOOR_PIN, door_isr, NULL));
    return read_door();
}

#else

lc_door_t door_init(void) { return LC_DOOR_UNKNOWN; }

#endif
