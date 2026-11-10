#include "battery.h"

#include <stdint.h>

#include "sdkconfig.h"

#if CONFIG_KEYPAD_BATTERY_ADC_GPIO >= 0

#include <stdbool.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *const TAG = "battery";

enum {
    SAMPLES = 4,
    PERCENT = 100,
};

/* Set once in battery_init; read only from the keypad task afterwards. */
static struct {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t cali;
    adc_channel_t channel;
    bool ready;
    uint16_t last_mv;
} s_bat; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): module state

void battery_init(void)
{
    adc_unit_t unit_id = ADC_UNIT_1;
    if (adc_oneshot_io_to_channel(CONFIG_KEYPAD_BATTERY_ADC_GPIO, &unit_id, &s_bat.channel) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d is not an ADC pin", CONFIG_KEYPAD_BATTERY_ADC_GPIO);
        return;
    }
    const adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = unit_id};
    const adc_oneshot_chan_cfg_t chan_cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit_id, .chan = s_bat.channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_new_unit(&unit_cfg, &s_bat.unit) != ESP_OK ||
        adc_oneshot_config_channel(s_bat.unit, s_bat.channel, &chan_cfg) != ESP_OK ||
        adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_bat.cali) != ESP_OK) {
        ESP_LOGE(TAG, "ADC setup failed; battery reported as unknown");
        return;
    }
    s_bat.ready = true;
    ESP_LOGI(TAG, "battery %u mV", battery_mv());
}

uint16_t battery_mv(void)
{
    if (!s_bat.ready) {
        return 0;
    }
    int sum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        int mv = 0;
        if (adc_oneshot_get_calibrated_result(s_bat.unit, s_bat.cali, s_bat.channel, &mv) != ESP_OK) {
            return 0;
        }
        sum += mv;
    }
    long mv = (long)sum / SAMPLES * CONFIG_KEYPAD_BATTERY_DIVIDER_PCT / PERCENT;
    s_bat.last_mv = (uint16_t)(mv > UINT16_MAX ? UINT16_MAX : (mv < 1 ? 1 : mv));
    return s_bat.last_mv;
}

uint8_t battery_flags(void)
{
    return s_bat.ready && s_bat.last_mv && s_bat.last_mv < CONFIG_KEYPAD_BATTERY_LOW_MV ? LP_STATUS_LOW_BATTERY
                                                                                     : 0;
}

#else

void battery_init(void) {}
uint16_t battery_mv(void) { return 0; }
uint8_t battery_flags(void) { return 0; }

#endif
