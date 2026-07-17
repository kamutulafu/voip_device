/*
 * Remote MP3 playback: download an MP3 over HTTP(S), decode with the
 * OpenCore-based micro-mp3 decoder (handles MPEG 1/2/2.5 Layer III, sync,
 * ID3 skipping, and framing internally), and stream the PCM to the speaker
 * at the file's native sample rate.
 */

#include "audio_mp3.h"

extern "C" {
#include "audio_driver.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
}

#include <stdlib.h>
#include <string.h>

#include "micro_mp3/mp3_decoder.h"

static const char *TAG = "audio_mp3";

/* ---- download the whole compressed file into a (PSRAM) buffer ---------- */

struct dl_ctx_t {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
};

static esp_err_t dl_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        dl_ctx_t *d = static_cast<dl_ctx_t *>(evt->user_data);
        if (!d) return ESP_OK;
        if (d->len + evt->data_len > d->cap) {
            size_t nc = d->cap ? d->cap * 2 : 32768;
            while (d->len + evt->data_len > nc) nc *= 2;
            uint8_t *t = static_cast<uint8_t *>(heap_caps_realloc(d->buf, nc, MALLOC_CAP_SPIRAM));
            if (!t) t = static_cast<uint8_t *>(realloc(d->buf, nc));
            if (!t) return ESP_ERR_NO_MEM;
            d->buf = t;
            d->cap = nc;
        }
        memcpy(d->buf + d->len, evt->data, evt->data_len);
        d->len += evt->data_len;
    }
    return ESP_OK;
}

static esp_err_t download_all(const char *url, uint8_t **out, size_t *out_len)
{
    int64_t start_time = esp_timer_get_time();
    dl_ctx_t d = {nullptr, 0, 0};
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 20000;
    cfg.event_handler = dl_event;
    cfg.user_data = &d;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;
    if (err != ESP_OK || status != 200 || d.len == 0) {
        ESP_LOGE(TAG, "download failed after %lld ms: err=%s status=%d len=%u",
                 elapsed_ms, esp_err_to_name(err), status, (unsigned)d.len);
        free(d.buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "downloaded %u bytes from %s (Took %lld ms)", (unsigned)d.len, url, elapsed_ms);
    *out = d.buf;
    *out_len = d.len;
    return ESP_OK;
}

/* ---- decode + play ----------------------------------------------------- */

extern "C" esp_err_t audio_play_mp3_url(const char *url)
{
    if (!url || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char *temp_url = strdup(url);
    if (temp_url) {
        if (strncmp(temp_url, "https://", 8) == 0) {
            // Replace https:// with http:// to speed up downloading from COS by skipping SSL handshake
            memmove(temp_url + 4, temp_url + 5, strlen(temp_url + 5) + 1);
        }
    }
    const char *url_to_download = temp_url ? temp_url : url;

    uint8_t *mp3 = nullptr;
    size_t mp3_len = 0;
    if (download_all(url_to_download, &mp3, &mp3_len) != ESP_OK) {
        free(temp_url);
        return ESP_FAIL;
    }
    free(temp_url);

    micro_mp3::Mp3Decoder decoder;

    /* PCM output buffer (caller-owned), and a mono downmix scratch buffer. */
    int16_t *pcm  = static_cast<int16_t *>(malloc(micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES));
    int16_t *mono = static_cast<int16_t *>(malloc(micro_mp3::MP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t)));
    if (!pcm || !mono) {
        ESP_LOGE(TAG, "pcm buffer alloc failed");
        free(pcm); free(mono); free(mp3);
        return ESP_ERR_NO_MEM;
    }

    const uint8_t *in = mp3;
    size_t in_len = mp3_len;
    bool started = false;
    int frames_ok = 0;

    while (in_len > 0) {
        size_t consumed = 0, samples = 0;
        micro_mp3::Mp3Result r = decoder.decode(
            in, in_len,
            reinterpret_cast<uint8_t *>(pcm), micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES,
            consumed, samples);

        in     += consumed;
        in_len -= consumed;

        if (r == micro_mp3::MP3_STREAM_INFO_READY) {
            ESP_LOGI(TAG, "mp3: %d Hz, %d ch", decoder.get_sample_rate(), decoder.get_channels());
            continue; /* format known, no PCM yet */
        }
        if (r == micro_mp3::MP3_DECODE_ERROR) {
            if (consumed == 0) break; /* no progress -> bail */
            continue;                 /* skip corrupt frame */
        }
        if (r < 0) {
            ESP_LOGW(TAG, "fatal decode result %d", (int)r);
            break;
        }

        if (samples > 0) {
            if (!started) {
                if (audio_play_pcm_begin(decoder.get_sample_rate()) != ESP_OK) {
                    break;
                }
                started = true;
            }
            int ch = decoder.get_channels();
            if (ch == 2) {
                size_t n = samples;
                if (n > (size_t)micro_mp3::MP3_MAX_SAMPLES_PER_FRAME) {
                    n = micro_mp3::MP3_MAX_SAMPLES_PER_FRAME;
                }
                for (size_t i = 0; i < n; i++) {
                    mono[i] = (int16_t)(((int)pcm[2 * i] + (int)pcm[2 * i + 1]) / 2);
                }
                audio_play_pcm_write(mono, n, 1);
            } else {
                audio_play_pcm_write(pcm, samples, 1);
            }
            frames_ok++;
        }

        if (consumed == 0 && samples == 0) {
            break; /* need-more-data at end of buffer -> done */
        }
    }

    if (started) {
        audio_play_pcm_end();
    }

    free(pcm);
    free(mono);
    free(mp3);

    ESP_LOGI(TAG, "decode finished, frames_ok=%d", frames_ok);
    return started ? ESP_OK : ESP_FAIL;
}
