#if LOCHNESS_MATTER
#include "matter_lock.h"
#else
#include "ble_server.h"
#endif
#include "console.h"
#include "door.h"
#include "esp_log.h"
#include "lock_core/lc_lock.h"
#include "lock_task.h"
#include "motor_sim.h"
#include "platform_esp/pe_platform.h"
#include "status_led.h"

static const char *const TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(pe_init());
    motor_sim_init();
    status_led_init();
    lock_task_start(); /* creates device keys: must precede the radio */
#if LOCHNESS_MATTER
    /* Matter brings up BLE (commissioning + the keypad link) and Thread. */
    ESP_ERROR_CHECK(matter_lock_start());
#else
    ESP_ERROR_CHECK(ble_server_start());
#endif

    lc_door_t door = door_init();
    if (door != LC_DOOR_UNKNOWN) {
        lock_task_post_door(door);
    }

    ESP_LOGI(TAG, "lochness lock unit ready");
    status_led_ready();
    console_start();
}
