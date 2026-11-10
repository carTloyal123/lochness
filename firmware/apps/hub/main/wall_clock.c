#include "wall_clock.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "hub_link.h"
#include "sdkconfig.h"

static const char *const TAG = "clock";

enum {
    MINUTES_PER_HOUR = 60,
    MINUTES_PER_DAY = 1440,
    SECONDS_PER_MINUTE = 60,
};

static volatile bool s_synced; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): set by the SNTP callback

static void on_sync(struct timeval *time_value)
{
    bool first = !s_synced;
    s_synced = true;
    if (first) {
        ESP_LOGI(TAG, "time synced: %lld", (long long)time_value->tv_sec);
    }
    hub_link_time_changed();
}

void wall_clock_init(void)
{
    /* Called from app_main before any other task reads the environment. */
    setenv("TZ", CONFIG_HUB_TZ, 1); // NOLINT(concurrency-mt-unsafe)
    tzset();
}

void wall_clock_network_up(void)
{
    static bool started = false;
    if (started || CONFIG_HUB_SNTP_SERVER[0] == '\0') {
        return;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_HUB_SNTP_SERVER);
    config.sync_cb = on_sync;
    config.wait_for_sync = false;
    if (esp_netif_sntp_init(&config) == ESP_OK) {
        started = true;
        ESP_LOGI(TAG, "SNTP started (%s)", CONFIG_HUB_SNTP_SERVER);
    } else {
        ESP_LOGW(TAG, "SNTP start failed");
    }
}

bool wall_clock_now(uint32_t *unix_s, int16_t *utc_offset_min)
{
    if (!s_synced) {
        return false;
    }
    time_t now = time(NULL);
    struct tm local;
    struct tm utc;
    if (localtime_r(&now, &local) == NULL || gmtime_r(&now, &utc) == NULL) {
        return false;
    }
    int minutes = ((local.tm_hour - utc.tm_hour) * MINUTES_PER_HOUR) + (local.tm_min - utc.tm_min);
    /* Crossing midnight: the local date differs from the UTC date by one day. */
    int day_delta = local.tm_yday - utc.tm_yday;
    if (local.tm_year != utc.tm_year) {
        day_delta = local.tm_year > utc.tm_year ? 1 : -1;
    }
    minutes += day_delta * MINUTES_PER_DAY;
    *unix_s = (uint32_t)now;
    *utc_offset_min = (int16_t)minutes;
    return true;
}
