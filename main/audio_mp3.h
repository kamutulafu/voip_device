#ifndef AUDIO_MP3_H
#define AUDIO_MP3_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Download an MP3 from an HTTP(S) URL, decode it, and play it on the
 *        speaker at the MP3's native sample rate.
 *
 * Streams frame-by-frame: the whole compressed file is downloaded to (PSRAM)
 * memory, then decoded and pushed to I2S one frame at a time to bound RAM use.
 * Stereo is down-mixed to mono. Requires WiFi.
 *
 * @param url HTTP(S) URL of the MP3 resource.
 * @return ESP_OK if at least one frame was played.
 */
esp_err_t audio_play_mp3_url(const char *url);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_MP3_H
