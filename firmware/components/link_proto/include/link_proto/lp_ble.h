/*
 * BLE transport mapping for the lochness link (shared with the mobile apps).
 *
 * The lock is a GATT server exposing one service:
 *   RX  write / write-without-response : one link frame per write (peer -> lock)
 *   TX  notify                         : one link frame per notification (lock -> peer)
 * Peers must exchange MTU (>= LP_BLE_MIN_MTU) and subscribe to TX before
 * sending the first frame.
 *
 * Advertising: 128-bit service data for LP_BLE_SERVICE_UUID with one flags
 * byte (LP_BLE_ADV_FLAG_*). No device name.
 *
 * UUIDs (canonical form):
 *   service 68af0681-139d-4a1a-9909-d52a69a3cae6
 *   rx      68af0682-139d-4a1a-9909-d52a69a3cae6
 *   tx      68af0683-139d-4a1a-9909-d52a69a3cae6
 * The byte arrays below are little-endian, as BLE puts them on the air.
 */
#pragma once

#define LP_BLE_UUID_LE(b12)                                                                        \
    {0xe6, 0xca, 0xa3, 0x69, 0x2a, 0xd5, 0x09, 0x99, 0x1a, 0x4a, 0x9d, 0x13, (b12), 0x06, 0xaf, 0x68}

#define LP_BLE_SERVICE_UUID_LE LP_BLE_UUID_LE(0x81)
#define LP_BLE_RX_UUID_LE LP_BLE_UUID_LE(0x82)
#define LP_BLE_TX_UUID_LE LP_BLE_UUID_LE(0x83)

#define LP_BLE_ADV_FLAG_PAIRING 0x01 /* a pairing window is open */

/* LP_MAX_FRAME + 3 bytes of ATT header. */
#define LP_BLE_MIN_MTU 203
