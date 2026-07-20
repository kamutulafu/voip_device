#include "dialogue.h"
#include "tts_xfyun.h"
#include "audio_driver.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "dialogue";

/* Scratch file reused for every TTS utterance. */
#define TTS_PLAY_PATH   "/spiffs/tts_play.wav"
#define BEEP_PATH       "/spiffs/beep.wav"

/* ============================================================
 * Serialized speech queue
 *
 * All spoken output (online TTS, local WAV files, the beep tone) is funneled
 * through a single worker task so utterances play strictly one-at-a-time and
 * never overlap on the shared I2S channel. Public speak/play functions enqueue
 * an item and block until the worker has finished it.
 *
 * dialogue_abort_all() bumps a generation counter (so items already queued are
 * discarded) and aborts any in-progress playback; used to cut the greeting
 * short the moment a face is recognized.
 * ============================================================ */

typedef enum { SP_TTS, SP_FILE, SP_BEEP } sp_type_t;

typedef struct {
    sp_type_t type;
    char text[512];              /* TTS text, or SP_FILE fallback text */
    char path[160];              /* SP_FILE path */
    SemaphoreHandle_t done;
    esp_err_t result;
    uint32_t generation;
} speech_item_t;

static QueueHandle_t     s_sp_queue = NULL;
static TaskHandle_t      s_sp_worker = NULL;
static volatile uint32_t s_sp_generation = 0;

static esp_err_t ensure_beep_file(void); /* forward decl */

/* Synthesize @p text and play it, unless an abort bumped the generation while
 * synthesizing (in which case playback is skipped). */
static esp_err_t worker_synth_and_play(const speech_item_t *it)
{
    (void)audio_set_volume(AUDIO_DEFAULT_VOLUME);

    uint8_t *wav_buf = NULL;
    size_t wav_len = 0;
    esp_err_t err = tts_xfyun_synthesize_to_mem(it->text, &wav_buf, &wav_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS synthesis failed: %s. Trying local error voice.", esp_err_to_name(err));
        struct stat st;
        if (stat(PATH_V_SYS_ERR, &st) == 0 && S_ISREG(st.st_mode)) {
            audio_play_clear_abort();
            audio_play_from_file(PATH_V_SYS_ERR);
        }
        return err;
    }

    /* If we were aborted during the (blocking) synthesis, don't play. */
    if (it->generation == s_sp_generation) {
        audio_play_clear_abort();
        err = audio_play_from_mem(wav_buf, wav_len);
    } else {
        ESP_LOGI(TAG, "utterance aborted during synth, skipping playback");
        err = ESP_OK;
    }
    free(wav_buf);
    return err;
}

static void speech_worker_task(void *arg)
{
    (void)arg;
    speech_item_t *it = NULL;
    for (;;) {
        if (xQueueReceive(s_sp_queue, &it, portMAX_DELAY) != pdTRUE || !it) {
            continue;
        }

        /* Discard items queued before the last abort. */
        if (it->generation != s_sp_generation) {
            it->result = ESP_OK;
            if (it->done) xSemaphoreGive(it->done);
            continue;
        }

        esp_err_t err = ESP_OK;
        switch (it->type) {
        case SP_TTS:
            err = worker_synth_and_play(it);
            break;
        case SP_FILE: {
            struct stat st;
            if (stat(it->path, &st) == 0 && S_ISREG(st.st_mode)) {
                ESP_LOGI(TAG, "Playing local voice: %s", it->path);
                (void)audio_set_volume(AUDIO_DEFAULT_VOLUME);
                audio_play_clear_abort();
                err = audio_play_from_file(it->path);
            } else if (it->text[0] != '\0') {
                ESP_LOGW(TAG, "Local voice %s missing, falling back to TTS", it->path);
                err = worker_synth_and_play(it);
            } else {
                err = ESP_ERR_NOT_FOUND;
            }
            break;
        }
        case SP_BEEP:
            if (ensure_beep_file() == ESP_OK) {
                audio_play_clear_abort();
                err = audio_play_from_file(BEEP_PATH);
            } else {
                ESP_LOGW(TAG, "beep tone unavailable");
            }
            break;
        }

        it->result = err;
        if (it->done) xSemaphoreGive(it->done);
    }
}

void dialogue_audio_init(void)
{
    if (s_sp_queue) {
        return;
    }
    s_sp_queue = xQueueCreate(8, sizeof(speech_item_t *));
    if (!s_sp_queue) {
        ESP_LOGE(TAG, "failed to create speech queue");
        return;
    }
    if (xTaskCreate(speech_worker_task, "speech_worker", 8192, NULL, 5, &s_sp_worker) != pdPASS) {
        ESP_LOGE(TAG, "failed to create speech worker task");
    }
}

void dialogue_abort_all(void)
{
    s_sp_generation++;      /* items already queued become stale */
    audio_play_abort();     /* stop any in-progress playback     */
}

/* Enqueue one speech item and block until the worker has finished it. */
static esp_err_t sp_enqueue_wait(sp_type_t type, const char *text, const char *path)
{
    if (!s_sp_queue) {
        dialogue_audio_init();
        if (!s_sp_queue) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    speech_item_t *it = calloc(1, sizeof(*it));
    if (!it) {
        return ESP_ERR_NO_MEM;
    }
    it->type = type;
    if (text) strlcpy(it->text, text, sizeof(it->text));
    if (path) strlcpy(it->path, path, sizeof(it->path));
    it->generation = s_sp_generation;
    it->done = xSemaphoreCreateBinary();
    if (!it->done) {
        free(it);
        return ESP_ERR_NO_MEM;
    }

    if (xQueueSend(s_sp_queue, &it, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(it->done);
        free(it);
        return ESP_FAIL;
    }

    xSemaphoreTake(it->done, portMAX_DELAY);
    esp_err_t r = it->result;
    vSemaphoreDelete(it->done);
    free(it);
    return r;
}

esp_err_t play_local_voice(const char *path, const char *fallback_text)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    return sp_enqueue_wait(SP_FILE, fallback_text, path);
}

esp_err_t dialogue_speak(const char *text)
{
    if (!text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "TTS(queue): %s", text);
    return sp_enqueue_wait(SP_TTS, text, NULL);
}

esp_err_t dialogue_speak_fmt(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return dialogue_speak(buf);
}

/* Generate a simple 16 kHz / 16-bit mono sine "beep" WAV once, then play it. */
static esp_err_t ensure_beep_file(void)
{
    FILE *probe = fopen(BEEP_PATH, "rb");
    if (probe) {
        fclose(probe);
        return ESP_OK;
    }

    const uint32_t sample_rate = 16000;
    const uint32_t duration_ms = 300;
    const double freq = 880.0;
    const uint32_t num_samples = sample_rate * duration_ms / 1000;
    const uint32_t data_bytes = num_samples * 2; /* mono, 16-bit */

    FILE *f = fopen(BEEP_PATH, "wb");
    if (!f) {
        return ESP_FAIL;
    }

    /* WAV header (mono to keep it small; player handles it). */
    uint8_t hdr[44];
    uint32_t chunk_size = 36 + data_bytes;
    uint32_t byte_rate = sample_rate * 2;
    memcpy(hdr, "RIFF", 4);
    hdr[4] = chunk_size & 0xff; hdr[5] = (chunk_size >> 8) & 0xff;
    hdr[6] = (chunk_size >> 16) & 0xff; hdr[7] = (chunk_size >> 24) & 0xff;
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0; /* subchunk1 size */
    hdr[20] = 1; hdr[21] = 0;                            /* PCM */
    hdr[22] = 1; hdr[23] = 0;                            /* mono */
    hdr[24] = sample_rate & 0xff; hdr[25] = (sample_rate >> 8) & 0xff;
    hdr[26] = (sample_rate >> 16) & 0xff; hdr[27] = (sample_rate >> 24) & 0xff;
    hdr[28] = byte_rate & 0xff; hdr[29] = (byte_rate >> 8) & 0xff;
    hdr[30] = (byte_rate >> 16) & 0xff; hdr[31] = (byte_rate >> 24) & 0xff;
    hdr[32] = 2; hdr[33] = 0;                            /* block align */
    hdr[34] = 16; hdr[35] = 0;                           /* bits per sample */
    memcpy(hdr + 36, "data", 4);
    hdr[40] = data_bytes & 0xff; hdr[41] = (data_bytes >> 8) & 0xff;
    hdr[42] = (data_bytes >> 16) & 0xff; hdr[43] = (data_bytes >> 24) & 0xff;
    fwrite(hdr, 1, sizeof(hdr), f);

    for (uint32_t i = 0; i < num_samples; i++) {
        double t = (double)i / sample_rate;
        int16_t s = (int16_t)(sin(2.0 * M_PI * freq * t) * 12000.0);
        fwrite(&s, sizeof(s), 1, f);
    }
    fclose(f);
    return ESP_OK;
}

void dialogue_beep(void)
{
    sp_enqueue_wait(SP_BEEP, NULL, NULL);
}

/* ============================================================
 * Templates
 * ============================================================ */

void dialogue_greeting(void)
{
    play_local_voice(PATH_V_GREETING, TEXT_V_GREETING);
}

void dialogue_welcome_menu(const char *name)
{
    dialogue_speak_fmt("哇，%s您好，很高兴为您服务，您可以说发留言、打电话或者加好友，说完请说over！",
                       (name && name[0]) ? name : "小朋友");
}

void dialogue_welcome_unknown(void)
{
    dialogue_speak("您好，我还不认识您呢，快让家长开通吧。"
                   "您也可以说发留言使用我的公益服务，说完请说over！");
}

void dialogue_timeout_bye(void)
{
    play_local_voice(PATH_V_NO_RESP, TEXT_V_NO_RESP);
}

void dialogue_confirm_info(const char *content, bool with_beep)
{
    if (with_beep) {
        dialogue_speak_fmt("让我再确认一下：是%s吗？请在滴声后告诉我正确或者错误，说完请说over！",
                           content ? content : "");
        dialogue_beep();
    } else {
        dialogue_speak_fmt("让我再确认一下：是%s吗？请告诉我正确或者错误，说完请说over！",
                           content ? content : "");
    }
}

void dialogue_ask_retry(void)
{
    dialogue_speak("呃……好像没听清楚，可以在滴声后再说一遍吗？说完请说over！");
    dialogue_beep();
}

void dialogue_farewell(void)
{
    dialogue_speak("有需要再找我，拜拜~");
}

int cmd_voice_update(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!wifi_manager_is_connected()) {
        printf("Error: WiFi is not connected. Please connect to WiFi first using: wifi_join <ssid> <password>\n");
        return 1;
    }

    printf("Starting local voice updates...\n");

    const struct {
        const char *text;
        const char *path;
        const char *desc;
    } voices[] = {
        { TEXT_V_SYS_START, PATH_V_SYS_START, "1. 系统启动成功，正在检查网络" },
        { TEXT_V_NET_OK,    PATH_V_NET_OK,    "2. 网络连接成功" },
        { TEXT_V_NET_ERR,   PATH_V_NET_ERR,   "3. 网络连接失败" },
        { TEXT_V_SYS_ERR,   PATH_V_SYS_ERR,   "4. 哎呀，系统开小差了" },
        { TEXT_V_FACE_FAIL, PATH_V_FACE_FAIL, "5. 未识别到人脸" },
        { TEXT_V_NO_RESP,   PATH_V_NO_RESP,   "6. 你好像不在这里，下次再找我吧，拜拜" },
        { TEXT_V_GREETING,  PATH_V_GREETING,  "7. 问候与倒计时" }
    };

    const int total_voices = sizeof(voices) / sizeof(voices[0]);
    int success_count = 0;
    for (int i = 0; i < total_voices; i++) {
        printf("Synthesizing [%s] -> %s...\n", voices[i].desc, voices[i].path);
        // Remove old file first to ensure fresh synthesis
        unlink(voices[i].path);
        esp_err_t err = tts_xfyun_synthesize_to_file(voices[i].text, voices[i].path);
        if (err == ESP_OK) {
            printf("Successfully updated %s\n", voices[i].path);
            success_count++;
        } else {
            printf("Failed to update %s: %s\n", voices[i].path, esp_err_to_name(err));
        }
    }

    printf("Local voice update completed. Success: %d/%d\n", success_count, total_voices);
    return (success_count == total_voices) ? 0 : 1;
}
