/* User feedback: LEDs and buzzer patterns for keypad events. */
#pragma once

#include <stdint.h>

#include "keypad_core/kp.h"

void feedback_start(void);
void feedback_show(kp_feedback_t feedback, uint16_t detail);
