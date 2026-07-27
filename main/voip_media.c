/*
 * WeChat Cloud VoIP - media bring-up after a successful call.
 *
 * After wx_cloudvoip_client_call() succeeds, the cloud media server must join
 * the WeChat VoIP room on behalf of the device. To make that happen we:
 *   1. Read the "server token" the VoIP SDK persisted to NVS.
 *   2. Parse the media proxy server IP from the payload.
 *   3. HTTP GET /report with the token so the server joins the room.
 *   4. Push the device's A/V media (H.264 video + I2S audio) to the server,
 *      which relays it into the VoIP room.
 *
 * Ported from the reference project (code_main) media_push_task, adapted to
 * this project's audio driver and shared esp_video init.
 */

#include "voip_media.h"
#include "voip_client.h"
#include "audio_driver.h"
#include "uvc_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#ifdef _IO
#undef _IO
#endif
#ifdef _IOR
#undef _IOR
#endif
#ifdef _IOW
#undef _IOW
#endif

#include "linux/videodev2.h"
#include "esp_video_device.h"

static const char *TAG = "voip_media";

#define VOIP_VIDEO_FPS          15
#define VOIP_CAM_DEV_PATH       ESP_VIDEO_MIPI_CSI_DEVICE_NAME  /* /dev/video0  */
#define VOIP_H264_DEV_PATH      ESP_VIDEO_H264_DEVICE_NAME      /* /dev/video11 */

/* NVS namespace where the SDK persists the server token. The SDK writes a file
 * whose trailing path segment is "server_token"; the OS adaptation layer
 * (voip_os_impl.c) derives the namespace by reversing that segment, yielding
 * "nekot_revres". */
#define VOIP_TOKEN_NVS_NAMESPACE "nekot_revres"
#define VOIP_TOKEN_NVS_KEY       "key"

typedef struct __attribute__((packed)) {
    uint8_t  type;    /* 0: audio, 1: video */
    uint32_t length;  /* payload length     */
} media_header_t;

/* ----- low level send helpers ------------------------------------------ */

static int send_all(int sock, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, p + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += n;
    }
    return 0;
}

static int send_media_packet(int sock, uint8_t type, const void *payload,
                             uint32_t length, bool use_http)
{
    if (use_http) {
        char chunk_hdr[32];
        int chunk_size = sizeof(media_header_t) + length;
        snprintf(chunk_hdr, sizeof(chunk_hdr), "%x\r\n", chunk_size);
        if (send_all(sock, chunk_hdr, strlen(chunk_hdr)) < 0) {
            return -1;
        }
    }

    media_header_t header;
    header.type = type;
    header.length = length;
    if (send_all(sock, &header, sizeof(header)) < 0) {
        return -1;
    }

    if (length > 0 && payload) {
        if (send_all(sock, payload, length) < 0) {
            return -1;
        }
    }

    if (use_http) {
        if (send_all(sock, "\r\n", 2) < 0) {
            return -1;
        }
    }
    return 0;
}

static esp_err_t set_codec_control(int fd, uint32_t ctrl_class, uint32_t id, int32_t value)
{
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[1];

    controls.ctrl_class = ctrl_class;
    controls.count = 1;
    controls.controls = control;
    control[0].id = id;
    control[0].value = value;

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "failed to set codec control: %d", (int)id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ----- uplink audio conditioning ---------------------------------------- */

/* Condition the captured mic PCM before it is pushed to the media server.
 *
 * The device speaker/PA, camera and Wi-Fi share the board power rails, so the
 * mic signal tends to carry a DC offset and low-frequency power-supply hum that
 * the far end perceives as a strong "current" noise. We run a first-order DC
 * blocker (high-pass, corner ~60 Hz) to remove that, followed by a hard clamp
 * so the high-pass transient can never wrap around into harsh static.
 *
 * State is kept across packets; call audio_uplink_reset() at the start of each
 * call so a new session does not inherit the previous filter state.
 */
static int32_t s_hpf_prev_x = 0;
static int32_t s_hpf_prev_y = 0;

static void audio_uplink_reset(void)
{
    s_hpf_prev_x = 0;
    s_hpf_prev_y = 0;
}

static void audio_condition_mono(int16_t *samples, size_t num_samples)
{
    /* R/256 ~= 0.977 -> high-pass corner around 60 Hz at 16 kHz. */
    const int32_t R = 250;
    for (size_t i = 0; i < num_samples; i++) {
        int32_t x = samples[i];
        /* y[n] = x[n] - x[n-1] + R * y[n-1] */
        int32_t y = x - s_hpf_prev_x + ((R * s_hpf_prev_y) >> 8);
        s_hpf_prev_x = x;
        s_hpf_prev_y = y;

        if (y > 32767) {
            y = 32767;
        } else if (y < -32768) {
            y = -32768;
        }
        samples[i] = (int16_t)y;
    }
}

/* ----- media recv task -------------------------------------------------- */

static void media_recv_task(void *pvParameter)
{
    int sock = (int)(intptr_t)pvParameter;
    ESP_LOGI(TAG, "VoIP media recv task started");

    uint8_t *recv_hdr = malloc(5);
    uint8_t *pcm_buf = malloc(2048);

    if (!recv_hdr || !pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate media recv buffers");
        free(recv_hdr);
        free(pcm_buf);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        int received = 0;
        while (received < 5) {
            int n = recv(sock, recv_hdr + received, 5 - received, 0);
            if (n <= 0) {
                ESP_LOGW(TAG, "media_recv_task: socket closed or error");
                goto exit;
            }
            received += n;
        }

        uint8_t type = recv_hdr[0];
        uint32_t length = *(uint32_t *)(recv_hdr + 1);

        if (length > 2048) {
            ESP_LOGE(TAG, "Received packet too large: %u", (unsigned)length);
            goto exit;
        }

        received = 0;
        while (received < length) {
            int n = recv(sock, pcm_buf + received, length - received, 0);
            if (n <= 0) {
                ESP_LOGW(TAG, "media_recv_task: socket closed or error reading payload");
                goto exit;
            }
            received += n;
        }

        if (type == 0) {
            size_t num_samples = length / 2;
            audio_play_pcm_write((int16_t *)pcm_buf, num_samples, 1);
        }
    }

exit:
    free(recv_hdr);
    free(pcm_buf);
    ESP_LOGI(TAG, "VoIP media recv task stopped");
    vTaskDelete(NULL);
}

/* ----- media push task -------------------------------------------------- */

static void media_push_task(void *pvParameter)
{
    char *ip_port = (char *)pvParameter;
    char *colon = strchr(ip_port, ':');
    if (!colon) {
        ESP_LOGE(TAG, "Invalid IP:PORT format");
        free(ip_port);
        voip_client_destroy();
        vTaskDelete(NULL);
        return;
    }
    *colon = '\0';
    char *ip = ip_port;
    int port = atoi(colon + 1);

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);

    int sock = -1;
    bool use_http = false;
    int cam_fd = -1;
    int m2m_fd = -1;
    void *cam_buffers[2] = {NULL, NULL};
    uint8_t *m2m_cap_buffer = NULL;
    uint32_t cam_width = 640;
    uint32_t cam_height = 480;
    uint8_t *audio_buf = NULL;

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket creation failed");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Media push connecting to %s:%d...", ip, port);
    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Media socket connect failed");
        goto cleanup;
    }
    ESP_LOGI(TAG, "Connected! Start pushing media...");

    // Create the receive task to handle downstream audio
    if (xTaskCreate(media_recv_task, "voip_media_recv", 4096, (void *)(intptr_t)sock, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create media recv task");
    }

    use_http = (port == 80 || port == 443);
    if (use_http) {
        char http_hdr[512];
        snprintf(http_hdr, sizeof(http_hdr),
                 "POST /media_push HTTP/1.1\r\n"
                 "Host: testgateway.tdskynet.com\r\n"
                 "Content-Type: application/octet-stream\r\n"
                 "Transfer-Encoding: chunked\r\n"
                 "Connection: close\r\n\r\n");
        if (send(sock, http_hdr, strlen(http_hdr), 0) < 0) {
            ESP_LOGE(TAG, "Failed to send HTTP headers");
            goto cleanup;
        }
    }

    /* ==== Camera + hardware H.264 encoder V4L2 pipeline ==== */
    if (camera_ensure_video_init() != ESP_OK) {
        ESP_LOGW(TAG, "esp_video init failed; pushing audio only");
    }

    cam_fd = open(VOIP_CAM_DEV_PATH, O_RDWR);
    m2m_fd = open(VOIP_H264_DEV_PATH, O_RDWR);

    if (cam_fd < 0 || m2m_fd < 0) {
        ESP_LOGW(TAG, "Failed to open %s or %s; video disabled (audio only)",
                 VOIP_CAM_DEV_PATH, VOIP_H264_DEV_PATH);
        if (cam_fd >= 0) { close(cam_fd); cam_fd = -1; }
        if (m2m_fd >= 0) { close(m2m_fd); m2m_fd = -1; }
    } else {
        /* 1) Camera capture format (YUV420) */
        struct v4l2_format fmt = {0};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = 800;
        fmt.fmt.pix.height = 640;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
        if (ioctl(cam_fd, VIDIOC_S_FMT, &fmt) < 0) {
            ESP_LOGW(TAG, "Camera VIDIOC_S_FMT 800x640 YUV420 failed");
        }

        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(cam_fd, VIDIOC_G_FMT, &fmt) == 0) {
            cam_width = fmt.fmt.pix.width;
            cam_height = fmt.fmt.pix.height;
            ESP_LOGI(TAG, "Camera active format: %ux%u", (unsigned)cam_width, (unsigned)cam_height);
        }

        /* 2) Request + map camera buffers */
        struct v4l2_requestbuffers req = {0};
        req.count = 2;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(cam_fd, VIDIOC_REQBUFS, &req) < 0) {
            ESP_LOGE(TAG, "Camera VIDIOC_REQBUFS failed");
            close(cam_fd); cam_fd = -1;
            close(m2m_fd); m2m_fd = -1;
        } else {
            for (int i = 0; i < 2; i++) {
                struct v4l2_buffer buf = {0};
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;
                buf.index = i;
                ioctl(cam_fd, VIDIOC_QUERYBUF, &buf);
                cam_buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, cam_fd, buf.m.offset);
                ioctl(cam_fd, VIDIOC_QBUF, &buf);
            }

            /* 3) Encoder OUTPUT format (YUV420 input) */
            struct v4l2_format format;
            memset(&format, 0, sizeof(format));
            format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            format.fmt.pix.width = cam_width;
            format.fmt.pix.height = cam_height;
            format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
            ioctl(m2m_fd, VIDIOC_S_FMT, &format);

            memset(&req, 0, sizeof(req));
            req.count = 1;
            req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            req.memory = V4L2_MEMORY_USERPTR;
            ioctl(m2m_fd, VIDIOC_REQBUFS, &req);

            /* 4) Encoder CAPTURE format (H.264 output) */
            uint32_t enc_out_size = cam_width * cam_height * 3 / 2;
            if (enc_out_size < 512 * 1024) {
                enc_out_size = 512 * 1024;
            }
            memset(&format, 0, sizeof(format));
            format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            format.fmt.pix.width = cam_width;
            format.fmt.pix.height = cam_height;
            format.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
            format.fmt.pix.sizeimage = enc_out_size;
            ioctl(m2m_fd, VIDIOC_S_FMT, &format);

            /* H.264 params (must be set AFTER the format) */
            set_codec_control(m2m_fd, V4L2_CID_CODEC_CLASS, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, 10);
            set_codec_control(m2m_fd, V4L2_CID_CODEC_CLASS, V4L2_CID_MPEG_VIDEO_BITRATE, 600000);
            set_codec_control(m2m_fd, V4L2_CID_CODEC_CLASS, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 24);
            set_codec_control(m2m_fd, V4L2_CID_CODEC_CLASS, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 40);

            memset(&req, 0, sizeof(req));
            req.count = 1;
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            req.memory = V4L2_MEMORY_MMAP;
            ioctl(m2m_fd, VIDIOC_REQBUFS, &req);

            struct v4l2_buffer m2m_buf = {0};
            m2m_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            m2m_buf.memory = V4L2_MEMORY_MMAP;
            m2m_buf.index = 0;
            ioctl(m2m_fd, VIDIOC_QUERYBUF, &m2m_buf);
            m2m_cap_buffer = (uint8_t *)mmap(NULL, m2m_buf.length, PROT_READ | PROT_WRITE,
                                             MAP_SHARED, m2m_fd, m2m_buf.m.offset);
            ioctl(m2m_fd, VIDIOC_QBUF, &m2m_buf);

            /* 5) Stream on */
            int stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(m2m_fd, VIDIOC_STREAMON, &stream_type);
            stream_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            ioctl(m2m_fd, VIDIOC_STREAMON, &stream_type);
            stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(cam_fd, VIDIOC_STREAMON, &stream_type);

            ESP_LOGI(TAG, "H.264 V4L2 pipeline started");

            for (int i = 0; i < 2; i++) {
                struct v4l2_buffer buf = {0};
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;
                ioctl(cam_fd, VIDIOC_DQBUF, &buf);
                ioctl(cam_fd, VIDIOC_QBUF, &buf);
            }
        }
    }

    size_t bytes_read = 0;
    audio_buf = malloc(1024);
    if (!audio_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        goto cleanup;
    }

    /* Fresh uplink audio filter state for this call. */
    audio_uplink_reset();

    const int64_t video_interval_us = 1000000 / VOIP_VIDEO_FPS;
    int64_t last_video_us = 0;

    bool is_answered = false;
    int64_t call_start_time_us = esp_timer_get_time();
    int64_t last_status_check_us = 0;

    while (audio_buf) {
        int64_t now_us = esp_timer_get_time();

        // 1. Check call status every 1s before answered
        if (!is_answered && (now_us - last_status_check_us >= 1000000)) {
            last_status_check_us = now_us;
            int status = voip_get_call_status(ip);
            if (status == 2) {
                is_answered = true;
                ESP_LOGI(TAG, "VoIP call answered! (Talking)");
            } else if (status > 2 && status != 5) {
                ESP_LOGI(TAG, "VoIP call ended by remote with status %d", status);
                break;
            }
        }

        // 2. Check 50 seconds auto-hangup if not answered
        if (!is_answered && (now_us - call_start_time_us >= 50000000)) {
            ESP_LOGW(TAG, "VoIP call not answered for 50 seconds. Auto hanging up...");
            voip_send_hangup(ip);
            break;
        }

        /* Audio: always pushed in real time */
        if (audio_read_raw(audio_buf, 1024, &bytes_read, portMAX_DELAY) == ESP_OK && bytes_read > 0) {
            // Pick the active microphone channel from stereo I2S frames.
            int16_t *samples = (int16_t *)audio_buf;
            size_t num_frames = bytes_read / 4;
            for (size_t i = 0; i < num_frames; i++) {
                samples[i] = mic_pick_channel(samples[2 * i], samples[2 * i + 1]);
            }

            /* Remove DC/low-frequency hum and clamp peaks before sending so the
             * far end does not hear the "current" noise. */
            audio_condition_mono(samples, num_frames);

            size_t mono_bytes = num_frames * sizeof(int16_t);

            if (send_media_packet(sock, 0, audio_buf, mono_bytes, use_http) < 0) {
                break;
            }

            /* Video: throttled to VOIP_VIDEO_FPS, but always recycle buffers */
            if (cam_fd >= 0 && m2m_fd >= 0) {
                struct v4l2_buffer cap_buf = {0};
                cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                cap_buf.memory = V4L2_MEMORY_MMAP;

                if (ioctl(cam_fd, VIDIOC_DQBUF, &cap_buf) == 0) {
                    int64_t now_us_vid = esp_timer_get_time();
                    bool push_video = (now_us_vid - last_video_us) >= video_interval_us;

                    if (push_video) {
                        last_video_us = now_us_vid;

                        struct v4l2_buffer m2m_out_buf;
                        memset(&m2m_out_buf, 0, sizeof(m2m_out_buf));
                        m2m_out_buf.index = 0;
                        m2m_out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
                        m2m_out_buf.memory = V4L2_MEMORY_USERPTR;
                        m2m_out_buf.m.userptr = (unsigned long)cam_buffers[cap_buf.index];
                        m2m_out_buf.length = cap_buf.bytesused;

                        if (ioctl(m2m_fd, VIDIOC_QBUF, &m2m_out_buf) == 0) {
                            struct v4l2_buffer m2m_cap_buf;
                            memset(&m2m_cap_buf, 0, sizeof(m2m_cap_buf));
                            m2m_cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                            m2m_cap_buf.memory = V4L2_MEMORY_MMAP;

                            if (ioctl(m2m_fd, VIDIOC_DQBUF, &m2m_cap_buf) == 0) {
#if 0
                                static FILE *dump_file = NULL;
                                static size_t dumped = 0;
                                static bool file_closed = false;
                                
                                if (!dump_file && !file_closed) {
                                    dump_file = fopen("/spiffs/dump.h264", "w");
                                    if (dump_file) {
                                        ESP_LOGI(TAG, "Opened /spiffs/dump.h264 for writing");
                                    } else {
                                        file_closed = true;
                                    }
                                }
                                
                                if (dump_file) {
                                    if (dumped < 500 * 1024) {
                                        fwrite(m2m_cap_buffer, 1, m2m_cap_buf.bytesused, dump_file);
                                        dumped += m2m_cap_buf.bytesused;
                                    } else {
                                        fclose(dump_file);
                                        dump_file = NULL;
                                        file_closed = true;
                                        ESP_LOGI(TAG, "Finished dumping 500KB of H.264 to SPIFFS");
                                    }
                                }
#endif

                                int ret = send_media_packet(sock, 1, m2m_cap_buffer,
                                                            m2m_cap_buf.bytesused, use_http);
                                ioctl(m2m_fd, VIDIOC_QBUF, &m2m_cap_buf);
                                if (ret < 0) {
                                    ioctl(cam_fd, VIDIOC_QBUF, &cap_buf);
                                    ioctl(m2m_fd, VIDIOC_DQBUF, &m2m_out_buf);
                                    break;
                                }
                            }
                            ioctl(m2m_fd, VIDIOC_DQBUF, &m2m_out_buf);
                        }
                    }

                    ioctl(cam_fd, VIDIOC_QBUF, &cap_buf);
                }
            }
        }
    }

cleanup:
    ESP_LOGW(TAG, "Media push stopped");
    
    /* Give the cloud proxy server some time to process the C program exit and parse the final status */
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    /* Fetch and print the final call status */
    int final_status = voip_get_call_status(ip);
    if (final_status == 2) {
        ESP_LOGI(TAG, "====== VoIP Call Result: [2] 接通 (TALKING) ======");
    } else if (final_status == 3) {
        ESP_LOGI(TAG, "====== VoIP Call Result: [3] 拒接未接听 (REJECTED) ======");
    } else if (final_status == 9) {
        ESP_LOGI(TAG, "====== VoIP Call Result: [9] 超时未接听 (TIMEOUT) ======");
    } else if (final_status == 8) {
        ESP_LOGI(TAG, "====== VoIP Call Result: [8] 占线未接听 (BUSY) ======");
    } else if (final_status == 7) {
        ESP_LOGI(TAG, "====== VoIP Call Result: [7] 异常/关机 (ABORTED) ======");
    } else if (final_status == 6) {
        ESP_LOGI(TAG, "====== VoIP Call Result: [6] 小程序端挂断 (HANGUP_BY_CALLEE) ======");
    } else if (final_status == 5) {
        ESP_LOGI(TAG, "====== VoIP Call Result: [5] 设备端挂断 (HANGUP_BY_CALLER) ======");
    } else {
        ESP_LOGI(TAG, "====== VoIP Call Result: [%d] ======", final_status);
    }

    if (sock >= 0) {
        if (use_http) {
            send(sock, "0\r\n\r\n", 5, 0);
        }
        close(sock);
    }
    if (cam_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(cam_fd, VIDIOC_STREAMOFF, &type);
        close(cam_fd);
    }
    if (m2m_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(m2m_fd, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(m2m_fd, VIDIOC_STREAMOFF, &type);
        close(m2m_fd);
    }
    if (audio_buf) {
        free(audio_buf);
    }
    free(ip_port);

    // Clean up WeChat VoIP SDK resources
    voip_client_destroy();

    vTaskDelete(NULL);
}

/* ----- token report + media start -------------------------------------- */

static bool read_server_token(char *out, size_t out_size)
{
    nvs_handle_t handle;
    if (nvs_open(VOIP_TOKEN_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t required = out_size - 1;
    esp_err_t err = nvs_get_blob(handle, VOIP_TOKEN_NVS_KEY, out, &required);
    nvs_close(handle);
    if (err == ESP_OK) {
        out[required] = '\0';
        return strlen(out) > 0;
    }
    return false;
}

/* Extract the proxy server IP from the payload. Supports a JSON payload with
 * "proxy_server_ip"/"media_ip", or a raw "ip" / "ip:port" string. */
static void parse_server_ip(const char *payload, char *server_ip, size_t ip_size)
{
    server_ip[0] = '\0';

    const char *ip_ptr = strstr(payload, "\"proxy_server_ip\"");
    if (!ip_ptr) {
        ip_ptr = strstr(payload, "\"media_ip\"");
    }
    if (ip_ptr) {
        const char *colon = strchr(ip_ptr, ':');
        if (colon) {
            const char *q1 = strchr(colon, '\"');
            if (q1) {
                const char *q2 = strchr(q1 + 1, '\"');
                if (q2) {
                    size_t len = q2 - (q1 + 1);
                    if (len > 0 && len < ip_size) {
                        memcpy(server_ip, q1 + 1, len);
                        server_ip[len] = '\0';
                    }
                }
            }
        }
    }

    /* Fallback: payload is a raw IP (optionally with :port). */
    if (server_ip[0] == '\0' && strchr(payload, '.')) {
        strlcpy(server_ip, payload, ip_size);
        char *c = strchr(server_ip, ':');
        if (c) {
            *c = '\0';
        }
    }
}

static void report_token(const char *server_ip, const char *token, const char *payload)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(server_ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(9001); /* connect to auto_voip_server */

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "report_token: socket failed");
        return;
    }

    ESP_LOGI(TAG, "Reporting token to %s:9001 ...", server_ip);
    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) == 0) {
        char *req = malloc(1024);
        if (req) {
            snprintf(req, 1024,
                     "GET /report HTTP/1.1\r\n"
                     "Host: testgateway.tdskynet.com\r\n"
                     "X-VoIP-Token: %s\r\n"
                     "X-VoIP-Payload: %s\r\n"
                     "Connection: close\r\n\r\n",
                     token, payload);
            send_all(sock, req, strlen(req));
            free(req);

            char resp[128];
            while (recv(sock, resp, sizeof(resp) - 1, 0) > 0) {
                /* drain until server closes */
            }
            ESP_LOGI(TAG, "Token reported successfully");
        }
    } else {
        ESP_LOGE(TAG, "report_token: connect to %s:9001 failed", server_ip);
    }
    close(sock);
}

void voip_media_on_call_connected(const char *payload)
{
    /* Ensure codec is up, PA enabled, and speaker at a safe loud level for the call. */
    if (audio_init() == ESP_OK) {
        audio_set_volume(audio_get_volume());
    }

    if (!payload) {
        payload = "";
    }

    char token[256] = {0};
    if (!read_server_token(token, sizeof(token))) {
        ESP_LOGE(TAG, "Failed to read server token from NVS; cannot join call");
        return;
    }
    ESP_LOGI(TAG, "Server token: %s", token);

    char server_ip[64] = {0};
    parse_server_ip(payload, server_ip, sizeof(server_ip));
    if (server_ip[0] == '\0') {
        ESP_LOGE(TAG, "No valid server IP in payload '%s'; cannot join call", payload);
        return;
    }
    ESP_LOGI(TAG, "Media proxy server IP: %s", server_ip);

    /* 1) Tell the cloud server to join the WeChat room using our token. */
    report_token(server_ip, token, payload);

    /* 2) Start pushing media to the server (port 8081 -> media_recv_server). */
    char *media_addr = malloc(128);
    if (media_addr) {
        snprintf(media_addr, 128, "%s:8081", server_ip);
        ESP_LOGI(TAG, "Scheduling media push to %s in 3s...", media_addr);
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (xTaskCreate(media_push_task, "voip_media_push", 8192, media_addr, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create media push task");
            free(media_addr);
        }
    }
}

int voip_get_call_status(const char *payload_or_ip)
{
    if (!payload_or_ip) return -1;

    char server_ip[64] = {0};
    
    if (strchr(payload_or_ip, '.') && !strchr(payload_or_ip, '{')) {
        // It's already a raw IP
        strlcpy(server_ip, payload_or_ip, sizeof(server_ip));
    } else {
        // It's a JSON payload
        parse_server_ip(payload_or_ip, server_ip, sizeof(server_ip));
    }
    
    if (server_ip[0] == '\0') {
        return -1;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(server_ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(9001);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return -1;
    }

    int status = -1;
    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) == 0) {
        char req[128];
        snprintf(req, sizeof(req),
                 "GET /status HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Connection: close\r\n\r\n",
                 server_ip);
        
        send_all(sock, req, strlen(req));

        char resp[512];
        int n = recv(sock, resp, sizeof(resp) - 1, 0);
        if (n > 0) {
            resp[n] = '\0';
            char *body = strstr(resp, "\r\n\r\n");
            if (body) {
                body += 4;
                status = atoi(body);
            }
        }
    }
    close(sock);
    return status;
}

void voip_send_hangup(const char *server_ip)
{
    if (!server_ip || server_ip[0] == '\0') return;

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(server_ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(9001);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "voip_send_hangup: socket failed");
        return;
    }

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) == 0) {
        char req[128];
        snprintf(req, sizeof(req),
                 "GET /hangup HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Connection: close\r\n\r\n",
                 server_ip);
        
        send_all(sock, req, strlen(req));
        
        char resp[128];
        recv(sock, resp, sizeof(resp) - 1, 0); // read response to ensure it's sent
        ESP_LOGI(TAG, "Sent hangup request to server");
    } else {
        ESP_LOGE(TAG, "voip_send_hangup: connect to %s:9001 failed", server_ip);
    }
    close(sock);
}
