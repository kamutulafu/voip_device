#ifndef DIALOGUE_H
#define DIALOGUE_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PATH_V_SYS_START   "/spiffs/v_sys_start.wav"
#define PATH_V_NET_OK      "/spiffs/v_net_ok.wav"
#define PATH_V_NET_ERR     "/spiffs/v_net_err.wav"
#define PATH_V_SYS_ERR     "/spiffs/v_sys_err.wav"
#define PATH_V_FACE_FAIL   "/spiffs/v_face_fail.wav"
#define PATH_V_NO_RESP     "/spiffs/v_no_resp.wav"
#define PATH_V_GREETING    "/spiffs/v_greeting.wav"
#define PATH_V_GOODBYE     "/spiffs/v_goodbye.wav"
#define PATH_V_CALL_END    "/spiffs/v_call_end.wav"

#define TEXT_V_SYS_START   "系统启动成功，正在检查网络"
#define TEXT_V_NET_OK      "网络连接成功"
#define TEXT_V_NET_ERR     "网络连接失败"
#define TEXT_V_SYS_ERR     "哎呀，系统开小差了"
#define TEXT_V_FACE_FAIL   "未识别到人脸"
#define TEXT_V_NO_RESP     "你好像不在这里，下次再找我吧，拜拜"
#define TEXT_V_GREETING    "小朋友您好，请正对摄像头站好，现在我要看看你是谁。三！二！一！"
#define TEXT_V_GOODBYE     "好的，有需要再找我，拜拜"
#define TEXT_V_CALL_END    "通话已结束"

/**
 * @brief Play a local WAV voice file from filesystem if it exists.
 *        Otherwise fall back to dynamic TTS synthesis.
 *
 * @param path Absolute path to the WAV file in SPIFFS (e.g. "/spiffs/v_sys_start.wav")
 * @param fallback_text UTF-8 text to speak if file does not exist or fails to play.
 * @return ESP_OK on success.
 */
/**
 * @brief Initialize the serialized speech queue + worker task.
 *        Must be called once at startup before any speak/play call.
 */
void dialogue_audio_init(void);

/**
 * @brief Abort all pending/in-progress speech: discards queued utterances and
 *        stops current playback. Used to cut the greeting short on recognition.
 */
void dialogue_abort_all(void);

esp_err_t play_local_voice(const char *path, const char *fallback_text);

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

/** 1.2 陌生人欢迎语。 */
void dialogue_welcome_unknown(void);

/** 1.2 公益用户在校外版设备的提示语。@p name 宝宝姓名。 */
void dialogue_welcome_public_offcampus(const char *name);

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

/**
 * @brief Console command to update the local voice files.
 *        Synthesizes the four local voice files and saves them to SPIFFS.
 */
int cmd_voice_update(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif // DIALOGUE_H
