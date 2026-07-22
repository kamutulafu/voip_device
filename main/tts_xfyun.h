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
 * @brief Synthesize UTF-8 text to speech and stream it directly to the
 *        speaker as audio chunks arrive from the iFlytek TTS service,
 *        instead of buffering the whole utterance in memory first.
 *
 * This cuts perceived latency dramatically for long replies: playback
 * starts as soon as the first audio chunk arrives rather than only after
 * the entire synthesis finishes (which, for a long reply, previously took
 * several seconds of dead silence before anything was heard).
 *
 * Internally wraps the call in audio_play_pcm_begin()/audio_play_pcm_end();
 * the caller must not call those separately for this utterance.
 *
 * Playback (and the underlying network request) stops early, cleanly, if
 * audio_play_abort() is called from another task while streaming is still
 * in progress (checked once per received audio chunk). This is treated as
 * a normal, non-error outcome (ESP_OK), matching the existing behavior of
 * audio_play_from_file() / audio_play_from_mem() when aborted.
 *
 * @param text UTF-8 text to synthesize and speak.
 * @return ESP_OK on success (including a clean early abort), an error
 *         code if the TTS request itself failed (bad auth, network error,
 *         iFlytek returned an error code, etc).
 */
esp_err_t tts_xfyun_synthesize_stream(const char *text);

/**
 * @brief Console command: synthesize text to speech and play it on the speaker.
 *
 * Usage: tts_test <text...>
 */
int cmd_tts_test(int argc, char **argv);

/**
 * @brief Console command: synthesize text to speech and stream it to the
 *        speaker as it arrives (same path used by the real dialogue flow).
 *        Useful for A/B timing comparisons against tts_test.
 *
 * Usage: tts_test_stream <text...>
 */
int cmd_tts_test_stream(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif // TTS_XFYUN_H
