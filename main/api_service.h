#ifndef API_SERVICE_H
#define API_SERVICE_H

#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the API service.
 * @param base_url The base URL (e.g., "https://gateway.tdskynet.com")
 */
void api_service_init(const char *base_url);

/**
 * @brief Set the base URL dynamically (e.g., after getting config).
 */
void api_service_set_base_url(const char *base_url);

/**
 * @brief Represents an API response
 */
typedef struct {
    int http_status;
    cJSON *root;        // Parsed JSON root (if response is JSON)
    char *raw_body;     // Raw response body (if not JSON or needed for debugging)
    
    // Extracted from common JSON format {"code": "0", "msg": "...", "data": ...}
    const char *code;
    const char *msg;
    cJSON *data;        
} api_result_t;

/**
 * @brief Free an API result
 */
void api_result_free(api_result_t *res);


// ============================================================================
// 1. Device Core Interfaces
// ============================================================================

api_result_t* api_search_face(const char *device_id, const char *sid, const char *image_path);
api_result_t* api_get_device_config(const char *device_id, int vn);
api_result_t* api_hrp(const char *payload);
api_result_t* api_startup_sync(const char *payload);
api_result_t* api_post_fault_report(const char *device_id, const char *etype, const char *descs, const char *detail);
api_result_t* api_post_event_log(const char *device_id, const char *etype, const char *descs, const char *username, const char *baby_id, const char *device_time);
api_result_t* api_keep_ss(const char *sid);
api_result_t* api_close_ss(const char *sid);
api_result_t* api_notify_active_service(const char *sid);
api_result_t* api_notify_auth_device_group(const char *sid, const char *target_user_id, const char *target_baby_id);

// ============================================================================
// 2. Business & Media Interfaces
// ============================================================================

api_result_t* api_save_call_log(const char *sid, cJSON *call_log_obj);
api_result_t* api_get_leave_msg_page(const char *sid);
api_result_t* api_mark_leave_msg_readed(const char *id, const char *sid, const char *read_time, const char *reader_did);
api_result_t* api_post_calls_photo(const char *session_id, const char *device_id, const char *image_path);
api_result_t* api_post_calls_audio(const char *session_id, const char *device_id, const char *audio_path);
api_result_t* api_save_leave_msg(const char *sid, cJSON *leave_msg_obj);
api_result_t* api_get_baby_friends_list(const char *sid);
api_result_t* api_get_baby_info_by_face_img(const char *device_id, const char *sid, const char *image_path);
api_result_t* api_add_friends(const char *device_id, const char *sid, const char *baby_id, const char *face_img_path, const char *baby_name);

// ============================================================================
// 3. SOS (Emergency Call) Interfaces
// ============================================================================

api_result_t* api_sos_get_service_list(const char *device_id, const char *device_time, int is_idle, const char *session_id);
api_result_t* api_sos_post_calls_photo(const char *session_id, const char *device_id, const char *image_path);
api_result_t* api_sos_post_device_start_log(const char *device_id, const char *session_id, const char *device_time, const char *photos, const char *service_id);
api_result_t* api_sos_post_device_start_chat_log(const char *device_id, const char *session_id, const char *service_id);
api_result_t* api_sos_post_device_end_log(const char *device_id, const char *session_id, const char *device_time, const char *service_id, const char *photos, const char *service_log, int timelong);
api_result_t* api_sos_post_device_back_log(const char *device_id, const char *session_id, const char *device_time, const char *service_id, const char *photos);
api_result_t* api_sos_push_app_msg(const char *device_id, const char *push_token, const char *msg);

// Memory-based uploads (bypassing filesystem)
api_result_t* api_search_face_mem(const char *device_id, const char *sid, const uint8_t *file_buf, size_t file_buf_len);
api_result_t* api_post_calls_photo_mem(const char *session_id, const char *device_id, const uint8_t *file_buf, size_t file_buf_len);
api_result_t* api_post_calls_audio_mem(const char *session_id, const char *device_id, const uint8_t *file_buf, size_t file_buf_len);
api_result_t* api_get_baby_info_by_face_img_mem(const char *device_id, const char *sid, const uint8_t *file_buf, size_t file_buf_len);
api_result_t* api_add_friends_mem(const char *device_id, const char *sid, const char *baby_id, const uint8_t *file_buf, size_t file_buf_len, const char *baby_name);

#ifdef __cplusplus
}
#endif

#endif // API_SERVICE_H
