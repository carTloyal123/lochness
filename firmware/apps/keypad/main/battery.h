/*
 * Battery voltage through a resistor divider on an ADC pin. Without one
 * (CONFIG_KEYPAD_BATTERY_ADC_GPIO = -1, the DevKit default) the keypad
 * reports 0 mV, which the protocol defines as "unknown".
 */
#pragma once

#include <stdint.h>

void battery_init(void);
/* Battery voltage in mV, or 0 if there is no sense circuit or the read failed. */
uint16_t battery_mv(void);
/* LP_STATUS_LOW_BATTERY when a known reading is below the low threshold. */
uint8_t battery_flags(void);
