/* Simulated motor until the DRV8833 + AS5600 driver lands (M1 hardware). */
#pragma once

#include <stdbool.h>

#include "lock_core/lc_lock.h"

void motor_sim_init(void);
void motor_sim_start(void *ctx, lc_op_t op);
void motor_sim_stop(void *ctx);
/* Make the next move report a jam (for exercising the jam path). */
void motor_sim_fail_next(void);
