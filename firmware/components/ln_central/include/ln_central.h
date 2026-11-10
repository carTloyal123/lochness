/*
 * BLE central for lock-link peers (keypad, hub): connects to the paired lock
 * (or scans for one with an open pairing window), exchanges MTU, discovers
 * the link service, subscribes, and hands frames to the caller.
 *
 * Disconnects requested by the caller are not reported back; disconnects
 * caused by the lock, the radio or a failed setup are.
 *
 * Callbacks run on the NimBLE host task and must only queue work.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lock address as stored by the peers: address type byte, then 6 bytes. */
#define LN_CENTRAL_ADDR_LEN 7

typedef struct {
    void (*connected)(const uint8_t *addr, size_t addr_len);
    void (*disconnected)(void);
    void (*frame)(const uint8_t *frame, size_t len);
} ln_central_cbs_t;

/*
 * own_stack: initialize and run NimBLE here (keypad). Otherwise another
 * component owns the host (the hub's Matter stack) and this one only waits
 * for it to sync.
 */
esp_err_t ln_central_start(const ln_central_cbs_t *cbs, bool own_stack);
/* addr == NULL: scan for a lock advertising an open pairing window. */
void ln_central_connect(const uint8_t *addr, size_t addr_len);
void ln_central_disconnect(void);
bool ln_central_send(const uint8_t *frame, size_t len);

#ifdef __cplusplus
}
#endif
