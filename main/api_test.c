#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_console.h"
#include "api_service.h"
#include "console.h"
#include "api_test.h"
#include "device_config.h"
#include "api_crypto.h"

static const char* TAG = "api_test";
static char current_sid[64] = "test-session-id";
static char current_device_id[64] = DEVICE_ID;

static void print_result(const char* name, api_result_t *res) {
    if (!res) {
        ESP_LOGE(TAG, "%s failed: Network error or no response", name);
        return;
    }
    ESP_LOGI(TAG, "--- %s Result ---", name);
    ESP_LOGI(TAG, "HTTP Status: %d", res->http_status);
    ESP_LOGI(TAG, "Code: %s", res->code ? res->code : "NULL");
    ESP_LOGI(TAG, "Msg: %s", res->msg ? res->msg : "NULL");
    if (res->data) {
        if (cJSON_IsString(res->data)) {
            ESP_LOGI(TAG, "Data is a string (possibly encrypted). Attempting RSA decryption...");
            char *decrypted = api_crypto_rsa_decrypt(res->data->valuestring);
            if (decrypted) {
                ESP_LOGI(TAG, "Decrypted plaintext: %s", decrypted);
                cJSON *decrypted_json = cJSON_Parse(decrypted);
                if (decrypted_json) {
                    char *formatted = cJSON_Print(decrypted_json);
                    ESP_LOGI(TAG, "Decrypted JSON Data:\n%s", formatted);
                    free(formatted);
                    cJSON_Delete(decrypted_json);
                }
                free(decrypted);
            } else {
                ESP_LOGW(TAG, "Decryption failed, raw string: %s", res->data->valuestring);
            }
        } else {
            char *data_str = cJSON_PrintUnformatted(res->data);
            ESP_LOGI(TAG, "Data: %s", data_str);
            free(data_str);
        }
    } else if (res->raw_body) {
        ESP_LOGI(TAG, "Raw Body: %s", res->raw_body);
    }
    ESP_LOGI(TAG, "-------------------");
    api_result_free(res);
}

static int cmd_api_test(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: api_test <api_name> [args...]\n");
        printf("Available tests:\n");
        printf("  searchFace2 [image_path] (default: /spiffs/test.jpg)\n");
        printf("  getDeviceConfig\n");
        printf("  hrp\n");
        printf("  startupSync\n");
        printf("  postFaultReport\n");
        printf("  keepSs\n");
        printf("  getLeaveMsgPage\n");
        printf("  sosServiceList\n");
        printf("  setSid <session_id>\n");
        printf("  setDeviceId <device_id>\n");
        return 1;
    }

    const char *api = argv[1];
    const char *device_id = current_device_id;

    if (strcmp(api, "searchFace2") == 0) {
        const char *img = argc > 2 ? argv[2] : "/spiffs/test.jpg";
        api_result_t *res = api_search_face(device_id, "", img);
        print_result(api, res);
    } else if (strcmp(api, "getDeviceConfig") == 0) {
        api_result_t *res = api_get_device_config(device_id, 1);
        print_result(api, res);
    } else if (strcmp(api, "hrp") == 0) {
        char payload[256];
        snprintf(payload, sizeof(payload), "%s:UJr1/irgHXF0HzZ1moHh0w9a/gsp3478xhZWaJhzsDmd6VZSkh88epCj25t3TJdiqlDf1Q7A431csb4x713eIBu9KfHXD632fs1KwjpKMubNHNSK1xQbSPWWgsyDUt66Vf98zWUTZHFXdkI6oW8WsVRfJ5bivWaweWZhs0PzDfo=", device_id);
        api_result_t *res = api_hrp(payload);
        print_result(api, res);
    } else if (strcmp(api, "startupSync") == 0) {
        char payload[256];
        snprintf(payload, sizeof(payload), "%s:UJr1/irgHXF0HzZ1moHh0w9a/gsp3478xhZWaJhzsDmd6VZSkh88epCj25t3TJdiqlDf1Q7A431csb4x713eIBu9KfHXD632fs1KwjpKMubNHNSK1xQbSPWWgsyDUt66Vf98zWUTZHFXdkI6oW8WsVRfJ5bivWaweWZhs0PzDfo=", device_id);
        api_result_t *res = api_startup_sync(payload);
        print_result(api, res);
    } else if (strcmp(api, "postFaultReport") == 0) {
        api_result_t *res = api_post_fault_report(device_id, "hardware", "camera error", "camera init failed code 5");
        print_result(api, res);
    } else if (strcmp(api, "keepSs") == 0) {
        api_result_t *res = api_keep_ss(current_sid);
        print_result(api, res);
    } else if (strcmp(api, "getLeaveMsgPage") == 0) {
        api_result_t *res = api_get_leave_msg_page(current_sid);
        print_result(api, res);
    } else if (strcmp(api, "sosServiceList") == 0) {
        api_result_t *res = api_sos_get_service_list(device_id, "2026-07-08 12:00:00", 1, "");
        print_result(api, res);
    } else if (strcmp(api, "setSid") == 0) {
        if (argc > 2) {
            snprintf(current_sid, sizeof(current_sid), "%s", argv[2]);
            printf("Session ID set to: %s\n", current_sid);
        } else {
            printf("Current Session ID: %s\n", current_sid);
        }
    } else if (strcmp(api, "setDeviceId") == 0) {
        if (argc > 2) {
            snprintf(current_device_id, sizeof(current_device_id), "%s", argv[2]);
            printf("Device ID set to: %s\n", current_device_id);
        } else {
            printf("Current Device ID: %s\n", current_device_id);
        }
    } else {
        printf("Unknown API test: %s\n", api);
    }

    return 0;
}

void register_api_test_cmd(void) {
    const esp_console_cmd_t cmd = {
        .command = "api_test",
        .help = "Test backend API endpoints",
        .hint = "<api_name> [args...]",
        .func = &cmd_api_test,
    };
    console_register_cmd(&cmd);
}
