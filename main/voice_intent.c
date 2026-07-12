#include "voice_intent.h"

#include <string.h>
#include <ctype.h>

/* ---- relation table ---------------------------------------------------- */

typedef struct {
    const char *code;   /* backend relation code           */
    const char *cn;     /* canonical Chinese display word   */
    const char *en;     /* English form used by saveCallLog */
    const char *aliases[4]; /* spoken variants (NULL-terminated list) */
} relation_entry_t;

/* Relation codes per API doc:
 *   baba/mama/zufu/zumu/waizufu/waizumu/xiongdi/jiemei/pengyou/moshengren/erzi/nver
 * English mapping per saveCallLog: father/mother/grandpa/grandma/brother/sister/friend */
static const relation_entry_t k_relations[] = {
    {"baba",      "爸爸", "father",  {"爸爸", "爸", "父亲", NULL}},
    {"mama",      "妈妈", "mother",  {"妈妈", "妈", "母亲", NULL}},
    {"zufu",      "爷爷", "grandpa", {"爷爷", "祖父", "阿公", NULL}},
    {"zumu",      "奶奶", "grandma", {"奶奶", "祖母", NULL, NULL}},
    {"waizufu",   "姥爷", "grandpa", {"姥爷", "外公", "外祖父", NULL}},
    {"waizumu",   "姥姥", "grandma", {"姥姥", "外婆", "外祖母", NULL}},
    {"xiongdi",   "兄弟", "brother", {"哥哥", "弟弟", "兄弟", NULL}},
    {"jiemei",    "姐妹", "sister",  {"姐姐", "妹妹", "姐妹", NULL}},
    {"pengyou",   "朋友", "friend",  {"朋友", NULL, NULL, NULL}},
    {"erzi",      "儿子", "friend",  {"儿子", NULL, NULL, NULL}},
    {"nver",      "女儿", "friend",  {"女儿", NULL, NULL, NULL}},
    {"moshengren","陌生人","friend", {"陌生人", NULL, NULL, NULL}},
};

#define NUM_RELATIONS (sizeof(k_relations) / sizeof(k_relations[0]))

static bool contains(const char *hay, const char *needle)
{
    if (!hay || !needle) return false;
    return strstr(hay, needle) != NULL;
}

bool voice_text_is_empty(const char *text)
{
    if (!text) return true;
    for (const char *p = text; *p; p++) {
        if (!isspace((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

voice_intent_t voice_intent_parse(const char *text)
{
    if (voice_text_is_empty(text)) {
        return INTENT_UNKNOWN;
    }

    /* Order matters: negations and more specific phrases MUST be checked before
     * the shorter positive phrases they contain (e.g. "不用回复" contains "回复",
     * "不是" contains "是"). */

    /* replay / re-listen */
    if (contains(text, "重播") || contains(text, "重听") || contains(text, "再听") ||
        contains(text, "再放") || contains(text, "重新播") || contains(text, "再播")) {
        return INTENT_REPLAY;
    }
    /* "不用回复" / "不回复" / "不用" -> NO_REPLY  (before the REPLY check) */
    if (contains(text, "不用回复") || contains(text, "不回复") ||
        contains(text, "不需要回复") || contains(text, "不用")) {
        return INTENT_NO_REPLY;
    }
    /* "我要回复" / "回复" */
    if (contains(text, "我要回复") || contains(text, "要回复") || contains(text, "回复")) {
        return INTENT_REPLY;
    }
    if (contains(text, "发留言") || contains(text, "留言") || contains(text, "发消息")) {
        return INTENT_SEND_MSG;
    }
    if (contains(text, "加好友") || contains(text, "交朋友") || contains(text, "加朋友")) {
        return INTENT_ADD_FRIEND;
    }
    if (contains(text, "打电话") || contains(text, "打给") || contains(text, "拨通") ||
        contains(text, "拨打") || contains(text, "通话") || contains(text, "电话")) {
        return INTENT_CALL;
    }
    /* Negative confirmations first (they contain positive substrings). */
    if (contains(text, "错误") || contains(text, "不对") || contains(text, "不是") ||
        contains(text, "不要") || contains(text, "否") || contains(text, "不")) {
        return INTENT_NO;
    }
    if (contains(text, "正确") || contains(text, "是的") || contains(text, "是") ||
        contains(text, "对") || contains(text, "好的") || contains(text, "好") ||
        contains(text, "要")) {
        return INTENT_YES;
    }
    return INTENT_UNKNOWN;
}

bool voice_intent_detect_relation(const char *text, char *out_code, size_t out_size)
{
    if (voice_text_is_empty(text) || !out_code || out_size == 0) {
        return false;
    }
    for (size_t i = 0; i < NUM_RELATIONS; i++) {
        const relation_entry_t *r = &k_relations[i];
        for (int a = 0; a < 4 && r->aliases[a]; a++) {
            if (contains(text, r->aliases[a])) {
                strlcpy(out_code, r->code, out_size);
                return true;
            }
        }
    }
    return false;
}

const char *relation_code_to_cn(const char *code)
{
    if (!code) return "";
    for (size_t i = 0; i < NUM_RELATIONS; i++) {
        if (strcmp(code, k_relations[i].code) == 0) {
            return k_relations[i].cn;
        }
    }
    return code;
}

const char *relation_code_to_en(const char *code)
{
    if (!code) return "";
    for (size_t i = 0; i < NUM_RELATIONS; i++) {
        if (strcmp(code, k_relations[i].code) == 0) {
            return k_relations[i].en;
        }
        /* Accept an English code passthrough as well. */
        if (strcmp(code, k_relations[i].en) == 0) {
            return k_relations[i].en;
        }
    }
    return code;
}

bool voice_intent_extract_mobile(const char *text, char *out, size_t out_size)
{
    if (voice_text_is_empty(text) || !out || out_size < 12) {
        return false;
    }
    char digits[64];
    size_t n = 0;
    for (const char *p = text; *p && n < sizeof(digits) - 1; p++) {
        if (*p >= '0' && *p <= '9') {
            digits[n++] = *p;
        }
    }
    digits[n] = '\0';

    if (n == 11 && digits[0] == '1') {
        strlcpy(out, digits, out_size);
        return true;
    }
    return false;
}
