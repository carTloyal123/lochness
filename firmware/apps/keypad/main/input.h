/*
 * Keypad input backends. Each backend posts characters to the keypad task:
 * '0'-'9', '*' (clear), '#' (unlock), 'L' (lock). Holding the pair button
 * starts pairing.
 *
 * Backends: 3x4 button matrix and dedicated buttons (GPIO). The capacitive
 * touch backend will plug in here once the touch controller is chosen.
 */
#pragma once

void input_start(void);
