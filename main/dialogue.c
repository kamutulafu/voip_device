#include "dialogue.h"
#include "tts_xfyun.h"
#include "audio_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "esp_log.h"

static const char *TAG = "dialogue";

/* Scratch file reused for every TTS utterance. */
#define TTS_PLAY_PATH   "/spiffs/tts_play.wav"
#define BEEP_PATH       "/spiffs/beep.wav"

esp_err_t dialogue_speak(const char *text)
{
    if (!text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "TTS: %s", text);

    esp_err_t err = tts_xfyun_synthesize_to_file(text, TTS_PLAY_PATH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS synthesis failed: %s", esp_err_to_name(err));
        return err;
    }

    err = audio_play_from_file(TTS_PLAY_PATH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS playback failed: %s", esp_err_to_name(err));
    }
    return err;
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
    if (ensure_beep_file() == ESP_OK) {
        audio_play_from_file(BEEP_PATH);
    } else {
        ESP_LOGW(TAG, "beep tone unavailable");
    }
}

/* ============================================================
 * Templates
 * ============================================================ */

void dialogue_greeting(void)
{
    dialogue_speak("小朋友您好，请正对摄像头站好，现在我要看看你是谁。3！2！1！");
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
    dialogue_speak("您好像没有说话，我先溜了哦，有需要再找我，拜拜~");
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
