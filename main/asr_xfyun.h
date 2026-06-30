#ifndef ASR_XFYUN_H
#define ASR_XFYUN_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Recognize speech from a 16 kHz / 16-bit mono PCM buffer using the
 *        iFlytek (讯飞) IAT (语音听写) WebSocket service.
 *
 * @param pcm           Pointer to 16-bit mono PCM samples (16 kHz)
 * @param num_samples   Number of int16 samples in the buffer
 * @param out_text      [out] Buffer that receives the recognized UTF-8 text
 * @param out_text_size Size of out_text in bytes
 * @return ESP_OK on success
 */
esp_err_t asr_xfyun_recognize(const int16_t *pcm, size_t num_samples,
                              char *out_text, size_t out_text_size);

/**
 * @brief Console command: record from the microphone and print the recognized text.
 *
 * Usage: asr_test [duration_sec]
 */
int cmd_asr_test(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif // ASR_XFYUN_H
