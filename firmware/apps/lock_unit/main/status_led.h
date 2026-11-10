/*
 * Lock state on the DevKitC-1's RGB LED (WS2812, GPIO 8 by default), for the
 * bench while the lock has no motor. Disabled when
 * CONFIG_LOCHNESS_STATUS_LED_GPIO is -1.
 *
 *   red      locked              green    unlocked
 *   amber    bolt moving         white    unknown
 *   red, fast blink: jammed      blue blink: pairing window open
 *   three green blinks: booted and ready (Matter and the keypad link up)
 */
#pragma once

#include <stdbool.h>

#include "link_proto/lp.h"

void status_led_init(void);
/* Cheap; call whenever state may have changed. Applied within ~100 ms. */
void status_led_show(lp_bolt_t bolt, bool pairing);
/* Blink green three times, then go back to the bolt colors. */
void status_led_ready(void);
