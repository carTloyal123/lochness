#if LOCHNESS_MATTER
#include "matter_lock.h"
#else
#include "ble_server.h"
#endif
#include "bolt_sense.h"
#include "console.h"
#include "door.h"
#include "esp_log.h"
#include "lock_core/lc_lock.h"
#include "lock_task.h"
#include "motor.h"
#include "platform_esp/pe_platform.h"
#include "status_led.h"

static const char *const TAG = "main";

/* Bolt switch changes: the motor stops on its target, and lock_core hears of
 * resting positions (a manual turn when no move is under way). Every change,
 * "between" included, also wakes the lock task to refresh the LED. */
static void on_bolt(bolt_sense_t pos)
{
    motor_on_bolt(pos);
    lock_task_post_bolt_position(bolt_sense_to_lc(pos));
}

void app_main(void)
{
    ESP_ERROR_CHECK(pe_init());
    bolt_sense_init(); /* before the lock task: it reads the bolt at boot */
    motor_init();
    status_led_init();
    lock_task_start(); /* creates device keys: must precede the radio */
    bolt_sense_start(on_bolt);
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
