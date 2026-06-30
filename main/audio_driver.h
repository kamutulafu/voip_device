#ifndef AUDIO_DRIVER_H
#define AUDIO_DRIVER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Initialize the audio system (I2C, ES8311 Codec, and I2S)
 * @return ESP_OK on success, appropriate error code otherwise
 */
esp_err_t audio_init(void);

/**
 * @brief Deinitialize the audio system
 */
void audio_deinit(void);

/**
 * @brief Record audio from microphone and save to a WAV file in SPIFFS
 * @param filename The path/name of the file to save (e.g. "/spiffs/rec.wav")
 * @param duration_sec Duration of the recording in seconds
 * @return ESP_OK on success
 */
esp_err_t audio_record_to_file(const char *filename, uint32_t duration_sec);

/**
 * @brief Play a WAV audio file from SPIFFS using the speaker
 * @param filename The path/name of the WAV file to play (e.g. "/spiffs/rec.wav")
 * @return ESP_OK on success
 */
esp_err_t audio_play_from_file(const char *filename);

/**
 * @brief Set the speaker playback volume
 * @param volume Volume level (0 to 100 percent)
 * @return ESP_OK on success
 */
esp_err_t audio_set_volume(uint8_t volume);

/**
 * @brief Record audio from the microphone into a 16 kHz / 16-bit mono PCM buffer.
 *
 * The buffer is allocated with malloc (SPIRAM-capable heap) and must be freed
 * by the caller with free().
 *
 * @param out_buf         [out] Pointer that receives the allocated PCM buffer
 * @param out_num_samples [out] Pointer that receives the number of int16 samples
 * @param duration_sec    Duration of the recording in seconds
 * @return ESP_OK on success
 */
esp_err_t audio_record_mono_pcm(int16_t **out_buf, size_t *out_num_samples, uint32_t duration_sec);

// Console command functions
int cmd_audio_record(int argc, char **argv);
int cmd_audio_play(int argc, char **argv);

#endif // AUDIO_DRIVER_H
