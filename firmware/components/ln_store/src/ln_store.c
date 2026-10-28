#include "ln_store.h"

#include <stdint.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *const TAG = "ln_store";

#define SECRET_NS "lochness_key"

esp_err_t ln_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Only after a partition layout or NVS format change. */
        ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

esp_err_t ln_blob_load(const char *nspace, const char *key, void *out, size_t len)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(nspace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t got = len;
    err = nvs_get_blob(handle, key, out, &got);
    nvs_close(handle);
    if (err == ESP_OK && got != len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

esp_err_t ln_blob_save(const char *nspace, const char *key, const void *data, size_t len)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(nspace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t ln_namespace_erase(const char *nspace)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(nspace, NVS_READWRITE, &handle);
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

esp_err_t ln_secret_get_or_create(const char *name, uint8_t *out, size_t len)
{
    esp_err_t err = ln_blob_load(SECRET_NS, name, out, len);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_INVALID_SIZE) {
        return err;
    }
    bootloader_random_enable();
    esp_fill_random(out, len);
    bootloader_random_disable();
    err = ln_blob_save(SECRET_NS, name, out, len);
    if (err != ESP_OK) {
        memset(out, 0, len);
        return err;
    }
    ESP_LOGI(TAG, "created secret '%s'", name);
    return ESP_OK;
}
