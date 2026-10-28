#include "ble_server.h"
#include "console.h"
#include "door.h"
#include "esp_log.h"
#include "lock_task.h"
#include "motor_sim.h"
#include "platform_esp/pe_platform.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(pe_init());
    motor_sim_init();
    lock_task_start(); /* creates device keys: must precede the radio */
    ESP_ERROR_CHECK(ble_server_start());

    lc_door_t door = door_init();
    if (door != LC_DOOR_UNKNOWN) {
        lock_task_post_door(door);
    }

    ESP_LOGI(TAG, "lochness lock unit ready");
    console_start();
}
