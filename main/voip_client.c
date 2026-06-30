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

#include "voip_client.h"

static const char *TAG = "voip_client";

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

out:
    wx_destory();
    s_initialized = false;
    return ret;
}

int cmd_voip_call(int argc, char **argv)
{
    if (argc < 6) {
        printf("Usage: voip_call <device_id> <model_id> <appid> <sn_ticket> <openid> [payload]\n");
        printf("  Places a VoIP video call from this device to a WeChat user (OpenID).\n");
        printf("  Requires an active WiFi connection (use wifi_join first).\n");
        return 1;
    }

    const char *device_id = argv[1];
    const char *model_id  = argv[2];
    const char *appid     = argv[3];
    const char *sn_ticket = argv[4];
    const char *openid    = argv[5];
    const char *payload   = (argc >= 7) ? argv[6] : "";

    printf("Placing VoIP call: device=%s model=%s appid=%s -> openid=%s\n",
           device_id, model_id, appid, openid);

    wx_error_t ret = voip_client_call(device_id, model_id, appid, sn_ticket,
                                      openid, payload, true);
    if (ret != WXERROR_OK) {
        printf("VoIP call failed (wx_error=%d). See logs for details.\n", ret);
        return 1;
    }

    printf("VoIP call request sent successfully.\n");
    return 0;
}
