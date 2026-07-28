#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "time_sync.h"
#include "app_time.h"

static const char *TAG = "time_sync";

/* 备选校时目标：都用明文 http://，不走 TLS，避免时间未校准时
 * mbedTLS 因证书"尚未生效"而握手失败。只需要响应头里的 Date
 * 字段，请求本身是否 200 成功并不重要。 */
static const char *s_bootstrap_hosts[] = {
    "http://www.baidu.com/",
    "http://tts-api.xfyun.cn/",
};
#define BOOTSTRAP_HOST_COUNT (sizeof(s_bootstrap_hosts) / sizeof(s_bootstrap_hosts[0]))

#define TIME_SYNC_MAX_ATTEMPTS   3
#define TIME_SYNC_REQUEST_TIMEOUT_MS 4000

static char s_date_buf[64];

static esp_err_t http_evt_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        evt->header_key != NULL && evt->header_value != NULL &&
        strcasecmp(evt->header_key, "Date") == 0) {
        strncpy(s_date_buf, evt->header_value, sizeof(s_date_buf) - 1);
        s_date_buf[sizeof(s_date_buf) - 1] = '\0';
    }
    return ESP_OK;
}

/* 手写解析 RFC1123 日期（例如 "Wed, 21 Oct 2015 07:28:00 GMT"），
 * 不依赖 strptime —— 部分 esp-idf newlib-nano 配置下 strptime
 * 支持不完整，手写 sscanf 更保险。讯飞 date 参数用的也是这个
 * 格式，两者天然一致。 */
static bool apply_http_date(const char *date_str)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    char mon[4] = {0};
    int mday = 0, year = 0, hh = 0, mm = 0, ss = 0;

    if (sscanf(date_str, "%*3s, %d %3s %d %d:%d:%d",
               &mday, mon, &year, &hh, &mm, &ss) != 6) {
        ESP_LOGW(TAG, "failed to parse HTTP Date header: '%s'", date_str);
        return false;
    }

    int month = -1;
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon, months[i], 3) == 0) {
            month = i;
            break;
        }
    }
    if (month < 0) {
        ESP_LOGW(TAG, "unrecognized month in HTTP Date header: '%s'", date_str);
        return false;
    }

    struct tm tm = {
        .tm_year = year - 1900,
        .tm_mon  = month,
        .tm_mday = mday,
        .tm_hour = hh,
        .tm_min  = mm,
        .tm_sec  = ss,
    };

    /* 备份当前时区，计算完 UTC 纪元秒后还原，避免全局时区被误设为 UTC0 */
    char old_tz[32] = {0};
    const char *env_tz = getenv("TZ");
    if (env_tz) {
        strncpy(old_tz, env_tz, sizeof(old_tz) - 1);
    }

    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(&tm);

    if (old_tz[0] != '\0') {
        setenv("TZ", old_tz, 1);
    } else {
        setenv("TZ", APP_TIME_DEFAULT_TZ, 1);
    }
    tzset();
    if (t <= 0) {
        ESP_LOGW(TAG, "mktime() failed for parsed HTTP Date");
        return false;
    }

    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "settimeofday() failed");
        return false;
    }

    ESP_LOGI(TAG, "system time synced from HTTP Date header: %s", date_str);
    return true;
}

static bool try_one_host(const char *url)
{
    s_date_buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_HEAD,
        .event_handler = http_evt_handler,
        .timeout_ms = TIME_SYNC_REQUEST_TIMEOUT_MS,
        .disable_auto_redirect = true, /* 拿到首个响应头即可，不必跟进跳转 */
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGW(TAG, "esp_http_client_init failed for %s", url);
        return false;
    }

    /* 注意：这里不严格依赖 perform() 的返回值。已知个别 esp-idf
     * 版本在 disable_auto_redirect=true 时会在内部多绕一圈才返回
     * 非 ESP_OK，但响应头（含 Date）在此之前已经被
     * http_evt_handler 捕获，所以只要 s_date_buf 非空就可以直接
     * 尝试应用，不必等待 perform() 报告成功。 */
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (s_date_buf[0] == '\0') {
        ESP_LOGW(TAG, "no Date header captured from %s", url);
        return false;
    }
    return apply_http_date(s_date_buf);
}

static void time_sync_task(void *arg)
{
    for (int attempt = 0; attempt < TIME_SYNC_MAX_ATTEMPTS; attempt++) {
        for (size_t i = 0; i < BOOTSTRAP_HOST_COUNT; i++) {
            ESP_LOGI(TAG, "bootstrap time sync attempt %d/%d via %s",
                     attempt + 1, TIME_SYNC_MAX_ATTEMPTS, s_bootstrap_hosts[i]);
            if (try_one_host(s_bootstrap_hosts[i])) {
                vTaskDelete(NULL);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000 * (attempt + 1))); /* 简单退避重试 */
    }

    ESP_LOGW(TAG, "HTTP date bootstrap failed after %d attempts; "
                  "will fall back to SNTP on first TTS/ASR call",
             TIME_SYNC_MAX_ATTEMPTS);
    vTaskDelete(NULL);
}

void time_sync_start(void)
{
    xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 5, NULL);
}
