/*
 * Matter over Thread: the lock unit is the Matter node. One Door Lock endpoint
 * backed by the lock task; see matter_lock.cpp.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the Door Lock endpoint, give the link's GATT service to Matter's BLE
 * manager and start Matter (Thread + BLE commissioning). Call after
 * lock_task_start(), which must create the device keys before the radio. */
esp_err_t matter_lock_start(void);

/* Console. Output is printed from the Matter thread, just after return. */
void matter_lock_print_status(void);
/* Open the commissioning window (BLE + DNS-SD) for 3..15 minutes. */
esp_err_t matter_lock_open_window(int minutes);
/* Thread radio: channel and MAC transmit counters, to measure contention.
 * reset zeroes the counters first. */
void matter_lock_print_radio(bool reset);
/* Forget every Matter fabric and the Thread network, then reboot. Lock users,
 * PINs, the log and keypad pairings are kept. */
void matter_lock_factory_reset(void);

#ifdef __cplusplus
}
#endif
