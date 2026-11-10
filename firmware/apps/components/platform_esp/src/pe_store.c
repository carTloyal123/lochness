#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lock_core/lc_creds.h"
#include "lock_core/lc_lock.h"
#include "lock_core/lc_log.h"
#include "lock_core/lc_ratelimit.h"
#include "nvs.h"
#include "platform_esp/pe_platform.h"

static const char *const TAG = "pe_store";

#define NAMESPACE "lochness"
/* Bump when any persisted struct layout changes. Mismatched blobs are ignored. */
enum { SCHEMA_VERSION = 1 };

static esp_err_t load_blob(nvs_handle_t handle, const char *key, void *dst, size_t size)
{
    size_t len = size;
    esp_err_t err = nvs_get_blob(handle, key, dst, &len);
    if (err == ESP_OK && len != size) {
        ESP_LOGW(TAG, "'%s' has size %u, expected %u; ignoring", key, (unsigned)len, (unsigned)size);
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

static esp_err_t open_checked(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    esp_err_t err = nvs_open(NAMESPACE, mode, handle);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t version = 0;
    if (nvs_get_u32(*handle, "schema", &version) == ESP_OK && version != SCHEMA_VERSION) {
        ESP_LOGW(TAG, "schema %lu != %d; stored state ignored", (unsigned long)version, SCHEMA_VERSION);
        nvs_close(*handle);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

esp_err_t pe_store_load(lc_t *lc)
{
    nvs_handle_t handle = 0;
    esp_err_t err = open_checked(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored state (first boot)");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    lc_creds_t *creds = malloc(sizeof(*creds));
    lc_log_t *log = malloc(sizeof(*log));
    lc_ratelimit_t rl;
    if (creds && load_blob(handle, "creds", creds, sizeof(*creds)) == ESP_OK) {
        lc->creds = *creds;
    }
    if (load_blob(handle, "rl", &rl, sizeof(rl)) == ESP_OK) {
        lc->rl = rl;
    }
    if (log && load_blob(handle, "log", log, sizeof(*log)) == ESP_OK) {
        lc->log = *log;
    }
    if (creds) {
        memset(creds, 0, sizeof(*creds));
    }
    free(creds);
    free(log);
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t save_blob(const char *key, const void *src, size_t size)
{
    nvs_handle_t handle = 0;
    esp_err_t err = open_checked(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(handle, "schema", SCHEMA_VERSION);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, key, src, size);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving '%s' failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t pe_store_save(const lc_t *lc, lc_persist_t what)
{
    switch (what) {
    case LC_PERSIST_CREDS:
        return save_blob("creds", &lc->creds, sizeof(lc->creds));
    case LC_PERSIST_RATELIMIT:
        return save_blob("rl", &lc->rl, sizeof(lc->rl));
    case LC_PERSIST_LOG:
        return save_blob("log", &lc->log, sizeof(lc->log));
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t pe_store_save_config(const lc_config_t *cfg) { return save_blob("cfg", cfg, sizeof(*cfg)); }

bool pe_store_load_config(lc_config_t *cfg)
{
    nvs_handle_t handle = 0;
    if (open_checked(NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    lc_config_t tmp;
    bool ok = load_blob(handle, "cfg", &tmp, sizeof(tmp)) == ESP_OK;
    nvs_close(handle);
    if (ok) {
        *cfg = tmp;
    }
    return ok;
}

esp_err_t pe_store_save_peers(const void *peers, size_t len) { return save_blob("peers", peers, len); }

bool pe_store_load_peers(void *peers, size_t len)
{
    nvs_handle_t handle = 0;
    if (open_checked(NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    bool ok = load_blob(handle, "peers", peers, len) == ESP_OK;
    nvs_close(handle);
    return ok;
}

esp_err_t pe_store_erase(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
