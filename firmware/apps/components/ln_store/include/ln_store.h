/*
 * Small NVS helpers shared by the lochness ESP-IDF apps.
 *
 * Secrets (long-term private keys) live in their own namespace so a factory
 * reset of app state does not rotate device identity. Until flash and NVS
 * encryption land (hardening milestone) they are stored in plaintext.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize NVS, erasing it only if its format is unusable. */
esp_err_t ln_store_init(void);

/*
 * Load secret `name`, creating it on first use from the hardware RNG with the
 * bootloader entropy source enabled.
 * MUST be called before Wi-Fi/BLE start: the entropy source cannot be
 * enabled while the radio is in use.
 */
esp_err_t ln_secret_get_or_create(const char *name, uint8_t *out, size_t len);

/* Returns ESP_ERR_NVS_NOT_FOUND if absent, ESP_ERR_INVALID_SIZE on size mismatch. */
esp_err_t ln_blob_load(const char *nspace, const char *key, void *out, size_t len);
esp_err_t ln_blob_save(const char *nspace, const char *key, const void *data, size_t len);
esp_err_t ln_namespace_erase(const char *nspace);

#ifdef __cplusplus
}
#endif
