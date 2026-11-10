/*
 * Keypad state on the DevKitC-1's onboard RGB LED (WS2812, GPIO 8 by default),
 * for the bench. Green means steady state; every other color is a transition.
 *
 *   white        starting up
 *   blue blink   searching for a lock with an open pairing window
 *   amber        not paired (hold the pair button, or `pair`)
 *   green        paired and ready
 *
 * A battery keypad would leave this off (CONFIG_KEYPAD_STATUS_LED_GPIO = -1);
 * lock/unlock results use the separate green/red LEDs and buzzer.
 */
#pragma once

#include <stdbool.h>

void status_led_init(void);
/* Boot finished (BLE and input up): leave the startup color. */
void status_led_ready(void);
/* From the keypad task, whenever pairing state may have changed. */
void status_led_show(bool paired, bool pairing);
