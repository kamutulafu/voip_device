#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_http_server.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <sys/time.h>
#include <time.h>
#include "wifi_manager.h"
#include "dialogue.h"
#include "api_service.h"
#include "device_config.h"
#include "voice_flow.h"
#include "audio_driver.h"
#include "app_time.h"
#include "time_sync.h"

static const char *TAG = "wifi_manager";

static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

/* NVS 存储位置 */
#define WIFI_NVS_NAMESPACE  "wifi_cfg"
#define WIFI_NVS_KEY_SSID   "ssid"
#define WIFI_NVS_KEY_PWD    "pwd"

/* 802.11 规定 SSID 最长 32 字节、PSK 最长 63 字节，缓冲区必须再留 1 字节给 '\0'，
 * 否则 nvs_get_str() 会返回 ESP_ERR_NVS_INVALID_LENGTH，导致已保存的配置被误判为"无配置" */
#define WIFI_SSID_BUF_LEN   (32 + 1)
#define WIFI_PWD_BUF_LEN    (64 + 1)

/* STA 连接等待上限，超时后回落到 AP 配网模式，避免任务永久阻塞 */
#define WIFI_CONNECT_TIMEOUT_MS  (40 * 1000)

/* 仅在主动发起 STA 连接期间才允许事件回调自动重连 */
static volatile bool s_sta_connecting = false;

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_wifi_inited = false;

static httpd_handle_t s_web_server = NULL;
static TaskHandle_t s_dns_task_handle = NULL;

#define DNS_PORT 53

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static const char *setup_html = 
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <meta charset=\"utf-8\">\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"    <title>童小盾 WiFi 设置</title>\n"
"    <style>\n"
"        * { box-sizing: border-box; margin: 0; padding: 0; }\n"
"        body {\n"
"            font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, sans-serif;\n"
"            background: linear-gradient(135deg, #0f172a, #1e1b4b);\n"
"            color: #f8fafc;\n"
"            min-height: 100vh;\n"
"            display: flex;\n"
"            align-items: center;\n"
"            justify-content: center;\n"
"            padding: 20px;\n"
"        }\n"
"        .container {\n"
"            width: 100%;\n"
"            max-width: 400px;\n"
"            background: rgba(255, 255, 255, 0.05);\n"
"            backdrop-filter: blur(16px);\n"
"            -webkit-backdrop-filter: blur(16px);\n"
"            border: 1px solid rgba(255, 255, 255, 0.1);\n"
"            border-radius: 24px;\n"
"            padding: 32px;\n"
"            box-shadow: 0 20px 40px rgba(0, 0, 0, 0.3);\n"
"            text-align: center;\n"
"        }\n"
"        .logo {\n"
"            font-size: 28px;\n"
"            font-weight: 800;\n"
"            background: linear-gradient(135deg, #38bdf8, #818cf8);\n"
"            -webkit-background-clip: text;\n"
"            -webkit-text-fill-color: transparent;\n"
"            margin-bottom: 8px;\n"
"        }\n"
"        .subtitle {\n"
"            font-size: 14px;\n"
"            color: #94a3b8;\n"
"            margin-bottom: 32px;\n"
"        }\n"
"        .form-group {\n"
"            margin-bottom: 20px;\n"
"            text-align: left;\n"
"        }\n"
"        label {\n"
"            display: block;\n"
"            font-size: 12px;\n"
"            font-weight: 600;\n"
"            color: #cbd5e1;\n"
"            margin-bottom: 8px;\n"
"            text-transform: uppercase;\n"
"            letter-spacing: 0.05em;\n"
"        }\n"
"        input {\n"
"            width: 100%;\n"
"            background: rgba(255, 255, 255, 0.08);\n"
"            border: 1px solid rgba(255, 255, 255, 0.1);\n"
"            border-radius: 12px;\n"
"            padding: 14px 16px;\n"
"            font-size: 16px;\n"
"            color: #fff;\n"
"            transition: all 0.3s ease;\n"
"        }\n"
"        input:focus {\n"
"            outline: none;\n"
"            border-color: #38bdf8;\n"
"            background: rgba(255, 255, 255, 0.12);\n"
"            box-shadow: 0 0 0 4px rgba(56, 189, 248, 0.15);\n"
"        }\n"
"        button {\n"
"            width: 100%;\n"
"            background: linear-gradient(135deg, #0284c7, #4f46e5);\n"
"            border: none;\n"
"            border-radius: 12px;\n"
"            padding: 14px;\n"
"            font-size: 16px;\n"
"            font-weight: 700;\n"
"            color: #fff;\n"
"            cursor: pointer;\n"
"            transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);\n"
"            box-shadow: 0 4px 12px rgba(79, 70, 229, 0.3);\n"
"            margin-top: 12px;\n"
"        }\n"
"        button:hover {\n"
"            transform: translateY(-2px);\n"
"            box-shadow: 0 6px 20px rgba(79, 70, 229, 0.4);\n"
"            filter: brightness(1.1);\n"
"        }\n"
"        button:active {\n"
"            transform: translateY(0);\n"
"        }\n"
"        .footer {\n"
"            margin-top: 24px;\n"
"            font-size: 11px;\n"
"            color: #64748b;\n"
"        }\n"
"    </style>\n"
"</head>\n"
"<body>\n"
"    <div class=\"container\">\n"
"        <div class=\"logo\">童小盾</div>\n"
"        <div class=\"subtitle\">配置设备 WiFi 连接</div>\n"
"        <form id=\"cfgForm\" action=\"/config\" method=\"POST\">\n"
"            <div class=\"form-group\">\n"
"                <label for=\"ssid\">WiFi 网络名称 (SSID)</label>\n"
"                <input type=\"text\" id=\"ssid\" name=\"ssid\" placeholder=\"请输入 WiFi 名称\" required autocomplete=\"off\">\n"
"            </div>\n"
"            <div class=\"form-group\">\n"
"                <label for=\"pwd\">WiFi 密码 (Password)</label>\n"
"                <input type=\"password\" id=\"pwd\" name=\"pwd\" placeholder=\"请输入密码 (无密码请留空)\" autocomplete=\"off\">\n"
"            </div>\n"
"            <div class=\"form-group\">\n"
"                <label for=\"volume\">喇叭初始音量 (0-100%)</label>\n"
"                <input type=\"number\" id=\"volume\" name=\"volume\" min=\"0\" max=\"100\" placeholder=\"默认 55%\">\n"
"            </div>\n"
"            <button type=\"submit\">保存并连接</button>\n"
"        </form>\n"
"        <div class=\"footer\">童小盾 系统 &copy; 2026</div>\n"
"    </div>\n"
"</body>\n"
"</html>";

static const char *success_html =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <meta charset=\"utf-8\">\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"    <title>配置已保存</title>\n"
"    <style>\n"
"        body {\n"
"            font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, sans-serif;\n"
"            background: linear-gradient(135deg, #0f172a, #1e1b4b);\n"
"            color: #f8fafc;\n"
"            min-height: 100vh;\n"
"            display: flex;\n"
"            align-items: center;\n"
"            justify-content: center;\n"
"            padding: 20px;\n"
"            text-align: center;\n"
"        }\n"
"        .container {\n"
"            background: rgba(255, 255, 255, 0.05);\n"
"            backdrop-filter: blur(16px);\n"
"            -webkit-backdrop-filter: blur(16px);\n"
"            border: 1px solid rgba(255, 255, 255, 0.1);\n"
"            border-radius: 24px;\n"
"            padding: 40px;\n"
"            max-width: 400px;\n"
"            box-shadow: 0 20px 40px rgba(0,0,0,0.3);\n"
"        }\n"
"        .icon {\n"
"            font-size: 48px;\n"
"            color: #10b981;\n"
"            margin-bottom: 20px;\n"
"        }\n"
"        h1 { font-size: 24px; font-weight: 700; margin-bottom: 12px; }\n"
"        p { color: #94a3b8; font-size: 15px; line-height: 1.6; }\n"
"    </style>\n"
"</head>\n"
"<body>\n"
"    <div class=\"container\">\n"
"        <div class=\"icon\">✓</div>\n"
"        <h1>配置保存成功</h1>\n"
"        <p>设备正在重启以连接您的 WiFi 网络。<br>您现在可以关闭此窗口。</p>\n"
"    </div>\n"
"</body>\n"
"</html>";

static const char *failure_html =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <meta charset=\"utf-8\">\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"    <title>配置保存失败</title>\n"
"    <style>\n"
"        body {\n"
"            font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, sans-serif;\n"
"            background: linear-gradient(135deg, #0f172a, #1e1b4b);\n"
"            color: #f8fafc;\n"
"            min-height: 100vh;\n"
"            display: flex;\n"
"            align-items: center;\n"
"            justify-content: center;\n"
"            padding: 20px;\n"
"            text-align: center;\n"
"        }\n"
"        .container {\n"
"            background: rgba(255, 255, 255, 0.05);\n"
"            backdrop-filter: blur(16px);\n"
"            -webkit-backdrop-filter: blur(16px);\n"
"            border: 1px solid rgba(255, 255, 255, 0.1);\n"
"            border-radius: 24px;\n"
"            padding: 40px;\n"
"            max-width: 400px;\n"
"            box-shadow: 0 20px 40px rgba(0,0,0,0.3);\n"
"        }\n"
"        .icon { font-size: 48px; color: #f87171; margin-bottom: 20px; }\n"
"        h1 { font-size: 24px; font-weight: 700; margin-bottom: 12px; }\n"
"        p { color: #94a3b8; font-size: 15px; line-height: 1.6; }\n"
"        a { color: #38bdf8; text-decoration: none; font-weight: 600; }\n"
"    </style>\n"
"</head>\n"
"<body>\n"
"    <div class=\"container\">\n"
"        <div class=\"icon\">✕</div>\n"
"        <h1>配置保存失败</h1>\n"
"        <p>设备未能保存 WiFi 信息，请确认 WiFi 名称不超过 32 个字节、密码不超过 64 个字节后重试。</p>\n"
"        <p style=\"margin-top:16px\"><a href=\"/\">返回重新填写</a></p>\n"
"    </div>\n"
"</body>\n"
"</html>";

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_sta_connecting) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        /* AP 配网模式下(或已放弃连接后)收到的残留断开事件必须忽略，
         * 否则会误增重试计数并向已停止的 STA 发起连接 */
        if (!s_sta_connecting) {
            return;
        }
        if (s_retry_num < 5) {
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP (%d/5), reason=%d",
                     s_retry_num, disc ? disc->reason : -1);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "connect to the AP fail after max retries, reason=%d",
                     disc ? disc->reason : -1);
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

static esp_err_t wifi_manager_init_common(void)
{
    if (s_wifi_inited) {
        return ESP_OK;
    }

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create WiFi event group");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        /* ESP32-P4 通过 ESP-Hosted(SDIO) 使用 C6 射频，硬件/从机固件异常时会失败。
         * 这里不再 ESP_ERROR_CHECK 直接 abort，避免新板首次上电陷入重启死循环 */
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

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

    /* 只使用本地 NVS(wifi_cfg) 作为唯一配置源；禁止 esp_wifi 把 AP/STA 配置
     * 持久化到从机 flash，避免重启后残留旧的 AP 配置干扰 STA 连接 */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    s_wifi_inited = true;
    return ESP_OK;
}

static void url_decode(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    size_t di = 0;
    char a, b;
    while (*src && di + 1 < dst_size) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            dst[di++] = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            dst[di++] = ' ';
            src++;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

/**
 * @brief 保存 WiFi 凭据到 NVS，并回读校验。
 *        任何一步失败都会返回错误，绝不谎报成功。
 */
static esp_err_t wifi_manager_save_credentials(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass == NULL) {
        pass = "";
    }
    if (strlen(ssid) >= WIFI_SSID_BUF_LEN || strlen(pass) >= WIFI_PWD_BUF_LEN) {
        ESP_LOGE(TAG, "SSID/password too long (ssid=%u bytes, pwd=%u bytes)",
                 (unsigned)strlen(ssid), (unsigned)strlen(pass));
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", WIFI_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_NVS_KEY_PWD, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write WiFi config to NVS: %s", esp_err_to_name(err));
        return err;
    }

    /* 回读校验，确保重启后一定能取到刚才写入的内容 */
    char chk_ssid[WIFI_SSID_BUF_LEN] = {0};
    size_t chk_len = sizeof(chk_ssid);
    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, WIFI_NVS_KEY_SSID, chk_ssid, &chk_len);
        nvs_close(handle);
    }
    if (err != ESP_OK || strcmp(chk_ssid, ssid) != 0) {
        ESP_LOGE(TAG, "WiFi config verification failed (err=%s, readback='%s')",
                 esp_err_to_name(err), chk_ssid);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    ESP_LOGI(TAG, "WiFi config saved & verified in NVS: SSID='%s'", ssid);
    return ESP_OK;
}

/**
 * @brief 从 NVS 读取 WiFi 凭据。密码键缺失时按空密码处理。
 */
static esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_size,
                                               char *pass, size_t pass_size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "nvs_open(%s) for read: %s", WIFI_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    size_t len = ssid_size;
    err = nvs_get_str(handle, WIFI_NVS_KEY_SSID, ssid, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read saved SSID failed: %s (need %u bytes, buffer %u)",
                 esp_err_to_name(err), (unsigned)len, (unsigned)ssid_size);
        nvs_close(handle);
        return err;
    }

    len = pass_size;
    esp_err_t pwd_err = nvs_get_str(handle, WIFI_NVS_KEY_PWD, pass, &len);
    if (pwd_err != ESP_OK) {
        ESP_LOGW(TAG, "Read saved password failed: %s, assuming open network",
                 esp_err_to_name(pwd_err));
        pass[0] = '\0';
    }

    nvs_close(handle);
    return ESP_OK;
}

static void dns_server_task(void *pvParameters)
{
    char rx_buffer[128];
    char tx_buffer[256];
    struct sockaddr_in server_addr;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(DNS_PORT);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create DNS socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS Socket unable to bind: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server started on port %d", DNS_PORT);

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

        if (len < sizeof(dns_header_t)) {
            if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            }
            continue;
        }

        dns_header_t *qd_header = (dns_header_t *)rx_buffer;
        
        // Only process queries
        if ((ntohs(qd_header->flags) & 0x8000) != 0) {
            continue;
        }

        // We only respond to standard queries with at least one question
        if (qd_header->qdcount == 0) {
            continue;
        }

        // Prepare response
        dns_header_t *ans_header = (dns_header_t *)tx_buffer;
        ans_header->id = qd_header->id;
        ans_header->flags = htons(0x8180); // Response, Standard query, Recursion desired/available, No error
        ans_header->qdcount = qd_header->qdcount;
        ans_header->ancount = htons(1);
        ans_header->nscount = 0;
        ans_header->arcount = 0;

        // Copy question section
        int cur_pos = sizeof(dns_header_t);
        // Copy the domain labels
        while (cur_pos < len && rx_buffer[cur_pos] != 0) {
            tx_buffer[cur_pos] = rx_buffer[cur_pos];
            cur_pos++;
        }
        if (cur_pos + 5 > len) {
            // Malformed question
            continue;
        }
        tx_buffer[cur_pos] = 0; // null terminator for name
        cur_pos++;
        
        // Copy QTYPE and QCLASS
        tx_buffer[cur_pos] = rx_buffer[cur_pos];
        tx_buffer[cur_pos + 1] = rx_buffer[cur_pos + 1];
        tx_buffer[cur_pos + 2] = rx_buffer[cur_pos + 2];
        tx_buffer[cur_pos + 3] = rx_buffer[cur_pos + 3];
        cur_pos += 4;

        // Add Answer section
        // 1. Name offset: 0xc00c (compression pointer to offset 12 in DNS frame, which is the start of the query name)
        tx_buffer[cur_pos] = 0xc0;
        tx_buffer[cur_pos + 1] = 0x0c;
        cur_pos += 2;

        // 2. Type: A record (0x0001)
        uint16_t type = htons(1);
        memcpy(tx_buffer + cur_pos, &type, 2);
        cur_pos += 2;

        // 3. Class: IN (0x0001)
        uint16_t class = htons(1);
        memcpy(tx_buffer + cur_pos, &class, 2);
        cur_pos += 2;

        // 4. TTL: 60 seconds (0x0000003c)
        uint32_t ttl = htonl(60);
        memcpy(tx_buffer + cur_pos, &ttl, 4);
        cur_pos += 4;

        // 5. RDLENGTH: 4 bytes (0x0004)
        uint16_t rdlength = htons(4);
        memcpy(tx_buffer + cur_pos, &rdlength, 2);
        cur_pos += 2;

        // 6. RDATA: IP address 192.168.4.1
        tx_buffer[cur_pos] = 192;
        tx_buffer[cur_pos + 1] = 168;
        tx_buffer[cur_pos + 2] = 4;
        tx_buffer[cur_pos + 3] = 1;
        cur_pos += 4;

        sendto(sock, tx_buffer, cur_pos, 0, (struct sockaddr *)&source_addr, socklen);
    }

    close(sock);
    vTaskDelete(NULL);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char host[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
        if (strstr(host, "192.168.4.1") == NULL) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
    }
    httpd_resp_send(req, setup_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void restart_task(void *arg)
{
    /* 留出时间把成功页面发回手机并关闭连接 */
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Rebooting device to apply the new WiFi configuration...");
    esp_restart();
}

static void schedule_restart(void)
{
    if (xTaskCreate(restart_task, "wifi_reboot", 3072, NULL, 5, NULL) != pdPASS) {
        /* 独立任务都创建不出来时，宁可立刻重启，也不能停在 AP 模式让用户以为已生效 */
        ESP_LOGE(TAG, "Failed to create restart task, restarting immediately");
        esp_restart();
    }
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    /* SSID/密码经 URL 编码后可能显著变长(如中文 SSID 每字符 9 字节)，缓冲区放宽到 512 */
    char buf[512];
    int ret, remaining = req->content_len;

    if (remaining <= 0 || remaining >= (int)sizeof(buf)) {
        ESP_LOGE(TAG, "Invalid POST content_len=%d", remaining);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form data");
        return ESP_FAIL;
    }

    int pos = 0;
    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf + pos, remaining)) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "httpd_req_recv failed: %d", ret);
            return ESP_FAIL;
        }
        remaining -= ret;
        pos += ret;
    }
    buf[pos] = '\0';

    char raw_ssid[WIFI_SSID_BUF_LEN] = {0};
    char raw_pwd[WIFI_PWD_BUF_LEN] = {0};
    int vol_val = -1;

    char *save_ptr = NULL;
    char *p = strtok_r(buf, "&", &save_ptr);
    while (p != NULL) {
        if (strncmp(p, "ssid=", 5) == 0) {
            url_decode(raw_ssid, sizeof(raw_ssid), p + 5);
        } else if (strncmp(p, "pwd=", 4) == 0) {
            url_decode(raw_pwd, sizeof(raw_pwd), p + 4);
        } else if (strncmp(p, "volume=", 7) == 0) {
            if (p[7] != '\0') {
                vol_val = atoi(p + 7);
            }
        }
        p = strtok_r(NULL, "&", &save_ptr);
    }

    if (vol_val >= 0 && vol_val <= 100) {
        /* 走 audio_store_volume(): 配网时音频通路通常还没起来，
         * 这里只需把用户的选择落到 NVS，重启后 audio_init() 自然会用上，
         * 不能因为 ES8311 还没初始化就把设置丢掉。 */
        esp_err_t verr = audio_store_volume((uint8_t)vol_val);
        if (verr == ESP_OK) {
            ESP_LOGI(TAG, "AP Mode: Speaker initial volume set to %d%% and saved to NVS", vol_val);
        } else {
            ESP_LOGE(TAG, "AP Mode: Failed to save volume %d%%: %s", vol_val, esp_err_to_name(verr));
        }
    }

    ESP_LOGI(TAG, "Received WiFi Configuration: SSID='%s' (%u bytes), password length=%u",
             raw_ssid, (unsigned)strlen(raw_ssid), (unsigned)strlen(raw_pwd));

    esp_err_t err = wifi_manager_save_credentials(raw_ssid, raw_pwd);
    if (err != ESP_OK) {
        /* 保存失败必须如实告知，不能再返回"配置保存成功" */
        ESP_LOGE(TAG, "Provisioning failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, failure_html, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_send(req, success_html, HTTPD_RESP_USE_STRLEN);
    schedule_restart();
    return ESP_OK;
}

static httpd_handle_t start_web_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_get = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &root_get);

        httpd_uri_t config_post = {
            .uri       = "/config",
            .method    = HTTP_POST,
            .handler   = config_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &config_post);

        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);
        
        return server;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return NULL;
}

void wifi_manager_start_ap(void)
{
    ESP_LOGI(TAG, "Starting WiFi Provisioning AP Mode...");

    if (wifi_manager_init_common() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi stack not available, cannot start provisioning AP");
        return;
    }

    /* 若是 STA 连接失败后回落进来，先彻底停掉 STA，避免残留重连干扰 SoftAP */
    s_sta_connecting = false;
    esp_wifi_disconnect();
    esp_wifi_stop();

    /* ESP32-P4 本身没有 WiFi 射频，射频在 ESP-Hosted 的 C6 从机上，
     * 因此 esp_read_mac(ESP_MAC_WIFI_STA) 会报 "0 mac type is incorrect (not found)"，
     * 且不会写入 mac[]，热点名会退化成固定的 TongXiaoDun_0100(读到未初始化数据)，
     * 多台设备撞名。改用本机 efuse 出厂 MAC，它在 P4 上恒可用且每颗芯片唯一，
     * 也不依赖 WiFi 的初始化/模式状态。 */
    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    if (mac_err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to read a MAC address for the AP SSID: %s", esp_err_to_name(mac_err));
    }
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "TongXiaoDun_%02X%02X", mac[4], mac[5]);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(ap_ssid),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };
    memcpy(wifi_config.ap.ssid, ap_ssid, strlen(ap_ssid));

    ESP_LOGI(TAG, "Configuring SoftAP SSID: %s", ap_ssid);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start SoftAP: %s", esp_err_to_name(err));
        return;
    }

    if (s_web_server == NULL) {
        s_web_server = start_web_server();
    }

    if (s_dns_task_handle == NULL) {
        xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &s_dns_task_handle);
    }
}

static void fetch_device_config_task(void *arg)
{
    /* 稍作等待 (例如 500ms)，让网络层进一步平稳后再发起配置获取 */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Fetching device config automatically after internet connection...");
    api_result_t *res = api_get_device_config(DEVICE_ID, 1);
    if (res) {
        if (res->http_status == 200) {
            ESP_LOGI(TAG, "Device config fetched successfully! Updated useType = %d (%s)",
                     get_device_use_type(), get_device_use_type() == 1 ? "校内版" : "校外版");
        } else {
            ESP_LOGW(TAG, "Device config fetch returned HTTP status %d", res->http_status);
        }
        api_result_free(res);
    } else {
        ESP_LOGE(TAG, "Failed to fetch device config");
    }
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_join_sta(const char* ssid, const char* pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass == NULL) {
        pass = "";
    }
    if (wifi_manager_init_common() != ESP_OK) {
        return ESP_FAIL;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

    /* 先停掉旧状态(可能处于 AP 配网模式)，再清零计数/事件位，
     * 否则 stop 过程中产生的断开事件会吃掉重试次数 */
    s_sta_connecting = false;
    esp_wifi_disconnect();
    esp_wifi_stop();

    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure STA: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    s_sta_connecting = true;   /* 必须在 esp_wifi_start() 之前置位，STA_START 事件才会发起连接 */
    err = esp_wifi_start();
    if (err != ESP_OK) {
        s_sta_connecting = false;
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s. Waiting for internet connectivity...", ssid);

        // Wait for actual internet connectivity by resolving the backend host
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = NULL;
        bool internet_ok = false;

        for (int retry = 0; retry < 15; retry++) {
            int err = getaddrinfo("gateway.tdskynet.com", NULL, &hints, &res);
            if (err == 0) {
                freeaddrinfo(res);
                internet_ok = true;
                break;
            }
            ESP_LOGI(TAG, "Internet not ready yet (getaddrinfo returned %d), retrying in 1s...", err);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (internet_ok) {
            ESP_LOGI(TAG, "Internet connectivity verified successfully! Network is fully accessible.");
            time_sync_start(); // 联网确认可用后，异步"蹭"一次HTTP Date响应头校准系统时间
            xTaskCreate(fetch_device_config_task, "fetch_dev_cfg", 8192, NULL, 5, NULL); // 自动异步获取设备配置更新 use_type (HTTPS+RSA加密需8KB栈空间)
            play_local_voice(PATH_V_NET_OK, TEXT_V_NET_OK);
        } else {
            ESP_LOGE(TAG, "Failed to verify internet connectivity (DNS resolution timed out).");
            play_local_voice(PATH_V_NET_ERR, TEXT_V_NET_ERR);
            return ESP_FAIL;
        }

        if (wifi_manager_save_credentials(ssid, pass) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist WiFi credentials to NVS");
        }
        return ESP_OK;
    }

    s_sta_connecting = false;
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID:%s (check SSID/password)", ssid);
    } else {
        ESP_LOGE(TAG, "Connecting to SSID:%s timed out after %d ms", ssid, WIFI_CONNECT_TIMEOUT_MS);
    }
    play_local_voice(PATH_V_NET_ERR, TEXT_V_NET_ERR);
    return ESP_FAIL;
}

static void wifi_autoconnect_task(void *pvParameters)
{
    play_local_voice(PATH_V_SYS_START, TEXT_V_SYS_START);

    char ssid[WIFI_SSID_BUF_LEN] = {0};
    char password[WIFI_PWD_BUF_LEN] = {0};

    esp_err_t err = wifi_manager_load_credentials(ssid, sizeof(ssid),
                                                  password, sizeof(password));

    if (err == ESP_OK && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Auto-connecting to saved WiFi: SSID=%s", ssid);
        if (wifi_manager_join_sta(ssid, password) != ESP_OK) {
            ESP_LOGW(TAG, "Auto-connect failed. Entering AP provisioning mode.");
            wifi_manager_start_ap();
        }
    } else {
        ESP_LOGI(TAG, "No saved WiFi config found. Entering AP provisioning mode.");
        play_local_voice(PATH_V_NET_ERR, TEXT_V_NET_ERR);
        wifi_manager_start_ap();
    }
    vTaskDelete(NULL);
}

void wifi_manager_start_autoconnect(void)
{
    xTaskCreate(wifi_autoconnect_task, "wifi_autoconnect", 4096, NULL, 5, NULL);
}

bool wifi_manager_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
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
