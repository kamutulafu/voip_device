#include "app_time.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "app_time";
static const char *NVS_NAMESPACE = "app_time";
static const char *NVS_KEY_TZ_MIN = "tz_min";

void app_time_init(void)
{
    nvs_handle_t handle;
    int32_t tz_min = 480; // Default UTC+8 (480 minutes)
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_i32(handle, NVS_KEY_TZ_MIN, &tz_min);
        nvs_close(handle);
    }

    setenv("TZ", APP_TIME_DEFAULT_TZ, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone initialized (default %s)", APP_TIME_DEFAULT_TZ);
}

esp_err_t app_time_set_utc_offset_minutes(int minutes_east)
{
    if (minutes_east < -840 || minutes_east > 840) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_i32(handle, NVS_KEY_TZ_MIN, minutes_east);
        nvs_commit(handle);
        nvs_close(handle);
    }
    return ESP_OK;
}

esp_err_t app_time_set_epoch(int64_t epoch_seconds)
{
    if (epoch_seconds < 1704067200) { // 2024-01-01
        return ESP_ERR_INVALID_ARG;
    }
    struct timeval tv = { .tv_sec = (time_t)epoch_seconds, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_time_local_str(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &tm_now);
}
