/*
 * Voice interaction state machine for the child self-rescue device.
 *
 * Implements the flow described in the design doc:
 *   IDLE -> GREETING(face recognition) -> {MENU | MESSAGES | RESTRICTED} ->
 *   {send message | add friend | call contact} -> FAREWELL -> IDLE
 *
 * Wake sources:
 *   - physical button on WAKE_BUTTON_GPIO (GPIO45 / pin 73)
 *   - console command "voice_wake" (voice_flow_wake())
 * A dedicated FreeRTOS task runs one session at a time.
 */

#include "voice_flow.h"
#include "dialogue.h"
#include "voice_intent.h"

#include "api_service.h"
#include "api_crypto.h"
#include "asr_xfyun.h"
#include "audio_driver.h"
#include "camera_capture.h"
#include "uvc_stream.h"
#include "device_config.h"
#include "console.h"
#include "esp_console.h"

#include "voip_client.h"
#include "voip_media.h"
#include "audio_mp3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "voice_flow";

/* ---- tunables ---------------------------------------------------------- */
#define LISTEN_RECORD_SEC       6      /* recording window per listen attempt   */
#define MAX_ASR_RETRY           3      /* 1.4 ask_retry maximum attempts        */
#define MAX_CONTACTS            8
#define MAX_MSGS                16
#define FACE_IMG_PATH           "/spiffs/face.jpg"
#define REPLY_WAV_PATH          "/spiffs/reply.wav"
#define CALL_POLL_TOTAL_MS      60000  /* poll VoIP status up to 60s            */
#define CALL_POLL_INTERVAL_MS   2000

/* ---- data model -------------------------------------------------------- */

typedef struct {
    char mobile[16];
    char relation[16];   /* relation code: baba/mama/...  */
    char user_id[40];
    char user_name[40];
    char wx_openid[64];
    int  answering_type; /* 0=phone, 1=wechat */
} contact_t;

typedef struct {
    char id[48];
    char from_id[40];
    char from_name[40];
    char relation[16];
    char sound_path[256];
} leave_msg_t;

typedef struct {
    char session_id[64];
    char baby_id[40];
    char baby_name[40];
    int  is_reg_user;
    int  is_reg_stranger;
    bool face_ok;         /* recognition succeeded */

    contact_t contacts[MAX_CONTACTS];
    int contact_count;

    leave_msg_t msgs[MAX_MSGS];
    int msg_count;

    int retry_count;
    bool timed_out;       /* True if dialogue timed out (user didn't speak) */
} session_t;

static session_t s_sess;
static volatile bool s_session_active = false;

/* Wake button (GPIO45): ISR posts to queue, task debounces and calls wake. */
static QueueHandle_t s_wake_btn_queue = NULL;

/* ---- small helpers ----------------------------------------------------- */

static void now_str(char *out, size_t sz)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, sz, "%Y-%m-%d %H:%M:%S", &tm);
}

/* Return a readable cJSON "data" object, transparently RSA-decrypting the
 * common {"data":"<base64 ciphertext>"} form. When *owned is set true the
 * caller must cJSON_Delete() the returned object. */
static cJSON *result_data_object(api_result_t *res, bool *owned)
{
    *owned = false;
    if (!res || !res->data) {
        return NULL;
    }
    if (cJSON_IsString(res->data)) {
        char *plain = api_crypto_rsa_decrypt(res->data->valuestring);
        if (!plain) {
            ESP_LOGE(TAG, "RSA decrypt of data failed");
            return NULL;
        }
        cJSON *j = cJSON_Parse(plain);
        free(plain);
        *owned = true;
        return j;
    }
    return res->data;
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : NULL;
}

static int json_int(cJSON *obj, const char *key, int dflt)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) return it->valueint;
    if (cJSON_IsString(it) && it->valuestring) return atoi(it->valuestring);
    return dflt;
}

/* ---- listening --------------------------------------------------------- */

/* Record one window and recognize. Returns true when non-empty text obtained. */
static bool listen_once(char *out, size_t sz)
{
    out[0] = '\0';
    int16_t *pcm = NULL;
    size_t n = 0;
    if (audio_record_mono_pcm(&pcm, &n, LISTEN_RECORD_SEC) != ESP_OK) {
        ESP_LOGE(TAG, "recording failed");
        return false;
    }

    // Local VAD (Voice Activity Detection): compute average absolute amplitude to check for silence
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += abs(pcm[i]);
    }
    int32_t avg_amp = sum / n;
    ESP_LOGI(TAG, "Recorded audio average amplitude: %d", (int)avg_amp);

    if (avg_amp < 80) {
        ESP_LOGI(TAG, "Silence detected (amplitude < 80). Skipping cloud ASR request.");
        free(pcm);
        return false;
    }

    esp_err_t err = asr_xfyun_recognize(pcm, n, out, sz);
    free(pcm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ASR failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "heard: '%s'", out);
    return !voice_text_is_empty(out);
}

/* Listen with the 1.4 retry template. Returns true on success; on false the
 * caller should play the 1.3 timeout farewell. */
static bool listen_with_retry(char *out, size_t sz)
{
    for (int attempt = 0; attempt < MAX_ASR_RETRY; attempt++) {
        if (listen_once(out, sz)) {
            return true;
        }
        if (attempt < MAX_ASR_RETRY - 1) {
            dialogue_ask_retry();
        }
    }
    return false;
}

static void voice_flow_timeout_exit(session_t *s)
{
    dialogue_timeout_bye();
    s->timed_out = true;
}

/* ---- parsing backend responses ---------------------------------------- */

static void parse_contacts(cJSON *data, session_t *s)
{
    s->contact_count = 0;
    cJSON *contacts = cJSON_GetObjectItemCaseSensitive(data, "contacts");
    if (!cJSON_IsArray(contacts)) {
        return;
    }
    cJSON *c = NULL;
    cJSON_ArrayForEach(c, contacts) {
        if (s->contact_count >= MAX_CONTACTS) break;
        contact_t *ct = &s->contacts[s->contact_count];
        memset(ct, 0, sizeof(*ct));

        const char *v;
        if ((v = json_str(c, "mobile")))   strlcpy(ct->mobile, v, sizeof(ct->mobile));
        if ((v = json_str(c, "relation"))) strlcpy(ct->relation, v, sizeof(ct->relation));
        if ((v = json_str(c, "userId")))   strlcpy(ct->user_id, v, sizeof(ct->user_id));
        if ((v = json_str(c, "userName"))) strlcpy(ct->user_name, v, sizeof(ct->user_name));
        /* doc uses both wxOpenId (searchFace2) and wxOpenid (friend list) */
        if ((v = json_str(c, "wxOpenId")) || (v = json_str(c, "wxOpenid"))) {
            strlcpy(ct->wx_openid, v, sizeof(ct->wx_openid));
        }
        ct->answering_type = json_int(c, "answeringType", 0);
        s->contact_count++;
    }
}

/* Populate the session from a searchFace2 result. Returns true when a baby was
 * recognized (code == "0" and a data object present). */
static bool handle_face_result(api_result_t *res, session_t *s)
{
    if (!res) {
        return false;
    }
    ESP_LOGI(TAG, "searchFace2 HTTP=%d code=%s msg=%s raw=%s",
             res->http_status, res->code ? res->code : "?", res->msg ? res->msg : "?",
             res->raw_body ? res->raw_body : "");

    /* code != "0" -> failure (face fuzzy / no user / stranger reg fail). */
    if (!res->code || strcmp(res->code, "0") != 0) {
        return false;
    }

    bool owned = false;
    cJSON *data = result_data_object(res, &owned);
    if (!data) {
        return false;
    }

    const char *v;
    if ((v = json_str(data, "babyName"))) strlcpy(s->baby_name, v, sizeof(s->baby_name));
    if ((v = json_str(data, "babyId")))   strlcpy(s->baby_id, v, sizeof(s->baby_id));
    /* session id key is "sessionid" (lower-case) per doc */
    if ((v = json_str(data, "sessionid")) || (v = json_str(data, "sessionId"))) {
        strlcpy(s->session_id, v, sizeof(s->session_id));
    }
    s->is_reg_user     = json_int(data, "isRegUser", 0);
    s->is_reg_stranger = json_int(data, "isRegStranger", 0);
    parse_contacts(data, s);

    if (owned) {
        cJSON_Delete(data);
    }

    /* Recognized when we have a session id and a name. */
    s->face_ok = (s->session_id[0] != '\0');
    return s->face_ok;
}

/* Fetch unread leave messages into the session. Returns the count. */
static int fetch_leave_msgs(session_t *s)
{
    s->msg_count = 0;
    api_result_t *res = api_get_leave_msg_page(s->session_id);
    if (!res) {
        return 0;
    }
    if (res->code && strcmp(res->code, "0") == 0 && res->data) {
        cJSON *list = cJSON_GetObjectItemCaseSensitive(res->data, "list");
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, list) {
            if (s->msg_count >= MAX_MSGS) break;
            leave_msg_t *m = &s->msgs[s->msg_count];
            memset(m, 0, sizeof(*m));
            const char *v;
            if ((v = json_str(item, "id")))        strlcpy(m->id, v, sizeof(m->id));
            if ((v = json_str(item, "fromId")))    strlcpy(m->from_id, v, sizeof(m->from_id));
            if ((v = json_str(item, "fromName")))  strlcpy(m->from_name, v, sizeof(m->from_name));
            if ((v = json_str(item, "relation")))  strlcpy(m->relation, v, sizeof(m->relation));
            if ((v = json_str(item, "soundPath"))) strlcpy(m->sound_path, v, sizeof(m->sound_path));
            s->msg_count++;
        }
    }
    api_result_free(res);
    ESP_LOGI(TAG, "unread leave messages: %d", s->msg_count);
    return s->msg_count;
}

/* ---- session heartbeat / lifecycle ------------------------------------ */

static void session_keep_alive(session_t *s)
{
    if (s->session_id[0]) {
        api_result_t *r = api_keep_ss(s->session_id);
        if (r) api_result_free(r);
    }
}

static void session_close(session_t *s)
{
    if (s->session_id[0]) {
        api_result_t *r = api_close_ss(s->session_id);
        if (r) api_result_free(r);
    }
}

/* ---- VoIP call helper -------------------------------------------------- */

typedef enum {
    CALL_ERR = -1,
    CALL_ANSWERED_ENDED,  /* was answered, then the call ended normally */
    CALL_NO_ANSWER,       /* rejected / busy / ring-timeout             */
    CALL_POWEROFF,        /* callee offline / aborted                   */
} call_result_t;

/* Place a WeChat VoIP call to @p openid and follow it to completion.
 * Returns a coarse result category the flow can react to. */
static call_result_t place_voip_call(const char *openid)
{
    if (!openid || openid[0] == '\0') {
        ESP_LOGW(TAG, "no wxOpenId for callee");
        return CALL_ERR;
    }

    char sn_ticket[256] = {0};
    if (voip_fetch_sn_ticket(DEVICE_ID, VOIP_MODEL_ID, sn_ticket, sizeof(sn_ticket)) != WXERROR_OK) {
        ESP_LOGE(TAG, "voip_fetch_sn_ticket failed");
        return CALL_ERR;
    }

    wx_error_t r = voip_client_call(DEVICE_ID, VOIP_MODEL_ID, VOIP_APPID, sn_ticket,
                                    openid, VOIP_PAYLOAD, true);
    if (r != WXERROR_OK) {
        ESP_LOGE(TAG, "voip_client_call failed: %d", r);
        return CALL_ERR;
    }

    /* Poll the proxy for the outcome. Status codes (see voip_media.c):
     *   2=talking, 3=rejected, 5=hangup(caller), 6=hangup(callee),
     *   7=aborted/poweroff, 8=busy, 9=ring timeout */
    bool answered = false;
    int elapsed = 0;
    while (elapsed < CALL_POLL_TOTAL_MS) {
        int st = voip_get_call_status(VOIP_PAYLOAD);
        ESP_LOGI(TAG, "call status = %d", st);
        if (st == 2) {
            answered = true;
        } else if (answered && (st == 5 || st == 6)) {
            return CALL_ANSWERED_ENDED;
        } else if (st == 3 || st == 8 || st == 9) {
            return CALL_NO_ANSWER;
        } else if (st == 7) {
            return CALL_POWEROFF;
        }
        vTaskDelay(pdMS_TO_TICKS(CALL_POLL_INTERVAL_MS));
        elapsed += CALL_POLL_INTERVAL_MS;
    }
    return answered ? CALL_ANSWERED_ENDED : CALL_NO_ANSWER;
}

/* Find a contact by relation code. Returns NULL when not present. */
static contact_t *find_contact_by_relation(session_t *s, const char *code)
{
    for (int i = 0; i < s->contact_count; i++) {
        if (strcmp(s->contacts[i].relation, code) == 0) {
            return &s->contacts[i];
        }
    }
    return NULL;
}

/* ---- leave message playback / upload ---------------------------------- */

static void extract_url(api_result_t *res, char *out, size_t sz)
{
    out[0] = '\0';
    if (res && res->data) {
        const char *u = json_str(res->data, "imgUrl");
        if (u) strlcpy(out, u, sz);
    }
}

/* Play a received leave message (remote MP3). */
static void play_leave_msg(const leave_msg_t *m)
{
    ESP_LOGI(TAG, "playing leave msg id=%s from=%s url=%s", m->id, m->from_name, m->sound_path);

    if (m->sound_path[0] == '\0') {
        dialogue_speak("这条留言没有语音内容哦");
        return;
    }

    if (audio_play_mp3_url(m->sound_path) != ESP_OK) {
        ESP_LOGW(TAG, "mp3 playback failed for %s", m->sound_path);
        dialogue_speak("留言播放失败了呢");
    }
}

static void mark_msg_read(session_t *s, const leave_msg_t *m)
{
    char t[32];
    now_str(t, sizeof(t));
    api_result_t *r = api_mark_leave_msg_readed(m->id, s->session_id, t, DEVICE_ID);
    if (r) api_result_free(r);
}

/* Record a message from the child, upload photo+audio, and save it.
 * direction per doc: "0"=child->parent, "4"=child->stranger phone. */
static void record_upload_save(session_t *s, const char *receiver_id,
                               const char *relation_code, const char *direction,
                               const char *reply_id)
{
    dialogue_speak("好嘞！请说出您的留言，说完说over");

    const uint32_t rec_sec = 8;
    uint8_t *audio_buf = NULL;
    size_t audio_len = 0;
    if (audio_record_to_mem(&audio_buf, &audio_len, rec_sec) != ESP_OK || audio_len == 0) {
        ESP_LOGE(TAG, "recording leave msg to memory failed");
        dialogue_speak("录音好像出问题了呢");
        return;
    }

    /* Best-effort scene photo. */
    char img_url[256] = "";
    char snd_url[256] = "";

    size_t photo_len = 0;
    uint8_t *photo_buf = heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM);
    if (!photo_buf) {
        photo_buf = malloc(128 * 1024);
    }
    if (photo_buf) {
        camera_capture_photo_mem(photo_buf, 128 * 1024, &photo_len);
    }

    if (photo_buf && photo_len > 0) {
        api_result_t *pr = api_post_calls_photo_mem(s->session_id, DEVICE_ID, photo_buf, photo_len);
        extract_url(pr, img_url, sizeof(img_url));
        if (pr) api_result_free(pr);
    } else {
        ESP_LOGW(TAG, "Skipping photo upload because captured photo is empty/invalid");
    }
    if (photo_buf) {
        free(photo_buf);
    }

    api_result_t *ar = api_post_calls_audio_mem(s->session_id, DEVICE_ID, audio_buf, audio_len);
    extract_url(ar, snd_url, sizeof(snd_url));
    if (ar) api_result_free(ar);
    free(audio_buf);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "sessionId", s->session_id);
    cJSON_AddStringToObject(o, "fromId", s->baby_id);
    cJSON_AddStringToObject(o, "receiverId", receiver_id ? receiver_id : "");
    cJSON_AddStringToObject(o, "relation", relation_code ? relation_code : "");
    cJSON_AddStringToObject(o, "direction", direction ? direction : "0");
    cJSON_AddNumberToObject(o, "duration", rec_sec);
    cJSON_AddStringToObject(o, "deviceId", DEVICE_ID);
    cJSON_AddStringToObject(o, "soundPath", snd_url);
    cJSON_AddStringToObject(o, "imgPath", img_url);
    cJSON_AddStringToObject(o, "replyId", reply_id ? reply_id : "newmsg");

    api_result_t *sr = api_save_leave_msg(s->session_id, o);
    cJSON_Delete(o);
    if (sr) api_result_free(sr);

    dialogue_speak("收到啦！已光速发送给对方");
}

/* ---- 2.1 / 2.2 message playback flow ---------------------------------- */

static void handle_messages(session_t *s)
{
    int n = s->msg_count;
    for (int i = 0; i < n; i++) {
        leave_msg_t *m = &s->msgs[i];

        /* Announce (首条与后续文案不同). */
        if (n == 1) {
            dialogue_speak_fmt("哇%s您好，很高兴为您服务，您有一条新留言，来自%s，现在开始播放",
                               s->baby_name[0] ? s->baby_name : "小朋友",
                               m->from_name[0] ? m->from_name : "家长");
        } else if (i == 0) {
            dialogue_speak_fmt("哇%s您好，您有%d条留言！现在开始播放第1条留言，来自%s",
                               s->baby_name[0] ? s->baby_name : "小朋友", n,
                               m->from_name[0] ? m->from_name : "家长");
        } else {
            dialogue_speak_fmt("第%d条留言，来自%s", i + 1,
                               m->from_name[0] ? m->from_name : "家长");
        }

        bool advance = false;
        while (!advance) {
            play_leave_msg(m);
            mark_msg_read(s, m);

            dialogue_speak("是否回复或重听？请说我要回复、不用回复或者重播，说完请说over");
            char text[256];
            if (!listen_with_retry(text, sizeof(text))) {
                voice_flow_timeout_exit(s);
                return;
            }
            switch (voice_intent_parse(text)) {
            case INTENT_REPLY:
                /* reply goes back to the sender of this message */
                record_upload_save(s, m->from_id, m->relation, "0", m->id);
                advance = true;
                break;
            case INTENT_REPLAY:
                /* replay: do NOT advance i, do NOT re-announce header */
                break;
            case INTENT_NO_REPLY:
            default:
                advance = true;
                break;
            }
        }
    }
}

/* ---- 2.3 send a new message ------------------------------------------- */

static void handle_send_msg(session_t *s)
{
    for (int tries = 0; tries < 3; tries++) {
        dialogue_speak(tries == 0
            ? "好的，想发送给谁呢？请说出对方的名字或电话，说完请说over"
            : "没有找到这个好友，请再说一次名字或电话，说完请说over");

        char text[256];
        if (!listen_with_retry(text, sizeof(text))) {
            voice_flow_timeout_exit(s);
            return;
        }

        char rel[16], mobile[16];

        /* (a) family-role contact */
        if (voice_intent_detect_relation(text, rel, sizeof(rel))) {
            contact_t *c = find_contact_by_relation(s, rel);
            if (c) {
                record_upload_save(s, c->user_id, rel, "0", "newmsg");
                return;
            }
            continue; /* role not in contacts -> re-ask */
        }

        /* (b) stranger mobile number -> confirm first (1.4) */
        if (voice_intent_extract_mobile(text, mobile, sizeof(mobile))) {
            dialogue_confirm_info(mobile, true);
            char c2[128];
            if (!listen_with_retry(c2, sizeof(c2))) {
                voice_flow_timeout_exit(s);
                return;
            }
            if (voice_intent_parse(c2) == INTENT_YES) {
                record_upload_save(s, mobile, "moshengren", "4", "newmsg");
                return;
            }
            continue; /* wrong number -> re-ask target */
        }

        /* (c) friend nickname matching is not yet wired (needs getBabyFriendsList
         *     name matching); fall through to re-ask. */
    }
}

/* ---- 3. face-to-face add friend (two-phase timeout) -------------------- */

/* Returns true if @p baby_id is already in the child's friend list. */
static bool is_existing_friend(session_t *s, const char *baby_id)
{
    if (!baby_id || baby_id[0] == '\0') return false;
    bool found = false;
    api_result_t *res = api_get_baby_friends_list(s->session_id);
    if (res && res->code && strcmp(res->code, "0") == 0 && cJSON_IsArray(res->data)) {
        cJSON *f = NULL;
        cJSON_ArrayForEach(f, res->data) {
            const char *fid = json_str(f, "friendBabyId");
            if (fid && strcmp(fid, baby_id) == 0) {
                found = true;
                break;
            }
        }
    }
    if (res) api_result_free(res);
    return found;
}

static void handle_add_friend(session_t *s)
{
    dialogue_speak("好的，现在请您藏起来，让您的好朋友站过来让我认识一下吧！");

    const int64_t start = esp_timer_get_time();
    const int64_t soft_us = 10 * 1000000LL;  /* 10s soft reminder */
    const int64_t hard_us = 15 * 1000000LL;  /* 15s hard timeout  */
    bool soft_done = false;

    uint8_t *photo_buf = heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM);
    if (!photo_buf) {
        photo_buf = malloc(128 * 1024);
    }
    if (!photo_buf) {
        ESP_LOGE(TAG, "failed to allocate photo memory buffer");
        dialogue_speak("内存分配失败，请稍后再试");
        return;
    }

    while ((esp_timer_get_time() - start) < hard_us) {
        if (!soft_done && (esp_timer_get_time() - start) >= soft_us) {
            dialogue_speak("你的小伙伴是不是害羞了呢？如果要加好友，请让Ta站到机器前哦！");
            soft_done = true;
        }

        size_t photo_len = 0;
        camera_capture_photo_mem(photo_buf, 128 * 1024, &photo_len);
        if (photo_len == 0) {
            ESP_LOGW(TAG, "Captured photo is empty/invalid, retrying...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        api_result_t *res = api_get_baby_info_by_face_img_mem(DEVICE_ID, s->session_id, photo_buf, photo_len);
        bool recognized = false;
        char friend_baby_id[40] = "";
        char friend_name[40] = "";

        if (res && res->code && strcmp(res->code, "0") == 0 && res->data) {
            bool owned = false;
            cJSON *data = result_data_object(res, &owned);
            if (data) {
                const char *v;
                if ((v = json_str(data, "babyId"))) {
                    strlcpy(friend_baby_id, v, sizeof(friend_baby_id));
                }
                if ((v = json_str(data, "babyName"))) {
                    strlcpy(friend_name, v, sizeof(friend_name));
                }
                if (owned) cJSON_Delete(data);
            }
            recognized = (friend_baby_id[0] != '\0');
        }

        if (res) api_result_free(res);

        if (recognized) {
            if (is_existing_friend(s, friend_baby_id)) {
                dialogue_speak("嘿嘿，经我再三查证，你们早已经是好朋友啦！");
            } else {
                /* registered user -> add as friend */
                api_result_t *ar = api_add_friends_mem(DEVICE_ID, s->session_id,
                                                    friend_baby_id, NULL, 0, friend_name);
                if (ar) api_result_free(ar);
                dialogue_speak_fmt("哇%s您好，现在你们已经正式成为好朋友啦",
                                   friend_name[0] ? friend_name : "小朋友");
            }
            free(photo_buf);
            return;
        }

        /* Recognized as a stranger (no babyId). If the backend flagged a stranger
         * registration we still create the friendship but note they're unknown. */
        /* (Only treated as stranger when we actually saw a "no user" style reply;
         *  otherwise keep waiting for someone to step in front of the camera.) */

        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    free(photo_buf);
    dialogue_speak("等太久了呢，我们下次再试吧");
}

/* ---- 4. call an emergency contact ------------------------------------- */

static void save_call_log(session_t *s, contact_t *c, call_result_t r)
{
    cJSON *o = cJSON_CreateObject();
    char t[32];
    now_str(t, sizeof(t));
    cJSON_AddStringToObject(o, "babyId", s->baby_id);
    cJSON_AddStringToObject(o, "babyName", s->baby_name);
    cJSON_AddStringToObject(o, "userId", c->user_id);
    cJSON_AddStringToObject(o, "relation", relation_code_to_en(c->relation));
    cJSON_AddNumberToObject(o, "type", 10); /* 10=video call */
    cJSON_AddNumberToObject(o, "status", (r == CALL_ANSWERED_ENDED) ? 10 : 20);
    cJSON_AddStringToObject(o, "callTime", t);
    cJSON_AddNumberToObject(o, "callDuration", 0);
    cJSON_AddStringToObject(o, "deviceId", DEVICE_ID);
    cJSON_AddStringToObject(o, "sessionId", s->session_id);
    cJSON_AddStringToObject(o, "imgPath", "");

    api_result_t *res = api_save_call_log(s->session_id, o);
    cJSON_Delete(o);
    if (res) api_result_free(res);
}

static void handle_call(session_t *s, const char *heard)
{
    char rel[16] = "";
    contact_t *target = NULL;

    bool has_rel = voice_intent_detect_relation(heard, rel, sizeof(rel));
    if (has_rel) {
        target = find_contact_by_relation(s, rel);
    }

    if (!target) {
        if (s->contact_count == 0) {
            dialogue_speak("您还没有紧急联系人呢，快让家长添加吧");
            return;
        }
        contact_t *first = &s->contacts[0];
        dialogue_speak_fmt("%s好像还不是你的联系人，是否帮你打给%s？请说是或否，说完请说over",
                           has_rel ? relation_code_to_cn(rel) : "这个联系人",
                           relation_code_to_cn(first->relation));
        char t[128];
        if (!listen_with_retry(t, sizeof(t))) {
            voice_flow_timeout_exit(s);
            return;
        }
        if (voice_intent_parse(t) == INTENT_YES) {
            target = first;
        } else {
            return; /* -> farewell */
        }
    }

    while (target) {
        dialogue_speak_fmt("好嘞！正在拨打%s的电话，请稍等！", relation_code_to_cn(target->relation));

        call_result_t r = place_voip_call(target->wx_openid);
        save_call_log(s, target, r);

        if (r == CALL_ANSWERED_ENDED) {
            return; /* talk finished -> farewell */
        } else if (r == CALL_NO_ANSWER) {
            dialogue_speak_fmt("哎呀，%s好像没有接听哦！要不要再试一次？请说是或否，说完请说over",
                               relation_code_to_cn(target->relation));
            char t[128];
            if (!listen_with_retry(t, sizeof(t))) {
                voice_flow_timeout_exit(s);
                return;
            }
            if (voice_intent_parse(t) == INTENT_YES) {
                continue; /* retry same target */
            }
            return;
        } else if (r == CALL_POWEROFF) {
            dialogue_speak_fmt("糟糕！%s的电话关机了！要不要拨打其他紧急联系人？请说出要拨打给谁，说完请说over",
                               relation_code_to_cn(target->relation));
            char t[128];
            if (!listen_with_retry(t, sizeof(t))) {
                voice_flow_timeout_exit(s);
                return;
            }
            char rel2[16];
            contact_t *next = NULL;
            if (voice_intent_detect_relation(t, rel2, sizeof(rel2))) {
                next = find_contact_by_relation(s, rel2);
            }
            if (next) {
                target = next;
                continue;
            }
            return; /* give up -> farewell */
        } else {
            dialogue_speak("呼叫失败了呢，请稍后再试");
            return;
        }
    }
}

/* ---- menu -------------------------------------------------------------- */

static void handle_menu(session_t *s, bool restricted)
{
    char text[256];
    if (!listen_with_retry(text, sizeof(text))) {
        voice_flow_timeout_exit(s);
        return;
    }

    voice_intent_t it = voice_intent_parse(text);

    if (restricted) {
        /* Unrecognized child: only the public-service message feature. */
        if (it == INTENT_SEND_MSG) {
            handle_send_msg(s);
        } else {
            dialogue_speak("这个功能需要家长开通哦，您可以说发留言使用公益服务");
        }
        return;
    }

    switch (it) {
    case INTENT_SEND_MSG:  handle_send_msg(s);      break;
    case INTENT_CALL:      handle_call(s, text);    break;
    case INTENT_ADD_FRIEND:handle_add_friend(s);    break;
    default:
        dialogue_speak("我没有听懂哦，您可以说发留言、打电话或者加好友");
        break;
    }
}

/* ---- session keep-alive task ------------------------------------------ */
/*
 * keepSs performs a full HTTPS request, which must NOT run inside an esp_timer
 * callback (that task's stack is tiny and TLS overflows it). Use a dedicated
 * task with an adequate stack instead.
 */

static TaskHandle_t s_keep_task = NULL;
static volatile bool s_keep_run = false;

static void keep_alive_task(void *arg)
{
    (void)arg;
    while (s_keep_run) {
        /* sleep ~20s, but wake promptly when asked to stop */
        for (int i = 0; i < 20 && s_keep_run; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (s_keep_run && s_sess.session_id[0]) {
            session_keep_alive(&s_sess);
        }
    }
    s_keep_task = NULL;
    vTaskDelete(NULL);
}

static void keep_alive_start(void)
{
    if (s_keep_task) {
        return;
    }
    s_keep_run = true;
    if (xTaskCreate(keep_alive_task, "voice_keepss", 8192, NULL, 4, &s_keep_task) != pdPASS) {
        ESP_LOGW(TAG, "failed to create keep-alive task");
        s_keep_run = false;
    }
}

static void keep_alive_stop(void)
{
    s_keep_run = false; /* task observes the flag and self-deletes */
}

/* ---- main session task ------------------------------------------------- */

static void voice_task(void *arg)
{
    (void)arg;
    session_t *s = &s_sess;
    memset(s, 0, sizeof(*s));

    /* 1.1 greeting + countdown, then face recognition. */
    dialogue_greeting();

    /* Ensure the camera/UVC pipeline is up (idempotent). Without this the
     * capture below produces no file and searchFace2 fails with file-not-found. */
    if (!uvc_is_initialized()) {
        ESP_LOGI(TAG, "camera not initialized; running uvc_init...");
        cmd_uvc_init(0, NULL);
    }

    size_t photo_len = 0;
    uint8_t *photo_buf = heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM);
    if (!photo_buf) {
        photo_buf = malloc(128 * 1024);
    }
    if (!photo_buf) {
        ESP_LOGE(TAG, "failed to allocate photo memory buffer");
        dialogue_speak("内存分配失败，请稍后再试");
        s_session_active = false;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t cap = camera_capture_photo_mem(photo_buf, 128 * 1024, &photo_len);
    if (cap != ESP_OK || photo_len == 0) {
        ESP_LOGE(TAG, "photo capture failed or file empty; aborting session");
        free(photo_buf);
        dialogue_speak("摄像头好像出问题了呢，请稍后再试");
        s_session_active = false;
        vTaskDelete(NULL);
        return;
    }

    api_result_t *res = api_search_face_mem(DEVICE_ID, "", photo_buf, photo_len);
    bool ok = handle_face_result(res, s);
    if (res) api_result_free(res);
    free(photo_buf);

    /* Start heartbeat now that we (may) have a session. */
    if (s->session_id[0]) {
        keep_alive_start();
    }

    if (ok) {
        int n = fetch_leave_msgs(s);
        if (n <= 0) {
            /* 1.2 success, no messages -> menu */
            dialogue_welcome_menu(s->baby_name);
            handle_menu(s, false);
        } else {
            /* 2.1 / 2.2 message playback (announces its own greeting) */
            handle_messages(s);
        }
    } else {
        /* 1.2 recognition failed -> restricted (public-service message only) */
        play_local_voice(PATH_V_FACE_FAIL, TEXT_V_FACE_FAIL);
        dialogue_welcome_unknown();
        handle_menu(s, true);
    }

    /* 1.5 farewell + close session. Only say farewell if we didn't already timeout. */
    if (!s->timed_out) {
        dialogue_farewell();
    }

    keep_alive_stop();
    session_close(s);

    s_session_active = false;
    ESP_LOGI(TAG, "voice session ended");
    vTaskDelete(NULL);
}

/* ---- physical wake button (GPIO45) ------------------------------------ */

static void IRAM_ATTR wake_button_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    uint32_t gpio_num = (uint32_t)WAKE_BUTTON_GPIO;
    if (s_wake_btn_queue) {
        xQueueSendFromISR(s_wake_btn_queue, &gpio_num, &hp);
    }
    if (hp) {
        portYIELD_FROM_ISR();
    }
}

static void wake_button_task(void *arg)
{
    (void)arg;
    uint32_t gpio_num;

    ESP_LOGI(TAG, "wake button task ready (GPIO%d, active-%s)",
             WAKE_BUTTON_GPIO,
             WAKE_BUTTON_ACTIVE_LEVEL == 0 ? "low" : "high");

    for (;;) {
        if (xQueueReceive(s_wake_btn_queue, &gpio_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Debounce: wait, then confirm level still active. */
        vTaskDelay(pdMS_TO_TICKS(WAKE_BUTTON_DEBOUNCE_MS));
        if (gpio_get_level(WAKE_BUTTON_GPIO) != WAKE_BUTTON_ACTIVE_LEVEL) {
            continue;
        }

        /* Drain extra edges generated during bounce / hold. */
        uint32_t dummy;
        while (xQueueReceive(s_wake_btn_queue, &dummy, 0) == pdTRUE) {
        }

        ESP_LOGI(TAG, "wake button pressed -> voice_flow_wake()");
        voice_flow_wake();

        /* Wait for release so one press only starts one session. */
        while (gpio_get_level(WAKE_BUTTON_GPIO) == WAKE_BUTTON_ACTIVE_LEVEL) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(WAKE_BUTTON_DEBOUNCE_MS));

        while (xQueueReceive(s_wake_btn_queue, &dummy, 0) == pdTRUE) {
        }
    }
}

static esp_err_t wake_button_init(void)
{
    if (s_wake_btn_queue) {
        return ESP_OK; /* already initialized */
    }

    s_wake_btn_queue = xQueueCreate(4, sizeof(uint32_t));
    if (!s_wake_btn_queue) {
        ESP_LOGE(TAG, "wake button queue create failed");
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << WAKE_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (WAKE_BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (WAKE_BUTTON_ACTIVE_LEVEL != 0) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = (WAKE_BUTTON_ACTIVE_LEVEL == 0) ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wake button gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* May already be installed by another driver; ignore that case. */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_isr_handler_add(WAKE_BUTTON_GPIO, wake_button_isr, (void *)(uint32_t)WAKE_BUTTON_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wake button isr add failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(wake_button_task, "wake_btn", 3072, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "wake button task create failed");
        gpio_isr_handler_remove(WAKE_BUTTON_GPIO);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "wake button ready: GPIO%d (package pin 73), press = voice_wake",
             WAKE_BUTTON_GPIO);
    return ESP_OK;
}

/* ---- public API + console command ------------------------------------- */

esp_err_t voice_flow_init(void)
{
    api_service_init(BACKEND_BASE_URL);

    /* Auto-cleanup any leftover large dump files or temporary media to free SPIFFS space */
    unlink("/spiffs/dump.h264");
    unlink("/spiffs/face.jpg");
    unlink("/spiffs/reply.wav");

    esp_err_t btn_err = wake_button_init();
    if (btn_err != ESP_OK) {
        ESP_LOGW(TAG, "wake button init failed (%s); console voice_wake still available",
                 esp_err_to_name(btn_err));
        /* Do not fail whole voice subsystem if only the button failed. */
    }

    return ESP_OK;
}

void voice_flow_wake(void)
{
    if (s_session_active) {
        ESP_LOGW(TAG, "wake ignored: a session is already running");
        return;
    }
    s_session_active = true;

    /* Large stack: TTS/ASR/JSON/VoIP all run within this task. */
    if (xTaskCreate(voice_task, "voice_flow", 12288, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create voice task");
        s_session_active = false;
    }
}

int cmd_voice_wake(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (s_session_active) {
        printf("A voice session is already running.\n");
        return 1;
    }
    printf("Wake triggered (console). Starting voice interaction session...\n");
    printf("(Physical button on GPIO%d works the same way.)\n", WAKE_BUTTON_GPIO);
    voice_flow_wake();
    return 0;
}

void register_voice_wake_cmd(void)
{
    const esp_console_cmd_t cmd = {
        .command = "voice_wake",
        .help = "Start voice session (same as wake button on GPIO45)",
        .hint = NULL,
        .func = &cmd_voice_wake,
    };
    console_register_cmd(&cmd);
}
