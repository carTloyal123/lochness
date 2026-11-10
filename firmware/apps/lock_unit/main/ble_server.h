/*
 * BLE GATT server for the lochness link (see link_proto/lp_ble.h). Runs on the
 * NimBLE host task and forwards connection events and frames to the lock task.
 *
 * The link advertises on its own extended advertising set (legacy PDUs, public
 * address), so Matter's commissioning advertising can run alongside it.
 *
 * Two ways to start:
 *  - With Matter, which owns the NimBLE host: hand ble_server_services() to
 *    Matter's BLE manager before Matter starts, then ble_server_attach().
 *  - Without Matter: ble_server_start() brings up NimBLE itself.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_server_start(void);

#if CONFIG_BT_ENABLED
struct ble_gatt_svc_def;
/* The link's GATT services, terminated by an empty entry. */
const struct ble_gatt_svc_def *ble_server_services(void);
/* Start advertising once the (Matter-owned) host is up. */
esp_err_t ble_server_attach(void);
#endif

bool ble_server_send(uint16_t conn, const uint8_t *frame, size_t len);
void ble_server_disconnect(uint16_t conn);
/* Reflect the pairing window in the advertising flags. */
void ble_server_set_pairing(bool open);
/* Stop advertising the link (peers can't connect) until resumed. For radio
 * experiments; live connections are kept. */
void ble_server_set_paused(bool paused);
bool ble_server_paused(void);

#ifdef __cplusplus
}
#endif
