#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_manager";

static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 5) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_join_sta(const char* ssid, const char* pass)
{
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        esp_event_handler_instance_t instance_any_id;
        esp_event_handler_instance_t instance_got_ip;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &event_handler,
                                                            NULL,
                                                            &instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &event_handler,
                                                            NULL,
                                                            &instance_got_ip));
    }
    
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

    esp_wifi_disconnect();
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", ssid, pass);
        // Save WiFi credentials to NVS
        nvs_handle_t my_handle;
        if (nvs_open("wifi_cfg", NVS_READWRITE, &my_handle) == ESP_OK) {
            nvs_set_str(my_handle, "ssid", ssid);
            nvs_set_str(my_handle, "pwd", pass);
            nvs_commit(my_handle);
            nvs_close(my_handle);
            ESP_LOGI(TAG, "WiFi credentials saved successfully to NVS");
        } else {
            ESP_LOGE(TAG, "Failed to open NVS to save WiFi credentials");
        }
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", ssid, pass);
        return ESP_FAIL;
    }
    return ESP_FAIL;
}

static void wifi_autoconnect_task(void *pvParameters)
{
    char ssid[32] = {0};
    char password[64] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pwd_len = sizeof(password);

    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        err = nvs_get_str(my_handle, "ssid", ssid, &ssid_len);
        if (err == ESP_OK) {
            err = nvs_get_str(my_handle, "pwd", password, &pwd_len);
        }
        nvs_close(my_handle);
    }

    if (err == ESP_OK && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Auto-connecting to saved WiFi: SSID=%s", ssid);
        wifi_manager_join_sta(ssid, password);
    } else {
        ESP_LOGI(TAG, "No saved WiFi config found, skipping auto-connect.");
    }
    vTaskDelete(NULL);
}

void wifi_manager_start_autoconnect(void)
{
    xTaskCreate(wifi_autoconnect_task, "wifi_autoconnect", 4096, NULL, 5, NULL);
}

int cmd_wifi_join(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: wifi_join <ssid> <password>\n");
        return 0;
    }
    printf("Connecting to %s...\n", argv[1]);
    wifi_manager_join_sta(argv[1], argv[2]);
    return 0;
}
