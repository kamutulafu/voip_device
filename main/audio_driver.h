#ifndef AUDIO_DRIVER_H
#define AUDIO_DRIVER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Default speaker volume (0-100). 75% balances loudness vs clipping on the PA. */
#define AUDIO_DEFAULT_VOLUME    55

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
 * @brief Flush and discard initial microphone/codec warm-up noise/samples from I2S RX channel.
 * @param warmup_ms Duration in milliseconds to flush (e.g. 50 ms).
 */
void audio_flush_rx_warmup(uint32_t warmup_ms);

/**
 * @brief Abort any active playback immediately.
 */
void audio_play_abort(void);

/**
 * @brief Clear the play abort flag.
 */
void audio_play_clear_abort(void);

/**
 * @brief Check whether the play abort flag is currently set, without
 *        modifying it. Intended for callers that stream audio chunk-by-chunk
 *        (e.g. TTS playback fed live from a network callback) and need to
 *        stop feeding new chunks as soon as audio_play_abort() is called by
 *        another task, the same way the built-in audio_play_from_file() /
 *        audio_play_from_mem() loops already do internally.
 *
 * @return true if audio_play_abort() was called and not yet cleared.
 */
bool audio_play_is_aborted(void);

/**
 * @brief Start the decoupled Audio Queue and Playback Consumer task for streaming TTS.
 * @param sample_rate Sample rate (e.g. 16000 Hz)
 * @return ESP_OK on success
 */
esp_err_t audio_queue_start(uint32_t sample_rate);

/**
 * @brief Push a PCM chunk into the Audio Queue (producer callback).
 * @param pcm Pointer to int16 PCM samples
 * @param num_samples Number of samples
 * @param is_last Set to true if this is the final chunk
 * @return ESP_OK on success
 */
esp_err_t audio_queue_push(const int16_t *pcm, size_t num_samples, bool is_last);

/**
 * @brief Signal completion of TTS stream (pushes final end chunk).
 */
void audio_queue_finish(void);

/**
 * @brief Wait for the Playback Consumer to finish playing all queued audio chunks.
 * @param timeout_ms Max time to wait in milliseconds
 * @return ESP_OK if finished cleanly, ESP_ERR_TIMEOUT if timed out
 */
esp_err_t audio_queue_wait_done(uint32_t timeout_ms);

/**
 * @brief Play a WAV audio file from SPIFFS using the speaker
 * @param filename The path/name of the WAV file to play (e.g. "/spiffs/rec.wav")
 * @return ESP_OK on success
 */
esp_err_t audio_play_from_file(const char *filename);

/**
 * @brief Record audio from microphone directly to a memory buffer as a WAV file.
 *
 * The buffer is allocated on the PSRAM heap and must be freed by the caller.
 *
 * @param out_buf       [out] Pointer that receives the allocated buffer
 * @param out_len       [out] Pointer that receives the total size of the WAV data
 * @param duration_sec  Duration of the recording in seconds
 * @return ESP_OK on success
 */
esp_err_t audio_record_to_mem(uint8_t **out_buf, size_t *out_len, uint32_t duration_sec);

/**
 * @brief Play a WAV format audio buffer directly from memory.
 *
 * @param buf Pointer to the WAV format buffer
 * @param len Total length of the buffer
 * @return ESP_OK on success
 */
esp_err_t audio_play_from_mem(const uint8_t *buf, size_t len);

/**
 * @brief Set the speaker playback volume
 * @param volume Volume level (0 to 100 percent)
 * @return ESP_OK on success
 */
esp_err_t audio_set_volume(uint8_t volume);

/**
 * @brief 持久化扬声器音量偏好，不要求音频硬件已初始化
 *
 * 用于 AP 配网等音频通路尚未建立的场景：音量一定会写入 NVS，
 * 若 codec 已经初始化则同时立即生效。
 *
 * @param volume Volume level (0 to 100 percent)
 * @return ESP_OK 写入 NVS 成功
 */
esp_err_t audio_store_volume(uint8_t volume);

/**
 * @brief Begin streaming PCM playback at an arbitrary sample rate.
 *
 * Reconfigures the I2S TX clock to @p sample_rate. Pair with
 * audio_play_pcm_write() and audio_play_pcm_end(). The clock is restored to the
 * default (16 kHz) by audio_play_pcm_end() so ASR/TTS keep working.
 *
 * @param sample_rate Playback sample rate in Hz (e.g. 44100, 22050, 16000)
 * @return ESP_OK on success
 */
esp_err_t audio_play_pcm_begin(uint32_t sample_rate);

/**
 * @brief Write 16-bit PCM samples to the speaker (used between begin/end).
 *
 * @param pcm         Pointer to int16 samples.
 * @param num_samples For mono: number of mono samples. For stereo: number of
 *                    interleaved int16 values (frames * 2).
 * @param channels    1 = mono (expanded to stereo), 2 = interleaved stereo.
 * @return ESP_OK on success
 */
esp_err_t audio_play_pcm_write(const int16_t *pcm, size_t num_samples, int channels);

/**
 * @brief End streaming PCM playback and restore the default 16 kHz clock.
 */
void audio_play_pcm_end(void);

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

/**
 * @brief Select the active mic channel with larger absolute amplitude from a stereo frame.
 */
static inline int16_t mic_pick_channel(int16_t left, int16_t right)
{
    int32_t abs_l = (left  < 0) ? -(int32_t)left  : (int32_t)left;
    int32_t abs_r = (right < 0) ? -(int32_t)right : (int32_t)right;
    return (abs_l >= abs_r) ? left : right;
}

/**
 * @brief Read raw frames directly from the I2S RX channel (16 kHz/16-bit/stereo).
 *        Ensures the audio system is initialized before reading.
 *
 * @param dest        Destination buffer
 * @param size        Number of bytes to read
 * @param bytes_read  [out] Number of bytes actually read
 * @param timeout_ms  Read timeout in milliseconds
 * @return ESP_OK on success
 */
esp_err_t audio_read_raw(void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms);

// Console command functions
int cmd_audio_record(int argc, char **argv);
int cmd_audio_play(int argc, char **argv);
int cmd_set_volume(int argc, char **argv);
int cmd_es8311_read(int argc, char **argv);

/**
 * @brief Read a single register from ES8311 via I2C
 */
esp_err_t audio_es8311_read_reg(uint8_t reg, uint8_t *val);

/**
 * @brief Dump ES8311 registers to console (useful for logic analyzer capture)
 */
esp_err_t audio_es8311_dump_registers(void);

/**
 * @brief Get the current speaker playback volume
 * @return Current volume level (0 to 100 percent)
 */
uint8_t audio_get_volume(void);

#endif // AUDIO_DRIVER_H
