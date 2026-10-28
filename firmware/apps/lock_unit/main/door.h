/* Door position reed switch (optional; see CONFIG_LOCHNESS_DOOR_GPIO). */
#pragma once

#include "lock_core/lc_lock.h"

/* Configures the GPIO and returns the current door state (UNKNOWN if none). */
lc_door_t door_init(void);
