/*
 * BLE GATT server for the lochness link (see link_proto/lp_ble.h). Runs on the
 * NimBLE host task and forwards connection events and frames to the lock task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t ble_server_start(void);
bool ble_server_send(uint16_t conn, const uint8_t *frame, size_t len);
void ble_server_disconnect(uint16_t conn);
/* Reflect the pairing window in the advertising flags. */
void ble_server_set_pairing(bool open);
