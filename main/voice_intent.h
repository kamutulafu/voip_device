#ifndef VOICE_INTENT_H
#define VOICE_INTENT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** High-level user intents parsed from recognized ASR text. */
typedef enum {
    INTENT_UNKNOWN = 0,
    INTENT_SEND_MSG,     // 发留言
    INTENT_CALL,         // 打电话 / 拨通
    INTENT_ADD_FRIEND,   // 加好友
    INTENT_REPLY,        // 我要回复
    INTENT_NO_REPLY,     // 不用回复
    INTENT_REPLAY,       // 重播 / 重听
    INTENT_YES,          // 是 / 正确 / 好
    INTENT_NO,           // 否 / 错误 / 不用
} voice_intent_t;

/**
 * @brief Parse recognized text into a high-level intent.
 *
 * Matching is keyword based on UTF-8 substrings. Returns INTENT_UNKNOWN when
 * @p text is empty or contains no known keyword.
 */
voice_intent_t voice_intent_parse(const char *text);

/**
 * @brief Detect a family-role relation code (baba/mama/zufu/...) mentioned in text.
 *
 * @param text     Recognized UTF-8 text (e.g. "给妈妈打电话").
 * @param out_code [out] Buffer receiving the relation code (e.g. "mama").
 * @param out_size Size of out_code.
 * @return true if a relation was recognized.
 */
bool voice_intent_detect_relation(const char *text, char *out_code, size_t out_size);

/** Map a relation code (e.g. "mama") to its Chinese display word (e.g. "妈妈"). */
const char *relation_code_to_cn(const char *code);

/** Map a relation code (e.g. "mama") to the English form used by saveCallLog (e.g. "mother"). */
const char *relation_code_to_en(const char *code);

/**
 * @brief Extract an 11-digit Chinese mobile number from recognized text.
 *
 * ASR often returns digits as Chinese numerals or with spaces; this helper
 * pulls ASCII digits out of the text. Returns true when exactly 11 digits
 * starting with '1' are found.
 */
bool voice_intent_extract_mobile(const char *text, char *out, size_t out_size);

/** Convenience: true when the recognized text is empty/whitespace only. */
bool voice_text_is_empty(const char *text);

#ifdef __cplusplus
}
#endif

#endif // VOICE_INTENT_H
