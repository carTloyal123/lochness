#include "ble_central.h"
#include "console.h"
#include "esp_log.h"
#include "feedback.h"
#include "input.h"
#include "keypad_task.h"
#include "ln_store.h"

static const char *const TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(ln_store_init());
    feedback_start();
    keypad_task_start(); /* creates the device key: must precede the radio */
    ESP_ERROR_CHECK(ble_central_start());
    input_start();
    ESP_LOGI(TAG, "lochness keypad ready");
    console_start();
}
