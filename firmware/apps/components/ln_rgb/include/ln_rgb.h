/*
 * One WS2812 RGB LED, such as the ESP32-C6-DevKitC-1's onboard LED (GPIO 8).
 * Shared by the lock's and keypad's status LEDs; each app picks its own
 * colors and blink patterns.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t r, g, b;
} ln_rgb_t;

/* False if the LED could not be set up (no LED on that pin). */
bool ln_rgb_init(int gpio);
/* Show a color; repeats of the current color cost nothing. One caller at a
 * time (the app's LED timer). */
void ln_rgb_set(ln_rgb_t color);

#ifdef __cplusplus
}
#endif
