/*
 * BLE central for the keypad: connects to the paired lock (or scans for one
 * with an open pairing window), discovers the link service, subscribes, and
 * passes frames to the keypad task.
 *
 * Disconnects requested by the keypad are not reported back; disconnects
 * caused by the lock, the radio or a failed setup are.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "keypad_core/kp.h"

esp_err_t ble_central_start(void);
/* pairing == NULL: scan for a lock advertising an open pairing window. */
void ble_central_connect(const kp_pairing_t *pairing);
void ble_central_disconnect(void);
bool ble_central_send(const uint8_t *frame, size_t len);
