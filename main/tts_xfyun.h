#ifndef TTS_XFYUN_H
#define TTS_XFYUN_H

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Synthesize UTF-8 text to speech using the iFlytek (讯飞) online TTS
 *        (在线语音合成) WebSocket service and store the result as a 16 kHz /
 *        16-bit mono WAV file.
 *
 * @param text      UTF-8 text to synthesize
 * @param filename  Output WAV file path (e.g. "/spiffs/tts.wav")
 * @return ESP_OK on success
 */
esp_err_t tts_xfyun_synthesize_to_file(const char *text, const char *filename);

/**
 * @brief Synthesize UTF-8 text to speech directly to a memory buffer (WAV format).
 *
 * The buffer is allocated on the PSRAM heap and must be freed by the caller.
 *
 * @param text     UTF-8 text to synthesize
 * @param out_buf  [out] Pointer that receives the allocated WAV format buffer
 * @param out_len  [out] Pointer that receives the total size of the WAV data
 * @return ESP_OK on success
 */
esp_err_t tts_xfyun_synthesize_to_mem(const char *text, uint8_t **out_buf, size_t *out_len);

/**
 * @brief Console command: synthesize text to speech and play it on the speaker.
 *
 * Usage: tts_test <text...>
 */
int cmd_tts_test(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif // TTS_XFYUN_H
