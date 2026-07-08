/*
 * WeChat Cloud VoIP SDK - application glue + console test command.
 *
 * Wires the OS/HTTPS adaptation layers (voip_os_impl.c / voip_https_impl.c)
 * into the SDK and drives the init -> register -> call flow.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include "voip_client.h"
#include "voip_media.h"
#include "device_config.h"

static const char *TAG = "voip_client";

/* Backend that issues snTickets. */
#define VOIP_BACKEND_HOST       "gateway.tdskynet.com"
#define VOIP_SNTICKET_PATH      "/wechat-service/api/device/getWecooperSnTicket"

static bool s_initialized = false;

wx_error_t voip_client_call(const char *device_id, const char *model_id,
                            const char *wxa_appid, const char *sn_ticket,
                            const char *openid, const char *payload, bool video)
{
    if (!device_id || !model_id || !wxa_appid || !sn_ticket || !openid) {
        return WXERROR_INVALID_ARGUMENT;
    }

    wx_cloudvoip_config_t config = {
        .data_dir   = "/data",          /* logical dir; OS layer maps to NVS */
        .device_id  = (char *)device_id,
        .model_id   = (char *)model_id,
        .wxa_appid  = (char *)wxa_appid,
        .wxa_flavor = WX_WXA_FLAVOR_RELEASE,
        .log_level  = WX_VOIPSDK_DEBUG_INFO,
    };

    /* Network send/recv scratch buffer (enlarge if payload is very large). */
    voip_network_stack.buffer_size = 2048;

    wx_error_t ret = wx_init(&voip_network_stack, &voip_os_impl, &config);
    if (ret != WXERROR_OK) {
        ESP_LOGE(TAG, "wx_init failed: %d", ret);
        return ret;
    }
    s_initialized = true;

    int is_registered = 0;
    ret = wx_device_is_registered(&is_registered);
    if (ret != WXERROR_OK) {
        ESP_LOGE(TAG, "wx_device_is_registered failed: %d", ret);
        goto out;
    }

    if (!is_registered) {
        ESP_LOGI(TAG, "Device not registered, registering with snTicket...");
        ret = wx_device_register(sn_ticket);
        if (ret != WXERROR_OK) {
            ESP_LOGE(TAG, "wx_device_register failed: %d", ret);
            goto out;
        }
        ESP_LOGI(TAG, "Device registered successfully");
    } else {
        ESP_LOGI(TAG, "Device already registered");
    }

    wx_cloudvoip_member_camera_status_t cam =
        video ? WX_CLOUDVOIP_MEMBER_CAMERA_STATUS_OPEN
              : WX_CLOUDVOIP_MEMBER_CAMERA_STATUS_CLOSE;

    /* For IoT VoIP mode the caller name must be an empty string; the WeChat
     * backend substitutes the real device name. */
    wx_cloudvoip_member_t caller = {
        .name = "",
        .id = device_id,
        .camera_status = cam,
    };

    wx_cloudvoip_member_t callee = {
        .name = "wechat_user",
        .id = openid,
        .camera_status = cam,
    };

    wx_cloudvoip_session_type_t room_type =
        video ? WX_CLOUDVOIP_SESSION_VIDEO : WX_CLOUDVOIP_SESSION_AUDIO;

    ret = wx_cloudvoip_client_call(room_type, &caller, &callee, "",
                                   payload ? payload : "");
    ESP_LOGI(TAG, "wx_cloudvoip_client_call returned %d", ret);

    if (ret == WXERROR_OK) {
        /* Call request accepted. Read the token the SDK persisted, report it
         * to the cloud server (so it joins the WeChat room) and start media
         * push. Keep the SDK initialized for the duration of the call. */
        voip_media_on_call_connected(payload ? payload : "");
        return WXERROR_OK;
    }

out:
    wx_destory();
    s_initialized = false;
    return ret;
}

/* ---- snTicket fetch over HTTPS ---------------------------------------- */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        http_resp_t *r = (http_resp_t *)evt->user_data;
        if (r) {
            if (r->len + evt->data_len + 1 > r->cap) {
                size_t new_cap = r->len + evt->data_len + 1;
                char *tmp = realloc(r->buf, new_cap);
                if (!tmp) {
                    return ESP_ERR_NO_MEM;
                }
                r->buf = tmp;
                r->cap = new_cap;
            }
            memcpy(r->buf + r->len, evt->data, evt->data_len);
            r->len += evt->data_len;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

/* Extract a snTicket from a JSON response, tolerating several shapes:
 *   {"snTicket":"x"} / {"ticket":"x"} / {"data":{"snTicket":"x"}} / {"data":"x"} */
static bool extract_sn_ticket(const char *json, char *out, size_t out_size)
{
    static const char *const keys[] = {"snTicket", "sn_ticket", "ticket"};

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }

    bool found = false;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && !found; i++) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
        if (cJSON_IsString(item) && item->valuestring) {
            snprintf(out, out_size, "%s", item->valuestring);
            found = true;
        }
    }

    if (!found) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsString(data) && data->valuestring) {
            snprintf(out, out_size, "%s", data->valuestring);
            found = true;
        } else if (cJSON_IsObject(data)) {
            for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && !found; i++) {
                cJSON *item = cJSON_GetObjectItemCaseSensitive(data, keys[i]);
                if (cJSON_IsString(item) && item->valuestring) {
                    snprintf(out, out_size, "%s", item->valuestring);
                    found = true;
                }
            }
        }
    }

    cJSON_Delete(root);
    return found;
}

wx_error_t voip_fetch_sn_ticket(const char *device_id, const char *sid,
                                char *out, size_t out_size)
{
    if (!device_id || !sid || !out || out_size == 0) {
        return WXERROR_INVALID_ARGUMENT;
    }
    out[0] = '\0';

    char url[512];
    snprintf(url, sizeof(url),
             "https://%s%s?deviceId=%s&sid=%s",
             VOIP_BACKEND_HOST, VOIP_SNTICKET_PATH, device_id, sid);

    http_resp_t resp = {0};

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .event_handler = http_event_handler,
        .user_data = &resp,
        /* Match the reference Python script which uses an unverified TLS
         * context. IDF's esp-tls only allows skipping verification when
         * CONFIG_ESP_TLS_INSECURE + CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY are
         * enabled (see sdkconfig.defaults). Harden this for production. */
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = NULL,
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return WXERROR_RESOURCE_EXHAUSTED;
    }

    wx_error_t result = WXERROR_OK;
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "snTicket request failed: %s", esp_err_to_name(err));
        result = WXERROR_RESPONSE;
        goto done;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "snTicket HTTP status %d, %u bytes", status, (unsigned)resp.len);
    if (status != 200 || resp.len == 0) {
        result = WXERROR_RESPONSE;
        goto done;
    }

    /* Try JSON; if that fails, treat the whole body as the raw ticket. */
    if (!extract_sn_ticket(resp.buf, out, out_size)) {
        char *s = resp.buf;
        while (*s == ' ' || *s == '\r' || *s == '\n' || *s == '\t') s++;
        snprintf(out, out_size, "%s", s);
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\r' ||
                         out[n - 1] == '\n' || out[n - 1] == '\t')) {
            out[--n] = '\0';
        }
    }

    if (out[0] == '\0') {
        result = WXERROR_RESPONSE;
    }

done:
    esp_http_client_cleanup(client);
    free(resp.buf);
    return result;
}

int cmd_voip_call(int argc, char **argv)
{
    /* All parameters default to the firmware-baked values; any can be
     * overridden positionally. The snTicket is always fetched at runtime. */
    const char *openid    = (argc >= 2) ? argv[1] : VOIP_OPENID;
    const char *device_id = argc > 2 ? argv[2] : DEVICE_ID;
    const char *model_id  = argc > 3 ? argv[3] : VOIP_MODEL_ID;
    const char *appid     = argc > 4 ? argv[4] : VOIP_APPID;
    const char *payload   = argc > 5 ? argv[5] : VOIP_PAYLOAD;

    printf("Fetching snTicket from %s ...\n", VOIP_BACKEND_HOST);
    char sn_ticket[256] = {0};
    wx_error_t ret = voip_fetch_sn_ticket(device_id, model_id, sn_ticket, sizeof(sn_ticket));
    if (ret != WXERROR_OK) {
        printf("Failed to obtain snTicket (wx_error=%d). Check WiFi/backend.\n", ret);
        return 1;
    }
    printf("snTicket obtained: %s\n", sn_ticket);

    printf("Placing VoIP call: device=%s model=%s appid=%s -> openid=%s\n",
           device_id, model_id, appid, openid);

    ret = voip_client_call(device_id, model_id, appid, sn_ticket,
                           openid, payload, true);
    if (ret != WXERROR_OK) {
        printf("VoIP call failed (wx_error=%d). See logs for details.\n", ret);
        return 1;
    }

    printf("VoIP call request sent successfully.\n");
    return 0;
}
