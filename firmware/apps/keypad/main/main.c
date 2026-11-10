#include "console.h"
#include "esp_log.h"
#include "feedback.h"
#include "input.h"
#include "keypad_task.h"
#include "ln_central.h"
#include "ln_store.h"
#include "status_led.h"

static const char *const TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(ln_store_init());
    status_led_init();
    feedback_start();
    keypad_task_start(); /* creates the device key: must precede the radio */
    static const ln_central_cbs_t CENTRAL_CBS = {
        .connected = keypad_task_post_connected,
        .disconnected = keypad_task_post_disconnected,
        .frame = keypad_task_post_frame,
    };
    ESP_ERROR_CHECK(ln_central_start(&CENTRAL_CBS));
    input_start();
    ESP_LOGI(TAG, "lochness keypad ready");
    status_led_ready();
    console_start();
}
