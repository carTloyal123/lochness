/*
 * Maps authenticated link requests onto lock_core. Pure C (no ESP-IDF) so the
 * host tests exercise the same code the firmware runs.
 */
#pragma once

#include "lock_core/lc_lock.h"
#include "lock_link/ll.h"

/* Peer roles map to lock_core sources: keypad → KEYPAD (PIN rules apply),
 * hub → REMOTE (remote policy applies), phone → APP. */
ll_access_result_t lock_bridge_access(lc_t *lc, lp_role_t role, lp_op_t op, const uint8_t *pin,
                                      size_t pin_len);
lp_bolt_t lock_bridge_bolt(const lc_t *lc);
