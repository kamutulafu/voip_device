#include "tts_xfyun.h"
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
#include "wifi_manager.h"

#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

static const char *TAG = "tts_xfyun";

/* iFlytek (讯飞) account credentials, shared with the reference app_TTS project. */
#define XF_APPID       "de400d88"
#define XF_API_KEY     "d9ab074d46f9e418b270c918ddea830e"
#define XF_API_SECRET  "ZDA5YTk3NmQ1OTcwOWMwY2QxNzBjMWQ2"

#define XF_HOST        "tts-api.xfyun.cn"
#define XF_PATH        "/v2/tts"

/* Synthesized audio format: 16 kHz / 16-bit / mono raw PCM */
#define TTS_SAMPLE_RATE         16000
#define TTS_RESULT_TIMEOUT_MS   60000

/* Voice name (发音人) and parameters. "x4_xiaofang" (讯飞芳芳). */
#define TTS_VOICE_NAME          "x4_xiaofang"
#define TTS_SPEED               45
#define TTS_VOLUME              50
#define TTS_PITCH               50

#pragma pack(push, 1)
typedef struct {
    char     chunk_id[4];       // "RIFF"
    uint32_t chunk_size;        // file_size - 8
    char     format[4];         // "WAVE"
    char     subchunk1_id[4];   // "fmt "
    uint32_t subchunk1_size;    // 16 for PCM
    uint16_t audio_format;      // 1 for PCM
    uint16_t num_channels;      // 1 (Mono)
    uint32_t sample_rate;       // 16000
    uint32_t byte_rate;         // sample_rate * channels * bits / 8
    uint16_t block_align;       // channels * bits / 8
    uint16_t bits_per_sample;   // 16
    char     subchunk2_id[4];   // "data"
    uint32_t subchunk2_size;    // data_size
} wav_header_t;
#pragma pack(pop)

typedef struct {
    SemaphoreHandle_t connected;
    SemaphoreHandle_t done;
    bool error;
    char err_msg[160];

    /* Re-assembly buffer for (possibly fragmented) text frames */
    char *acc;
    size_t acc_len;
    size_t acc_cap;

    /* Output WAV file and the number of audio bytes written so far */
    FILE *fp;
    
    /* Memory buffer for output WAV file */
    uint8_t *mem_buf;
    size_t mem_len;
    size_t mem_cap;

    /* Streaming playback mode: feed each decoded PCM chunk straight to the
     * speaker via audio_play_pcm_write() as it arrives, instead of
     * buffering the whole utterance first. */
    bool streaming;
    bool pcm_started;   /* audio_play_pcm_begin() already called */
    bool aborted;        /* audio_play_abort() fired mid-stream (not an error) */

    size_t audio_bytes;
} tts_ctx_t;

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

/* Build the authenticated wss:// URL for the TTS service. */
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

/* Decode one TTS JSON message and append its audio chunk to the output file. */
static void parse_result_json(tts_ctx_t *ctx, const char *json, size_t len)
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
        cJSON *audio = cJSON_GetObjectItem(data, "audio");
        if (cJSON_IsString(audio) && audio->valuestring && audio->valuestring[0] != '\0') {
            size_t b64_len = strlen(audio->valuestring);
            size_t pcm_cap = (b64_len / 4 + 1) * 3 + 4;
            unsigned char *pcm = malloc(pcm_cap);
            if (pcm) {
                size_t pcm_len = 0;
                if (mbedtls_base64_decode(pcm, pcm_cap, &pcm_len,
                                          (const unsigned char *)audio->valuestring,
                                          b64_len) == 0 && pcm_len > 0) {
                    if (ctx->fp) {
                        if (fwrite(pcm, 1, pcm_len, ctx->fp) == pcm_len) {
                            ctx->audio_bytes += pcm_len;
                        } else {
                            ctx->error = true;
                            snprintf(ctx->err_msg, sizeof(ctx->err_msg),
                                     "Failed to write audio to file (disk full?)");
                        }
                    } else if (ctx->mem_buf) {
                        if (sizeof(wav_header_t) + ctx->audio_bytes + pcm_len > ctx->mem_cap) {
                            size_t new_cap = ctx->mem_cap == 0 ? 64 * 1024 : ctx->mem_cap * 2;
                            while (sizeof(wav_header_t) + ctx->audio_bytes + pcm_len > new_cap) {
                                new_cap *= 2;
                            }
                            uint8_t *tmp = heap_caps_realloc(ctx->mem_buf, new_cap, MALLOC_CAP_SPIRAM);
                            if (!tmp) {
                                tmp = realloc(ctx->mem_buf, new_cap);
                            }
                            if (tmp) {
                                ctx->mem_buf = tmp;
                                ctx->mem_cap = new_cap;
                            } else {
                                ctx->error = true;
                                snprintf(ctx->err_msg, sizeof(ctx->err_msg), "Out of memory");
                            }
                        }
                        
                        if (!ctx->error) {
                            memcpy(ctx->mem_buf + sizeof(wav_header_t) + ctx->audio_bytes, pcm, pcm_len);
                            ctx->audio_bytes += pcm_len;
                        }
                    } else if (ctx->streaming) {
                        cJSON *status_obj = cJSON_GetObjectItem(data, "status");
                        bool is_last_chunk = (cJSON_IsNumber(status_obj) && status_obj->valueint == 2);
                        if (audio_play_is_aborted()) {
                            ctx->aborted = true;
                        } else {
                            audio_queue_push((const int16_t *)pcm, pcm_len / 2, is_last_chunk);
                        }
                        ctx->audio_bytes += pcm_len;
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to base64-decode audio chunk");
                }
                free(pcm);
            }
        }

        cJSON *status = cJSON_GetObjectItem(data, "status");
        if (ctx->error || ctx->aborted || (cJSON_IsNumber(status) && status->valueint == 2)) {
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
    tts_ctx_t *ctx = (tts_ctx_t *)handler_args;
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

/* Send the single synthesis request frame (text is base64-encoded UTF-8). */
static esp_err_t send_tts_request(esp_websocket_client_handle_t client, const char *text)
{
    size_t text_len = strlen(text);
    size_t b64_cap = 4 * ((text_len + 2) / 3) + 1;
    char *text_b64 = malloc(b64_cap);
    if (!text_b64) {
        return ESP_ERR_NO_MEM;
    }
    size_t out_len = 0;
    if (mbedtls_base64_encode((unsigned char *)text_b64, b64_cap, &out_len,
                              (const unsigned char *)text, text_len) != 0) {
        free(text_b64);
        return ESP_FAIL;
    }
    text_b64[out_len] = '\0';

    size_t json_cap = out_len + 512;
    char *json = malloc(json_cap);
    if (!json) {
        free(text_b64);
        return ESP_ERR_NO_MEM;
    }

    int n = snprintf(json, json_cap,
        "{\"common\":{\"app_id\":\"%s\"},"
        "\"business\":{\"aue\":\"raw\",\"auf\":\"audio/L16;rate=16000\","
        "\"vcn\":\"%s\",\"tte\":\"UTF8\",\"speed\":%d,\"volume\":%d,\"pitch\":%d},"
        "\"data\":{\"status\":2,\"text\":\"%s\"}}",
        XF_APPID, TTS_VOICE_NAME, TTS_SPEED, TTS_VOLUME, TTS_PITCH, text_b64);

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
    free(text_b64);
    return ret;
}

static esp_err_t ensure_time_synced(void)
{
    return ESP_OK; /* Hardware RTC is maintained by RTC backup battery */
}

/* Write the standard 44-byte mono/16 kHz/16-bit WAV header to *f. */
static void write_wav_header(FILE *f, uint32_t data_bytes)
{
    wav_header_t header;
    memcpy(header.chunk_id, "RIFF", 4);
    header.chunk_size = data_bytes + sizeof(wav_header_t) - 8;
    memcpy(header.format, "WAVE", 4);
    memcpy(header.subchunk1_id, "fmt ", 4);
    header.subchunk1_size = 16;
    header.audio_format = 1; // PCM
    header.num_channels = 1; // Mono
    header.sample_rate = TTS_SAMPLE_RATE;
    header.bits_per_sample = 16;
    header.byte_rate = TTS_SAMPLE_RATE * 1 * 16 / 8;
    header.block_align = 1 * 16 / 8;
    memcpy(header.subchunk2_id, "data", 4);
    header.subchunk2_size = data_bytes;
    fwrite(&header, 1, sizeof(header), f);
}

/* ----- public API ------------------------------------------------------ */

esp_err_t tts_xfyun_synthesize_to_file(const char *text, const char *filename)
{
    int64_t start_time = esp_timer_get_time();
    if (!text || text[0] == '\0' || !filename) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_time_synced();
    if (err != ESP_OK) {
        return err;
    }

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open %s for writing", filename);
        return ESP_FAIL;
    }
    /* Reserve space for the WAV header; rewritten with real size at the end. */
    write_wav_header(fp, 0);

    char *url = build_auth_url();
    if (!url) {
        ESP_LOGE(TAG, "Failed to build auth URL");
        fclose(fp);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Connecting to TTS service...");

    tts_ctx_t ctx = {0};
    ctx.connected = xSemaphoreCreateBinary();
    ctx.done = xSemaphoreCreateBinary();
    ctx.fp = fp;
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

    err = send_tts_request(client, text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send TTS request");
        goto cleanup_client;
    }

    /* Wait for all audio frames (status=2) */
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(TTS_RESULT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for synthesis result");
        err = ESP_ERR_TIMEOUT;
    } else if (ctx.error) {
        ESP_LOGE(TAG, "%s", ctx.err_msg);
        err = ESP_FAIL;
    } else if (ctx.audio_bytes == 0) {
        ESP_LOGE(TAG, "No audio received from TTS service");
        err = ESP_FAIL;
    } else {
        err = ESP_OK;
    }

cleanup_client:
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
cleanup:
    /* Finalize the WAV header with the real audio size. */
    if (fseek(fp, 0, SEEK_SET) == 0) {
        write_wav_header(fp, (uint32_t)ctx.audio_bytes);
    }
    fclose(fp);

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
        ESP_LOGI(TAG, "Synthesized %u bytes of audio to %s (Took %lld ms)",
                 (unsigned)ctx.audio_bytes, filename, elapsed_ms);
    } else {
        ESP_LOGE(TAG, "TTS file synthesis failed after %lld ms", elapsed_ms);
    }
    return err;
}

esp_err_t tts_xfyun_synthesize_to_mem(const char *text, uint8_t **out_buf, size_t *out_len)
{
    int64_t start_time = esp_timer_get_time();
    if (!text || text[0] == '\0' || !out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    esp_err_t err = ensure_time_synced();
    if (err != ESP_OK) {
        return err;
    }

    char *url = build_auth_url();
    if (!url) {
        ESP_LOGE(TAG, "Failed to build auth URL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Connecting to TTS service...");

    tts_ctx_t ctx = {0};
    ctx.connected = xSemaphoreCreateBinary();
    ctx.done = xSemaphoreCreateBinary();
    ctx.mem_cap = 64 * 1024;
    ctx.mem_buf = heap_caps_malloc(ctx.mem_cap, MALLOC_CAP_SPIRAM);
    if (!ctx.mem_buf) {
        ctx.mem_buf = malloc(ctx.mem_cap);
    }
    if (!ctx.mem_buf || !ctx.connected || !ctx.done) {
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

    err = send_tts_request(client, text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send TTS request");
        goto cleanup_client;
    }

    /* Wait for all audio frames (status=2) */
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(TTS_RESULT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for synthesis result");
        err = ESP_ERR_TIMEOUT;
    } else if (ctx.error) {
        ESP_LOGE(TAG, "%s", ctx.err_msg);
        err = ESP_FAIL;
    } else if (ctx.audio_bytes == 0) {
        ESP_LOGE(TAG, "No audio received from TTS service");
        err = ESP_FAIL;
    } else {
        err = ESP_OK;
    }

cleanup_client:
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
cleanup:
    if (err == ESP_OK && ctx.mem_buf) {
        // Write the standard 44-byte mono/16 kHz/16-bit WAV header to the start of buffer.
        wav_header_t header;
        memcpy(header.chunk_id, "RIFF", 4);
        header.chunk_size = (uint32_t)ctx.audio_bytes + sizeof(wav_header_t) - 8;
        memcpy(header.format, "WAVE", 4);
        memcpy(header.subchunk1_id, "fmt ", 4);
        header.subchunk1_size = 16;
        header.audio_format = 1; // PCM
        header.num_channels = 1; // Mono
        header.sample_rate = TTS_SAMPLE_RATE;
        header.bits_per_sample = 16;
        header.byte_rate = TTS_SAMPLE_RATE * 1 * 16 / 8;
        header.block_align = 1 * 16 / 8;
        memcpy(header.subchunk2_id, "data", 4);
        header.subchunk2_size = (uint32_t)ctx.audio_bytes;
        
        memcpy(ctx.mem_buf, &header, sizeof(wav_header_t));
        *out_buf = ctx.mem_buf;
        *out_len = sizeof(wav_header_t) + ctx.audio_bytes;
    } else {
        if (ctx.mem_buf) {
            free(ctx.mem_buf);
        }
    }

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
        ESP_LOGI(TAG, "Synthesized %u bytes of audio to memory (Took %lld ms)", (unsigned)ctx.audio_bytes, elapsed_ms);
    } else {
        ESP_LOGE(TAG, "TTS memory synthesis failed after %lld ms", elapsed_ms);
    }
    return err;
}

esp_err_t tts_xfyun_synthesize_stream(const char *text)
{
    int64_t start_time = esp_timer_get_time();
    if (!text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_time_synced();
    if (err != ESP_OK) {
        return err;
    }

    char *url = build_auth_url();
    if (!url) {
        ESP_LOGE(TAG, "Failed to build auth URL");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Connecting to TTS service (streaming playback)...");

    audio_queue_start(TTS_SAMPLE_RATE);

    tts_ctx_t ctx = {0};
    ctx.streaming = true;
    ctx.connected = xSemaphoreCreateBinary();
    ctx.done = xSemaphoreCreateBinary();
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

    err = send_tts_request(client, text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send TTS request");
        goto cleanup_client;
    }

    /* Wait for all audio frames (status=2), an error, or a mid-stream abort. */
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(TTS_RESULT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for synthesis result");
        err = ESP_ERR_TIMEOUT;
    } else if (ctx.aborted) {
        ESP_LOGI(TAG, "TTS streaming playback aborted mid-utterance");
        err = ESP_OK; /* not a failure, just cut short on purpose */
    } else if (ctx.error) {
        ESP_LOGE(TAG, "%s", ctx.err_msg);
        err = ESP_FAIL;
    } else if (ctx.audio_bytes == 0) {
        ESP_LOGE(TAG, "No audio received from TTS service");
        err = ESP_FAIL;
    } else {
        err = ESP_OK;
    }

    /* Signal completion to Audio_Queue and wait for Playback Consumer thread to finish playing smooth audio */
    audio_queue_finish();
    audio_queue_wait_done(60000);

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
        ESP_LOGI(TAG, "Streamed %u bytes of audio to speaker (Took %lld ms)",
                 (unsigned)ctx.audio_bytes, elapsed_ms);
    } else {
        ESP_LOGE(TAG, "TTS streaming synthesis failed after %lld ms", elapsed_ms);
    }
    return err;
}

int cmd_tts_test(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: tts_test <text...>\n");
        printf("Example: tts_test 你好，我是儿童求助系统\n");
        return 1;
    }

    /* Join all arguments into a single string separated by spaces. */
    char text[512] = {0};
    size_t pos = 0;
    for (int i = 1; i < argc; i++) {
        int n = snprintf(text + pos, sizeof(text) - pos,
                         (i == 1) ? "%s" : " %s", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(text) - pos) {
            break;
        }
        pos += n;
    }

    const char *filepath = "/spiffs/tts.wav";
    printf("Synthesizing speech via iFlytek: \"%s\"\n", text);

    esp_err_t err = tts_xfyun_synthesize_to_file(text, filepath);
    if (err != ESP_OK) {
        printf("Text-to-speech failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Playing synthesized audio...\n");
    err = audio_play_from_file(filepath);
    if (err != ESP_OK) {
        printf("Playback failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Done.\n");
    return 0;
}

int cmd_tts_test_stream(int argc, char **argv)
{
    char text[1024] = {0};

    if (argc < 2) {
        snprintf(text, sizeof(text),
                 "小朋友你好！我是智能儿童求助与安全守护终端。当你在生活中遇到困难或紧急情况时，可以通过我随时向爸爸妈妈或求助中心发起语音通话和留言。请始终保持勇敢与冷静，我会时刻守护在你的身边，为你提供及时、安全、温暖的帮助与保障。现在正在为您进行长文本流式语音合成播放压力测试，用于验证高吞吐网络数据下的解耦缓冲区与抖动平滑播放能力，请仔细听取声音是否清晰连贯没有任何暂停卡顿。");
    } else {
        size_t pos = 0;
        for (int i = 1; i < argc; i++) {
            int n = snprintf(text + pos, sizeof(text) - pos,
                             (i == 1) ? "%s" : " %s", argv[i]);
            if (n < 0 || (size_t)n >= sizeof(text) - pos) {
                break;
            }
            pos += n;
        }
    }

    printf("Streaming speech via iFlytek: \"%s\"\n", text);
    audio_play_clear_abort();
    esp_err_t err = tts_xfyun_synthesize_stream(text);
    if (err != ESP_OK) {
        printf("Streaming text-to-speech failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Done.\n");
    return 0;
}
