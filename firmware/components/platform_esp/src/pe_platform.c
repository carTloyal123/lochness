#include "platform_esp/pe_platform.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "ln_store.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#if CONFIG_LOCHNESS_PIN_KEY_EFUSE
#include "esp_hmac.h"
#endif

static const char *TAG = "pe";

#define KEY_NAME "pin_hmac"
#define KEY_LEN 32

static bool s_time_known;
static uint32_t s_time_unix_at_set;
static uint64_t s_time_ms_at_set;
static int16_t s_tz_offset_min;

#if CONFIG_LOCHNESS_PIN_KEY_DEV_NVS
static psa_key_id_t s_pin_key;

static esp_err_t dev_key_init(void)
{
    uint8_t key[KEY_LEN];
    esp_err_t err = ln_secret_get_or_create(KEY_NAME, key, sizeof(key));
    if (err != ESP_OK) {
        return err;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, KEY_LEN * 8);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_status_t st = psa_import_key(&attr, key, sizeof(key), &s_pin_key);
    memset(key, 0, sizeof(key));
    return st == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}
#endif

esp_err_t pe_init(void)
{
    esp_err_t err = ln_store_init();
    if (err != ESP_OK) {
        return err;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
#if CONFIG_LOCHNESS_PIN_KEY_DEV_NVS
    ESP_LOGW(TAG, "PIN digests use a DEVELOPMENT key stored in NVS. Not for production.");
    return dev_key_init();
#else
    return ESP_OK;
#endif
}

static uint64_t p_now_ms(void *ctx)
{
    (void)ctx;
    return (uint64_t)esp_timer_get_time() / 1000u;
}

void pe_time_set(uint32_t unix_s, int16_t tz_offset_min)
{
    s_time_unix_at_set = unix_s;
    s_time_ms_at_set = p_now_ms(NULL);
    s_tz_offset_min = tz_offset_min;
    s_time_known = true;
}

bool pe_time_known(void) { return s_time_known; }

static bool p_wall_time(void *ctx, lc_wall_time_t *out)
{
    (void)ctx;
    if (!s_time_known) {
        return false;
    }
    uint32_t unix_s = s_time_unix_at_set + (uint32_t)((p_now_ms(NULL) - s_time_ms_at_set) / 1000u);
    int64_t local = (int64_t)unix_s + (int64_t)s_tz_offset_min * 60;
    int64_t days = local / 86400;
    out->unix_s = unix_s;
    out->weekday = (uint8_t)((days + 4) % 7); /* 1970-01-01 was a Thursday */
    out->minute_of_day = (uint16_t)((local % 86400) / 60);
    return true;
}

static lc_err_t p_pin_digest(void *ctx, const uint8_t salt[LC_SALT_LEN], const uint8_t *pin,
                             size_t pin_len, uint8_t out[LC_DIGEST_LEN])
{
    (void)ctx;
    if (pin_len > LC_PIN_MAX_LEN) {
        return LC_ERR_INVALID_ARG;
    }
    uint8_t msg[LC_SALT_LEN + LC_PIN_MAX_LEN];
    memcpy(msg, salt, LC_SALT_LEN);
    memcpy(msg + LC_SALT_LEN, pin, pin_len);
    size_t msg_len = LC_SALT_LEN + pin_len;
    lc_err_t result = LC_ERR_PLATFORM;

#if CONFIG_LOCHNESS_PIN_KEY_EFUSE
    if (esp_hmac_calculate((hmac_key_id_t)CONFIG_LOCHNESS_PIN_KEY_EFUSE_BLOCK, msg, msg_len, out) ==
        ESP_OK) {
        result = LC_OK;
    }
#else
    size_t out_len = 0;
    if (psa_mac_compute(s_pin_key, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, msg_len, out, LC_DIGEST_LEN,
                        &out_len) == PSA_SUCCESS &&
        out_len == LC_DIGEST_LEN) {
        result = LC_OK;
    }
#endif
    memset(msg, 0, sizeof(msg));
    return result;
}

static lc_err_t p_random(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return LC_OK;
}

lc_platform_t pe_platform(void)
{
    return (lc_platform_t){
        .ctx = NULL,
        .now_ms = p_now_ms,
        .wall_time = p_wall_time,
        .pin_digest = p_pin_digest,
        .random = p_random,
    };
}
