#include <stdio.h>
#include <string.h>
#include <ctype.h>
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
#include "wifi_manager.h"
#include "dialogue.h"

static const char *TAG = "wifi_manager";

static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

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
"        <form action=\"/config\" method=\"POST\">\n"
"            <div class=\"form-group\">\n"
"                <label for=\"ssid\">WiFi 网络名称 (SSID)</label>\n"
"                <input type=\"text\" id=\"ssid\" name=\"ssid\" placeholder=\"请输入 WiFi 名称\" required autocomplete=\"off\">\n"
"            </div>\n"
"            <div class=\"form-group\">\n"
"                <label for=\"pwd\">WiFi 密码 (Password)</label>\n"
"                <input type=\"password\" id=\"pwd\" name=\"pwd\" placeholder=\"请输入密码 (无密码请留空)\" autocomplete=\"off\">\n"
"            </div>\n"
"            <button type=\"submit\">连接设备</button>\n"
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

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 5) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP (%d/5)", s_retry_num);
        } else {
            ESP_LOGE(TAG, "connect to the AP fail after max retries");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_manager_init_common(void)
{
    if (s_wifi_inited) {
        return;
    }
    s_wifi_event_group = xEventGroupCreate();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

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
    s_wifi_inited = true;
}

static void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
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

static void restart_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Rebooting device...");
    esp_restart();
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Content too long");
        return ESP_FAIL;
    }

    int pos = 0;
    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf + pos, remaining)) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        remaining -= ret;
        pos += ret;
    }
    buf[pos] = '\0';

    char raw_ssid[64] = {0};
    char raw_pwd[64] = {0};

    char *p = strtok(buf, "&");
    while (p != NULL) {
        if (strncmp(p, "ssid=", 5) == 0) {
            url_decode(raw_ssid, p + 5);
        } else if (strncmp(p, "pwd=", 4) == 0) {
            url_decode(raw_pwd, p + 4);
        }
        p = strtok(NULL, "&");
    }

    ESP_LOGI(TAG, "Received WiFi Configuration: SSID='%s' Password='%s'", raw_ssid, raw_pwd);

    nvs_handle_t my_handle;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_str(my_handle, "ssid", raw_ssid);
        nvs_set_str(my_handle, "pwd", raw_pwd);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "WiFi config saved to NVS.");
    } else {
        ESP_LOGE(TAG, "Failed to open NVS to save WiFi config.");
    }

    httpd_resp_send(req, success_html, HTTPD_RESP_USE_STRLEN);

    TimerHandle_t xTimer = xTimerCreate("RebootTimer", pdMS_TO_TICKS(2000), pdFALSE, NULL, restart_timer_callback);
    if (xTimer != NULL) {
        xTimerStart(xTimer, 0);
    }

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

    wifi_manager_init_common();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (s_web_server == NULL) {
        s_web_server = start_web_server();
    }

    if (s_dns_task_handle == NULL) {
        xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &s_dns_task_handle);
    }
}

esp_err_t wifi_manager_join_sta(const char* ssid, const char* pass)
{
    wifi_manager_init_common();
    
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

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
        play_local_voice(PATH_V_NET_OK, TEXT_V_NET_OK);
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
        play_local_voice(PATH_V_NET_ERR, TEXT_V_NET_ERR);
        return ESP_FAIL;
    }
    return ESP_FAIL;
}

static void wifi_autoconnect_task(void *pvParameters)
{
    play_local_voice(PATH_V_SYS_START, TEXT_V_SYS_START);

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
