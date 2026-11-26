/*
 * Keypad input backends. Each backend posts characters to the keypad task:
 * '0'-'9', '*' (clear), '#' (unlock), 'L' (lock), 'T' (toggle). Holding the
 * pair button starts pairing; a short press types CONFIG_KEYPAD_BENCH_PIN and
 * 'T' when that bench option is set.
 *
 * Backends: 3x4 button matrix and dedicated buttons (GPIO). The capacitive
 * touch backend will plug in here once the touch controller is chosen.
 */
#pragma once

void input_start(void);
