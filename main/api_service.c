#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "api_service.h"

static const char *TAG = "api_service";
static char s_base_url[128] = "https://gateway.tdskynet.com";

void api_service_init(const char *base_url) {
    if (base_url) snprintf(s_base_url, sizeof(s_base_url), "%s", base_url);
}

void api_service_set_base_url(const char *base_url) {
    if (base_url) snprintf(s_base_url, sizeof(s_base_url), "%s", base_url);
}

void api_result_free(api_result_t *res) {
    if (res) {
        if (res->root) cJSON_Delete(res->root);
        if (res->raw_body) free(res->raw_body);
        if (res->code_alloc) free(res->code_alloc);
        free(res);
    }
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool manual;   /* when true, response body is read manually; handler skips accumulation */
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        http_resp_t *r = (http_resp_t *)evt->user_data;
        if (r && !r->manual) {
            if (r->len + evt->data_len + 1 > r->cap) {
                size_t new_cap = r->cap == 0 ? 512 : r->cap * 2;
                while (r->len + evt->data_len + 1 > new_cap) new_cap *= 2;
                char *tmp = realloc(r->buf, new_cap);
                if (!tmp) return ESP_ERR_NO_MEM;
                r->buf = tmp;
                r->cap = new_cap;
            }
            memcpy(r->buf + r->len, evt->data, evt->data_len);
            r->len += evt->data_len;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

static api_result_t* api_execute_internal(
    esp_http_client_method_t method,
    const char *path,
    const char *query_string,
    const char *header_deviceid,
    const char *header_sid,
    const char *header_aqm,
    const char *json_body,
    const char *raw_body,
    const char *file_path,
    const uint8_t *file_buf,
    size_t file_buf_len,
    cJSON *multipart_fields
) {
    char url[512];
    if (query_string && strlen(query_string) > 0) {
        snprintf(url, sizeof(url), "%s%s?%s", s_base_url, path, query_string);
    } else {
        snprintf(url, sizeof(url), "%s%s", s_base_url, path);
    }

    http_resp_t resp = {0};
    /* Multipart/streaming upload reads the response body manually via
     * esp_http_client_read(); the event handler must NOT also accumulate it
     * (it fires during read and would duplicate/corrupt the body). */
    resp.manual = (file_path != NULL) || (file_buf != NULL && file_buf_len > 0);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = 15000,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;

    if (header_deviceid) esp_http_client_set_header(client, "deviceid", header_deviceid);
    if (header_sid) esp_http_client_set_header(client, "sid", header_sid);
    if (header_aqm) esp_http_client_set_header(client, "aqm-authorization", header_aqm);

    const char *boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    if (file_path || (file_buf && file_buf_len > 0)) {
        char content_type[128];
        snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
        esp_http_client_set_header(client, "Content-Type", content_type);
    } else if (json_body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, json_body, strlen(json_body));
    } else if (raw_body) {
        esp_http_client_set_header(client, "Content-Type", "text/plain");
        esp_http_client_set_post_field(client, raw_body, strlen(raw_body));
    }

    api_result_t *res = calloc(1, sizeof(api_result_t));
    if (!res) {
        esp_http_client_cleanup(client);
        return NULL;
    }

    if (file_path || (file_buf && file_buf_len > 0)) {
        int file_data_len = 0;
        if (file_path) {
            struct stat st;
            if (stat(file_path, &st) != 0) {
                ESP_LOGE(TAG, "File not found: %s", file_path);
                esp_http_client_cleanup(client);
                free(res);
                return NULL;
            }
            if (st.st_size == 0) {
                ESP_LOGE(TAG, "File is empty: %s", file_path);
                esp_http_client_cleanup(client);
                free(res);
                return NULL;
            }
            file_data_len = st.st_size;
        } else {
            file_data_len = file_buf_len;
        }
        
        int total_len = 0;
        char len_chunk[512];
        if (multipart_fields) {
            cJSON *item = multipart_fields->child;
            while (item) {
                if (cJSON_IsString(item)) {
                    total_len += snprintf(len_chunk, sizeof(len_chunk), 
                        "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", 
                        boundary, item->string, item->valuestring);
                }
                item = item->next;
            }
        }
        
        total_len += snprintf(len_chunk, sizeof(len_chunk), 
            "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\nContent-Type: application/octet-stream\r\n\r\n", 
            boundary, "upload.dat");
            
        total_len += file_data_len;
        total_len += snprintf(len_chunk, sizeof(len_chunk), "\r\n--%s--\r\n", boundary);

        esp_err_t err = esp_http_client_open(client, total_len);
        if (err == ESP_OK) {
            char chunk[512];
            if (multipart_fields) {
                cJSON *item = multipart_fields->child;
                while (item) {
                    if (cJSON_IsString(item)) {
                        int len = snprintf(chunk, sizeof(chunk), 
                            "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", 
                            boundary, item->string, item->valuestring);
                        esp_http_client_write(client, chunk, len);
                    }
                    item = item->next;
                }
            }
            
            int len = snprintf(chunk, sizeof(chunk), 
                "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\nContent-Type: application/octet-stream\r\n\r\n", 
                boundary, "upload.dat");
            esp_http_client_write(client, chunk, len);
            
            if (file_path) {
                FILE *f = fopen(file_path, "rb");
                if (f) {
                    char fbuf[1024];
                    size_t r;
                    while ((r = fread(fbuf, 1, sizeof(fbuf), f)) > 0) {
                        esp_http_client_write(client, fbuf, r);
                    }
                    fclose(f);
                }
            } else if (file_buf && file_buf_len > 0) {
                esp_http_client_write(client, (const char *)file_buf, file_buf_len);
            }
            
            len = snprintf(chunk, sizeof(chunk), "\r\n--%s--\r\n", boundary);
            esp_http_client_write(client, chunk, len);
            esp_http_client_fetch_headers(client);
            /* Reset buffer so only the response data (read below) is kept.
             * The event-handler may have written partial data during the upload
             * phase; clearing here prevents double-accumulation. */
            resp.len = 0;
            if (resp.buf) resp.buf[0] = '\0';
        }
        
        /* When using esp_http_client_open() (streaming upload), the
         * HTTP_EVENT_ON_DATA callback is NOT triggered for the response body.
         * We must accumulate the response manually via esp_http_client_read(). */
        char rbuf[512];
        int rlen = 0;
        while ((rlen = esp_http_client_read(client, rbuf, sizeof(rbuf))) > 0) {
            if (resp.len + rlen + 1 > resp.cap) {
                size_t ncap = resp.cap == 0 ? 512 : resp.cap * 2;
                while (resp.len + rlen + 1 > ncap) ncap *= 2;
                char *tmp = realloc(resp.buf, ncap);
                if (tmp) { resp.buf = tmp; resp.cap = ncap; }
            }
            if (resp.buf) {
                memcpy(resp.buf + resp.len, rbuf, rlen);
                resp.len += rlen;
                resp.buf[resp.len] = '\0';
            }
        }
    } else {
        esp_http_client_perform(client);
    }

    res->http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (resp.buf) {
        res->raw_body = resp.buf;
        res->root = cJSON_Parse(res->raw_body);
        if (res->root) {
            cJSON *code_obj = cJSON_GetObjectItemCaseSensitive(res->root, "code");
            cJSON *msg_obj = cJSON_GetObjectItemCaseSensitive(res->root, "msg");
            cJSON *data_obj = cJSON_GetObjectItemCaseSensitive(res->root, "data");
            
            if (cJSON_IsString(code_obj)) {
                res->code = code_obj->valuestring;
            } else if (cJSON_IsNumber(code_obj)) {
                res->code_alloc = malloc(32);
                if (res->code_alloc) {
                    snprintf(res->code_alloc, 32, "%d", (int)code_obj->valueint);
                    res->code = res->code_alloc;
                }
            }
            
            if (!msg_obj) {
                msg_obj = cJSON_GetObjectItemCaseSensitive(res->root, "message");
            }
            if (cJSON_IsString(msg_obj)) res->msg = msg_obj->valuestring;
            res->data = data_obj;
        }
    }
    
    return res;
}

static api_result_t* api_execute(
    esp_http_client_method_t method,
    const char *path,
    const char *query_string,
    const char *header_deviceid,
    const char *header_sid,
    const char *header_aqm,
    const char *json_body,
    const char *raw_body,
    const char *file_path,
    cJSON *multipart_fields
) {
    return api_execute_internal(method, path, query_string, header_deviceid, header_sid, header_aqm, json_body, raw_body, file_path, NULL, 0, multipart_fields);
}

// ---- Implementations ----

api_result_t* api_search_face(const char *device_id, const char *sid, const char *image_path) {
    return api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/searchFace2", NULL, device_id, sid, NULL, NULL, NULL, image_path, NULL);
}

api_result_t* api_get_device_config(const char *device_id, int vn) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "id", device_id ? device_id : "");
    cJSON_AddNumberToObject(req, "vn", vn);
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/getDeviceConfig", NULL, NULL, NULL, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_hrp(const char *payload) {
    return api_execute(HTTP_METHOD_GET, "/wechat-service/api/device/hrp", NULL, NULL, NULL, NULL, NULL, payload, NULL, NULL);
}

api_result_t* api_startup_sync(const char *payload) {
    return api_execute(HTTP_METHOD_GET, "/wechat-service/api/device/startupSync", NULL, NULL, NULL, NULL, NULL, payload, NULL, NULL);
}

api_result_t* api_post_fault_report(const char *device_id, const char *etype, const char *descs, const char *detail) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "deviceId", device_id ? device_id : "");
    cJSON_AddStringToObject(req, "etype", etype ? etype : "");
    cJSON_AddStringToObject(req, "descs", descs ? descs : "");
    cJSON_AddStringToObject(req, "detail", detail ? detail : "");
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/postFaultReport", NULL, NULL, NULL, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_post_event_log(const char *device_id, const char *etype, const char *descs, const char *username, const char *baby_id, const char *device_time) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "deviceId", device_id ? device_id : "");
    cJSON_AddStringToObject(req, "etype", etype ? etype : "");
    cJSON_AddStringToObject(req, "descs", descs ? descs : "");
    cJSON_AddStringToObject(req, "username", username ? username : "");
    if(baby_id) cJSON_AddStringToObject(req, "babyId", baby_id);
    cJSON_AddStringToObject(req, "deviceTime", device_time ? device_time : "");
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/postEventLog", NULL, NULL, NULL, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_keep_ss(const char *sid) {
    return api_execute(HTTP_METHOD_GET, "/wechat-service/api/device/keepSs", NULL, NULL, sid, NULL, NULL, NULL, NULL, NULL);
}

api_result_t* api_close_ss(const char *sid) {
    return api_execute(HTTP_METHOD_GET, "/wechat-service/api/device/closeSs", NULL, NULL, sid, NULL, NULL, NULL, NULL, NULL);
}

api_result_t* api_notify_active_service(const char *sid) {
    return api_execute(HTTP_METHOD_GET, "/wechat-service/api/device/notifyActiveService", NULL, NULL, sid, NULL, NULL, NULL, NULL, NULL);
}

api_result_t* api_notify_auth_device_group(const char *sid, const char *target_user_id, const char *target_baby_id) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "targetUserId", target_user_id ? target_user_id : "");
    if(target_baby_id) cJSON_AddStringToObject(req, "targetBabyId", target_baby_id);
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/notifyAuthDeviceGroup", NULL, NULL, sid, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_save_call_log(const char *sid, cJSON *call_log_obj) {
    char *json_body = cJSON_PrintUnformatted(call_log_obj);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/saveCallLog", NULL, NULL, sid, NULL, json_body, NULL, NULL, NULL);
    free(json_body);
    return res;
}

api_result_t* api_get_leave_msg_page(const char *sid) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "sessionId", sid ? sid : "");
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/getLeaveMsgPage", NULL, NULL, sid, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_mark_leave_msg_readed(const char *id, const char *sid, const char *read_time, const char *reader_did) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "id", id ? id : "");
    cJSON_AddStringToObject(req, "sessionId", sid ? sid : "");
    cJSON_AddStringToObject(req, "readTime", read_time ? read_time : "");
    cJSON_AddStringToObject(req, "readerDid", reader_did ? reader_did : "");
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/markLeaveMsgReaded", NULL, NULL, NULL, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_post_calls_photo(const char *session_id, const char *device_id, const char *image_path) {
    char query[256] = {0};
    snprintf(query, sizeof(query), "sessionId=%s&deviceId=%s", session_id?session_id:"", device_id?device_id:"");
    return api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/postCallsPhoto", query, NULL, NULL, NULL, NULL, NULL, image_path, NULL);
}

api_result_t* api_post_calls_audio(const char *session_id, const char *device_id, const char *audio_path) {
    char query[256] = {0};
    snprintf(query, sizeof(query), "sessionId=%s&deviceId=%s", session_id?session_id:"", device_id?device_id:"");
    return api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/postCallsAudio", query, NULL, NULL, NULL, NULL, NULL, audio_path, NULL);
}

api_result_t* api_save_leave_msg(const char *sid, cJSON *leave_msg_obj) {
    char *json_body = cJSON_PrintUnformatted(leave_msg_obj);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/saveLeaveMsg", NULL, NULL, sid, NULL, json_body, NULL, NULL, NULL);
    free(json_body);
    return res;
}

api_result_t* api_get_baby_friends_list(const char *sid) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "sessionId", sid ? sid : "");
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/getBabyFriendsList", NULL, NULL, sid, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_get_baby_info_by_face_img(const char *device_id, const char *sid, const char *image_path) {
    return api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/getBabyInfoByFaceImg", NULL, device_id, sid, NULL, NULL, NULL, image_path, NULL);
}

api_result_t* api_add_friends(const char *device_id, const char *sid, const char *baby_id, const char *face_img_path, const char *baby_name) {
    cJSON *fields = cJSON_CreateObject();
    if(baby_id) cJSON_AddStringToObject(fields, "babyId", baby_id);
    if(baby_name) cJSON_AddStringToObject(fields, "babyName", baby_name);
    
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/addFriends", NULL, device_id, sid, NULL, NULL, NULL, face_img_path, fields);
    cJSON_Delete(fields);
    return res;
}

api_result_t* api_sos_get_service_list(const char *device_id, const char *device_time, int is_idle, const char *session_id) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "deviceTime", device_time ? device_time : "");
    cJSON_AddNumberToObject(req, "isIdle", is_idle);
    cJSON_AddStringToObject(req, "sessionId", session_id ? session_id : "");
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/sos/getServiceList", NULL, device_id, NULL, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_sos_post_calls_photo(const char *session_id, const char *device_id, const char *image_path) {
    char query[256] = {0};
    snprintf(query, sizeof(query), "sessionId=%s&deviceId=%s", session_id?session_id:"", device_id?device_id:"");
    return api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/sos/postSosCallsPhoto", query, NULL, NULL, NULL, NULL, NULL, image_path, NULL);
}

api_result_t* api_sos_post_device_start_log(const char *device_id, const char *session_id, const char *device_time, const char *photos, const char *service_id) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "sessionid", session_id ? session_id : ""); // DOC says sessionid
    cJSON_AddStringToObject(req, "deviceTime", device_time ? device_time : "");
    cJSON_AddStringToObject(req, "photos", photos ? photos : "");
    cJSON_AddStringToObject(req, "serviceId", service_id ? service_id : "");
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/sos/postDeviceSosStartLog", NULL, device_id, NULL, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_sos_post_device_start_chat_log(const char *device_id, const char *session_id, const char *service_id) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "sessionid", session_id ? session_id : "");
    cJSON_AddStringToObject(req, "serviceId", service_id ? service_id : "");
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/sos/postDeviceSosStartChatLog", NULL, device_id, NULL, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_sos_post_device_end_log(const char *device_id, const char *session_id, const char *device_time, const char *service_id, const char *photos, const char *service_log, int timelong) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "sessionid", session_id ? session_id : "");
    cJSON_AddStringToObject(req, "deviceTime", device_time ? device_time : "");
    cJSON_AddStringToObject(req, "serviceId", service_id ? service_id : "");
    cJSON_AddStringToObject(req, "photos", photos ? photos : "");
    cJSON_AddStringToObject(req, "serviceLog", service_log ? service_log : "");
    cJSON_AddNumberToObject(req, "timelong", timelong);
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/sos/postDeviceSosEndLog", NULL, device_id, NULL, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_sos_post_device_back_log(const char *device_id, const char *session_id, const char *device_time, const char *service_id, const char *photos) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "sessionid", session_id ? session_id : "");
    cJSON_AddStringToObject(req, "deviceTime", device_time ? device_time : "");
    cJSON_AddStringToObject(req, "serviceId", service_id ? service_id : "");
    cJSON_AddStringToObject(req, "photos", photos ? photos : "");
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/sos/postDeviceSosBackLog", NULL, device_id, NULL, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_sos_push_app_msg(const char *device_id, const char *push_token, const char *msg) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "deviceId", device_id ? device_id : "");
    cJSON_AddStringToObject(req, "pushToken", push_token ? push_token : "");
    if(msg) cJSON_AddStringToObject(req, "msg", msg);
    char *json_body = cJSON_PrintUnformatted(req);
    api_result_t *res = api_execute(HTTP_METHOD_POST, "/wechat-service/api/device/sos/pushAppMsg", NULL, device_id, NULL, NULL, json_body, NULL, NULL, NULL);
    cJSON_Delete(req);
    free(json_body);
    return res;
}

api_result_t* api_search_face_mem(const char *device_id, const char *sid, const uint8_t *file_buf, size_t file_buf_len) {
    return api_execute_internal(HTTP_METHOD_POST, "/wechat-service/api/device/searchFace2", NULL, device_id, sid, NULL, NULL, NULL, NULL, file_buf, file_buf_len, NULL);
}

api_result_t* api_post_calls_photo_mem(const char *session_id, const char *device_id, const uint8_t *file_buf, size_t file_buf_len) {
    char query[256] = {0};
    snprintf(query, sizeof(query), "sessionId=%s&deviceId=%s", session_id?session_id:"", device_id?device_id:"");
    return api_execute_internal(HTTP_METHOD_POST, "/wechat-service/api/device/postCallsPhoto", query, NULL, NULL, NULL, NULL, NULL, NULL, file_buf, file_buf_len, NULL);
}

api_result_t* api_post_calls_audio_mem(const char *session_id, const char *device_id, const uint8_t *file_buf, size_t file_buf_len) {
    char query[256] = {0};
    snprintf(query, sizeof(query), "sessionId=%s&deviceId=%s", session_id?session_id:"", device_id?device_id:"");
    return api_execute_internal(HTTP_METHOD_POST, "/wechat-service/api/device/postCallsAudio", query, NULL, NULL, NULL, NULL, NULL, NULL, file_buf, file_buf_len, NULL);
}

api_result_t* api_get_baby_info_by_face_img_mem(const char *device_id, const char *sid, const uint8_t *file_buf, size_t file_buf_len) {
    return api_execute_internal(HTTP_METHOD_POST, "/wechat-service/api/device/getBabyInfoByFaceImg", NULL, device_id, sid, NULL, NULL, NULL, NULL, file_buf, file_buf_len, NULL);
}

api_result_t* api_add_friends_mem(const char *device_id, const char *sid, const char *baby_id, const uint8_t *file_buf, size_t file_buf_len, const char *baby_name) {
    cJSON *fields = cJSON_CreateObject();
    if(baby_id) cJSON_AddStringToObject(fields, "babyId", baby_id);
    if(baby_name) cJSON_AddStringToObject(fields, "babyName", baby_name);
    
    api_result_t *res = api_execute_internal(HTTP_METHOD_POST, "/wechat-service/api/device/addFriends", NULL, device_id, sid, NULL, NULL, NULL, NULL, file_buf, file_buf_len, fields);
    cJSON_Delete(fields);
    return res;
}
