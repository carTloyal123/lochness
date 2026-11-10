/* Matter Door Lock endpoint glue: see door_lock.cpp. */
#pragma once

#include <stdbool.h>

#include "link_proto/lp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* hub_link callbacks (hub task): queue the update into the CHIP context. */
void door_lock_on_state(const lp_msg_t *msg);
void door_lock_on_link(bool is_up);

#ifdef __cplusplus
}
#endif
