/*
 * Wall clock for the lock's schedules and log, from SNTP once the hub is on
 * the network. Local time follows CONFIG_HUB_TZ (POSIX TZ string).
 *
 * TODO(M3): take UTC from the Matter Time Synchronization cluster too, so a
 * hub with no WAN access (blocked at the router) still keeps time.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void wall_clock_init(void);
/* Call when the hub gets an IP address; starts SNTP once. */
void wall_clock_network_up(void);
/* Current UTC and local offset (DST included); false until synced. */
bool wall_clock_now(uint32_t *unix_s, int16_t *utc_offset_min);

#ifdef __cplusplus
}
#endif
