/*
 * WeChat Cloud VoIP SDK - OS adaptation layer for ESP-IDF / FreeRTOS.
 *
 * Ported from the SDK demo (device/v4/demo/src/freertos/esp32_os_impl.c).
 * File persistence is implemented on top of NVS: each logical SDK file is
 * stored as a single blob ("key") inside an NVS namespace derived from the
 * file path.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "wxvoip_os_impl.h"

static const char *TAG = "voip_os";

static void *voip_os_malloc(size_t size)
{
    void *p = malloc(size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

static void voip_os_free(void *ptr)
{
    if (ptr) {
        free(ptr);
    }
}

static void voip_os_print(const char *fmt, va_list ap)
{
    vprintf(fmt, ap);
    fflush(stdout);
}

static uint32_t voip_os_gettime_ms(void)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static uint32_t voip_os_gettime_second(void)
{
    return (uint32_t)time(NULL);
}

static void voip_os_msleep(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void voip_os_getmac(uint8_t *mac, size_t mac_len)
{
    uint8_t m[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, m) != ESP_OK) {
        /* Fall back to the factory eFuse base MAC if WiFi isn't started yet. */
        esp_read_mac(m, ESP_MAC_WIFI_STA);
    }
    size_t n = mac_len < sizeof(m) ? mac_len : sizeof(m);
    memcpy(mac, m, n);
}

static size_t voip_os_get_disksize(void)
{
    return 1024; /* kB; informational only */
}

static size_t voip_os_get_memsize(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_DEFAULT) / 1024; /* kB */
}

static const char *voip_os_get_platform(void)
{
    return "freertos";
}

/* Derive a (<=15 char) NVS namespace from the trailing segment of a path. */
static void path_to_namespace(const char *path, char *ns, size_t ns_size)
{
    size_t j = 0;
    for (int i = (int)strlen(path) - 1; i >= 0 && j < ns_size - 1; i--) {
        if (path[i] == '/' || path[i] == '\\') {
            break;
        }
        ns[j++] = path[i];
    }
    ns[j] = '\0';
}

static int voip_os_create_file(const char *path)
{
    char ns[16] = {0}; /* NVS namespace max length is 15 chars */
    path_to_namespace(path, ns, sizeof(ns));

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", ns, esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "create file %s -> handle %d", ns, (int)handle);
    return (int)handle;
}

static size_t voip_os_read_file(int fd, void *buf, size_t count)
{
    size_t len = count;
    esp_err_t err = nvs_get_blob((nvs_handle_t)fd, "key", buf, &len);
    if (err == ESP_OK) {
        return len;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static size_t voip_os_write_file(int fd, const void *buf, size_t count)
{
    esp_err_t err = nvs_set_blob((nvs_handle_t)fd, "key", buf, count);
    if (err == ESP_OK && nvs_commit((nvs_handle_t)fd) == ESP_OK) {
        return count;
    }
    ESP_LOGE(TAG, "nvs_set_blob failed: %s", esp_err_to_name(err));
    return 0;
}

static int voip_os_close_file(int fd)
{
    nvs_close((nvs_handle_t)fd);
    return 0;
}

static long int voip_os_random(void)
{
    return (long int)esp_random();
}

wxvoip_os_impl_t voip_os_impl = {
    .os_malloc = voip_os_malloc,
    .os_free = voip_os_free,
    .os_printf = voip_os_print,
    .os_gettime_ms = voip_os_gettime_ms,
    .os_gettime_second = voip_os_gettime_second,
    .os_msleep = voip_os_msleep,
    .os_getmac = voip_os_getmac,
    .os_get_disksize = voip_os_get_disksize,
    .os_get_memsize = voip_os_get_memsize,
    .os_get_platform = voip_os_get_platform,
    .os_create_file = voip_os_create_file,
    .os_read_file = voip_os_read_file,
    .os_write_file = voip_os_write_file,
    .os_close_file = voip_os_close_file,
    .os_random = voip_os_random,
};
