/*
 * ESP-IDF implementations of the services lock_core needs: time, randomness,
 * the hardware-keyed PIN digest, and NVS persistence.
 */
#pragma once

#include "esp_err.h"
#include "lock_core/lc_lock.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize NVS, crypto and the PIN key. Call once before anything else. */
esp_err_t pe_init(void);
lc_platform_t pe_platform(void);

/* Wall time arrives from the hub (Matter Time Sync); until then it is unknown. */
void pe_time_set(uint32_t unix_s, int16_t tz_offset_min);
bool pe_time_known(void);

/* Load persisted creds / rate limit / log into lc. Missing or incompatible
 * blobs leave the corresponding state as lc_init() set it. */
esp_err_t pe_store_load(lc_t *lc);
esp_err_t pe_store_save(const lc_t *lc, lc_persist_t what);
esp_err_t pe_store_save_config(const lc_config_t *cfg);
/* Returns false if no saved config exists. */
bool pe_store_load_config(lc_config_t *cfg);
/* Paired link peers (opaque blob owned by lock_link). Erased by pe_store_erase. */
esp_err_t pe_store_save_peers(const void *peers, size_t len);
bool pe_store_load_peers(void *peers, size_t len);

/* Erase all lock state (users, PINs, log, config, paired peers). Device keys are kept. */
esp_err_t pe_store_erase(void);

#ifdef __cplusplus
}
#endif
