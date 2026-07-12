#ifndef DIALOGUE_H
#define DIALOGUE_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Synthesize UTF-8 text with iFlytek TTS and play it on the speaker.
 *        Blocks until playback finishes. Requires WiFi.
 *
 * @param text UTF-8 text to speak.
 * @return ESP_OK on success.
 */
esp_err_t dialogue_speak(const char *text);

/**
 * @brief printf-style variant of dialogue_speak().
 */
esp_err_t dialogue_speak_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief Play a short "滴" prompt tone (滴声). Generates a beep WAV on first use.
 *        Best-effort: returns without error even if the tone cannot be played.
 */
void dialogue_beep(void);

/* ============================================================
 * Reusable dialogue templates (see design doc section 1).
 * ============================================================ */

/** 1.1 打招呼：唤醒后、人脸识别前的问候 + 倒计时。 */
void dialogue_greeting(void);

/** 1.2 识别成功、无留言的欢迎语（菜单）。@p name 宝宝姓名。 */
void dialogue_welcome_menu(const char *name);

/** 1.2 识别失败的受限菜单欢迎语（仅公益留言）。 */
void dialogue_welcome_unknown(void);

/** 1.3 长时间无回应的告别语（挂断前）。 */
void dialogue_timeout_bye(void);

/** 1.4 信息确认模板：确认号码/联系人等关键信息。
 *  @param content 要确认的内容
 *  @param with_beep 是否带"滴声"提示（陌生人场景带滴声，好友场景可不带）
 */
void dialogue_confirm_info(const char *content, bool with_beep);

/** 1.4 未识别输入 / ASR 失败的重试提示（含滴声）。 */
void dialogue_ask_retry(void);

/** 1.5 统一告别语。 */
void dialogue_farewell(void);

#ifdef __cplusplus
}
#endif

#endif // DIALOGUE_H
