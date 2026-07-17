#include "asr_xfyun.h"
#include "audio_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"
#include "wifi_manager.h"

#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

static const char *TAG = "asr_xfyun";

/* iFlytek (讯飞) account credentials, shared with the reference app_TTS project. */
#define XF_APPID       "de400d88"
#define XF_API_KEY     "d9ab074d46f9e418b270c918ddea830e"
#define XF_API_SECRET  "ZDA5YTk3NmQ1OTcwOWMwY2QxNzBjMWQ2"

#define XF_HOST        "iat-api.xfyun.cn"
#define XF_PATH        "/v2/iat"

#define ASR_AUDIO_FRAME_BYTES   1280   /* ~40 ms of 16 kHz/16-bit mono audio */
#define ASR_FRAME_INTERVAL_MS   40
#define ASR_RESULT_TIMEOUT_MS   15000

typedef struct {
    SemaphoreHandle_t connected;
    SemaphoreHandle_t done;
    bool error;
    char err_msg[160];

    /* Re-assembly buffer for (possibly fragmented) text frames */
    char *acc;
    size_t acc_len;
    size_t acc_cap;

    /* Recognized text accumulation */
    char *text;
    size_t text_size;
    size_t text_len;
} asr_ctx_t;

/* ----- helpers --------------------------------------------------------- */

/* Percent-encode a string for safe use in a URL query value. */
static char *url_encode(const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t len = strlen(src);
    char *out = malloc(len * 3 + 1);
    if (!out) {
        return NULL;
    }
    char *p = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = (char)c;
        } else {
            *p++ = '%';
            *p++ = hex[(c >> 4) & 0xF];
            *p++ = hex[c & 0xF];
        }
    }
    *p = '\0';
    return out;
}

/* Build the authenticated wss:// URL for the IAT service. */
static char *build_auth_url(void)
{
    /* RFC1123 GMT date, e.g. "Tue, 30 Jun 2026 06:00:00 GMT" */
    char date[64];
    time_t now = time(NULL);
    struct tm tm_gmt;
    gmtime_r(&now, &tm_gmt);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm_gmt);

    /* signature origin */
    char sign_origin[256];
    int n = snprintf(sign_origin, sizeof(sign_origin),
                     "host: %s\ndate: %s\nGET %s HTTP/1.1", XF_HOST, date, XF_PATH);
    if (n <= 0 || n >= (int)sizeof(sign_origin)) {
        return NULL;
    }

    /* HMAC-SHA256(secret, sign_origin) -> base64 */
    unsigned char hmac[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL ||
        mbedtls_md_hmac(md, (const unsigned char *)XF_API_SECRET, strlen(XF_API_SECRET),
                        (const unsigned char *)sign_origin, strlen(sign_origin), hmac) != 0) {
        ESP_LOGE(TAG, "HMAC computation failed");
        return NULL;
    }

    char signature_b64[64];
    size_t sig_len = 0;
    if (mbedtls_base64_encode((unsigned char *)signature_b64, sizeof(signature_b64),
                              &sig_len, hmac, sizeof(hmac)) != 0) {
        return NULL;
    }
    signature_b64[sig_len] = '\0';

    /* authorization origin -> base64 */
    char auth_origin[256];
    n = snprintf(auth_origin, sizeof(auth_origin),
                 "api_key=\"%s\", algorithm=\"hmac-sha256\", "
                 "headers=\"host date request-line\", signature=\"%s\"",
                 XF_API_KEY, signature_b64);
    if (n <= 0 || n >= (int)sizeof(auth_origin)) {
        return NULL;
    }

    char authorization_b64[512];
    size_t auth_len = 0;
    if (mbedtls_base64_encode((unsigned char *)authorization_b64, sizeof(authorization_b64),
                              &auth_len, (const unsigned char *)auth_origin,
                              strlen(auth_origin)) != 0) {
        return NULL;
    }
    authorization_b64[auth_len] = '\0';

    char *date_enc = url_encode(date);
    char *auth_enc = url_encode(authorization_b64);
    if (!date_enc || !auth_enc) {
        free(date_enc);
        free(auth_enc);
        return NULL;
    }

    char *url = malloc(1024);
    if (url) {
        snprintf(url, 1024, "wss://%s%s?authorization=%s&date=%s&host=%s",
                 XF_HOST, XF_PATH, auth_enc, date_enc, XF_HOST);
    }

    free(date_enc);
    free(auth_enc);
    return url;
}

/* Append recognized words from one IAT JSON message to ctx->text. */
static void parse_result_json(asr_ctx_t *ctx, const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON result");
        return;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        ctx->error = true;
        snprintf(ctx->err_msg, sizeof(ctx->err_msg), "iFlytek error %d: %s",
                 code->valueint, cJSON_IsString(msg) ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        xSemaphoreGive(ctx->done);
        return;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsObject(data)) {
        cJSON *result = cJSON_GetObjectItem(data, "result");
        cJSON *ws = result ? cJSON_GetObjectItem(result, "ws") : NULL;
        if (cJSON_IsArray(ws)) {
            cJSON *ws_item = NULL;
            cJSON_ArrayForEach(ws_item, ws) {
                cJSON *cw = cJSON_GetObjectItem(ws_item, "cw");
                if (cJSON_IsArray(cw)) {
                    cJSON *cw_item = NULL;
                    cJSON_ArrayForEach(cw_item, cw) {
                        cJSON *w = cJSON_GetObjectItem(cw_item, "w");
                        if (cJSON_IsString(w) && w->valuestring) {
                            size_t wl = strlen(w->valuestring);
                            if (ctx->text_len + wl + 1 < ctx->text_size) {
                                memcpy(ctx->text + ctx->text_len, w->valuestring, wl);
                                ctx->text_len += wl;
                                ctx->text[ctx->text_len] = '\0';
                            }
                        }
                    }
                }
            }
        }

        cJSON *status = cJSON_GetObjectItem(data, "status");
        if (cJSON_IsNumber(status) && status->valueint == 2) {
            cJSON_Delete(root);
            xSemaphoreGive(ctx->done);
            return;
        }
    }

    cJSON_Delete(root);
}

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    asr_ctx_t *ctx = (asr_ctx_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        xSemaphoreGive(ctx->connected);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x08) { /* close frame */
            break;
        }
        if (data->op_code != 0x01 || data->data_len <= 0) { /* only text frames */
            break;
        }
        /* Re-assemble possibly fragmented text frame */
        if (ctx->acc_len + data->data_len + 1 > ctx->acc_cap) {
            size_t new_cap = ctx->acc_len + data->data_len + 1;
            char *tmp = realloc(ctx->acc, new_cap);
            if (!tmp) {
                break;
            }
            ctx->acc = tmp;
            ctx->acc_cap = new_cap;
        }
        memcpy(ctx->acc + ctx->acc_len, data->data_ptr, data->data_len);
        ctx->acc_len += data->data_len;
        ctx->acc[ctx->acc_len] = '\0';

        if (data->payload_offset + data->data_len >= data->payload_len) {
            parse_result_json(ctx, ctx->acc, ctx->acc_len);
            ctx->acc_len = 0; /* ready for next message */
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        ctx->error = true;
        snprintf(ctx->err_msg, sizeof(ctx->err_msg), "WebSocket transport error");
        xSemaphoreGive(ctx->done);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        break;

    default:
        break;
    }
}

/* Send one IAT data frame (status 0=first, 1=continue, 2=last). */
static esp_err_t send_audio_frame(esp_websocket_client_handle_t client, int status,
                                  const uint8_t *pcm, size_t bytes)
{
    /* base64 encode the audio chunk */
    char *audio_b64 = NULL;
    if (bytes > 0) {
        size_t b64_cap = 4 * ((bytes + 2) / 3) + 1;
        audio_b64 = malloc(b64_cap);
        if (!audio_b64) {
            return ESP_ERR_NO_MEM;
        }
        size_t out_len = 0;
        if (mbedtls_base64_encode((unsigned char *)audio_b64, b64_cap, &out_len, pcm, bytes) != 0) {
            free(audio_b64);
            return ESP_FAIL;
        }
        audio_b64[out_len] = '\0';
    }

    /* Build JSON frame */
    size_t json_cap = (audio_b64 ? strlen(audio_b64) : 0) + 512;
    char *json = malloc(json_cap);
    if (!json) {
        free(audio_b64);
        return ESP_ERR_NO_MEM;
    }

    int n;
    if (status == 0) {
        n = snprintf(json, json_cap,
            "{\"common\":{\"app_id\":\"%s\"},"
            "\"business\":{\"language\":\"zh_cn\",\"domain\":\"iat\",\"accent\":\"mandarin\",\"vad_eos\":3000},"
            "\"data\":{\"status\":0,\"format\":\"audio/L16;rate=16000\",\"encoding\":\"raw\",\"audio\":\"%s\"}}",
            XF_APPID, audio_b64 ? audio_b64 : "");
    } else {
        n = snprintf(json, json_cap,
            "{\"data\":{\"status\":%d,\"format\":\"audio/L16;rate=16000\",\"encoding\":\"raw\",\"audio\":\"%s\"}}",
            status, audio_b64 ? audio_b64 : "");
    }

    esp_err_t ret = ESP_OK;
    if (n > 0 && n < (int)json_cap) {
        int sent = esp_websocket_client_send_text(client, json, n, pdMS_TO_TICKS(5000));
        if (sent < 0) {
            ret = ESP_FAIL;
        }
    } else {
        ret = ESP_FAIL;
    }

    free(json);
    free(audio_b64);
    return ret;
}

static esp_err_t ensure_time_synced(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year >= (2024 - 1900)) {
        return ESP_OK; /* already synced */
    }

    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "WiFi not connected, skipping SNTP sync");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Synchronizing time via SNTP...");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    esp_netif_sntp_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP sync failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* ----- public API ------------------------------------------------------ */

esp_err_t asr_xfyun_recognize(const int16_t *pcm, size_t num_samples,
                              char *out_text, size_t out_text_size)
{
    int64_t start_time = esp_timer_get_time();
    if (!pcm || num_samples == 0 || !out_text || out_text_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';

    esp_err_t err = ensure_time_synced();
    if (err != ESP_OK) {
        return err;
    }

    char *url = build_auth_url();
    if (!url) {
        ESP_LOGE(TAG, "Failed to build auth URL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Connecting to IAT service...");

    asr_ctx_t ctx = {0};
    ctx.connected = xSemaphoreCreateBinary();
    ctx.done = xSemaphoreCreateBinary();
    ctx.text = out_text;
    ctx.text_size = out_text_size;
    if (!ctx.connected || !ctx.done) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .buffer_size = 8192,
        .task_stack = 8192,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 10000,
        .disable_auto_reconnect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    if (!client) {
        err = ESP_FAIL;
        goto cleanup;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler, &ctx);

    err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        goto cleanup_client;
    }

    if (xSemaphoreTake(ctx.connected, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for connection");
        err = ESP_ERR_TIMEOUT;
        goto cleanup_client;
    }

    /* Stream the PCM buffer in ~40 ms frames */
    const uint8_t *bytes = (const uint8_t *)pcm;
    size_t total_bytes = num_samples * sizeof(int16_t);
    size_t offset = 0;
    bool first = true;

    while (offset < total_bytes) {
        size_t chunk = total_bytes - offset;
        if (chunk > ASR_AUDIO_FRAME_BYTES) {
            chunk = ASR_AUDIO_FRAME_BYTES;
        }
        bool last = (offset + chunk >= total_bytes);
        int status = first ? 0 : (last ? 2 : 1);

        err = send_audio_frame(client, status, bytes + offset, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send audio frame");
            goto cleanup_client;
        }
        first = false;
        offset += chunk;
        // Stream pre-recorded audio at high-speed (2ms delay) instead of real-time (40ms) to reduce latency
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    /* Make sure a final (status=2) frame is sent even if the loop ended on a
     * full-size chunk that was not flagged as last. */
    if (!first) {
        send_audio_frame(client, 2, NULL, 0);
    }

    /* Wait for the final recognition result */
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(ASR_RESULT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for recognition result");
        err = ESP_ERR_TIMEOUT;
    } else if (ctx.error) {
        ESP_LOGE(TAG, "%s", ctx.err_msg);
        err = ESP_FAIL;
    } else {
        err = ESP_OK;
    }

cleanup_client:
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
cleanup:
    free(ctx.acc);
    if (ctx.connected) {
        vSemaphoreDelete(ctx.connected);
    }
    if (ctx.done) {
        vSemaphoreDelete(ctx.done);
    }
    free(url);

    int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Speech recognition completed. Result: '%s' (Took %lld ms)", out_text, elapsed_ms);
    } else {
        ESP_LOGE(TAG, "Speech recognition failed after %lld ms", elapsed_ms);
    }
    return err;
}

int cmd_asr_test(int argc, char **argv)
{
    uint32_t duration = 5; // default 5 seconds
    if (argc >= 2) {
        int d = atoi(argv[1]);
        if (d < 1 || d > 60) {
            printf("Invalid duration (1 to 60 seconds)\n");
            return 1;
        }
        duration = (uint32_t)d;
    }

    printf("Recording %u second(s) of audio for speech recognition...\n", (unsigned)duration);

    int16_t *pcm = NULL;
    size_t num_samples = 0;
    esp_err_t err = audio_record_mono_pcm(&pcm, &num_samples, duration);
    if (err != ESP_OK) {
        printf("Recording failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Recognizing speech via iFlytek...\n");
    char text[1024] = {0};
    err = asr_xfyun_recognize(pcm, num_samples, text, sizeof(text));
    free(pcm);

    if (err != ESP_OK) {
        printf("Speech recognition failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    if (text[0] == '\0') {
        printf("Recognition result: (empty - no speech detected)\n");
    } else {
        printf("Recognition result: %s\n", text);
    }
    return 0;
}
