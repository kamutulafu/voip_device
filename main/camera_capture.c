#include "camera_capture.h"
#include "uvc_stream.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "camera_capture";

static volatile bool s_capture_requested = false;
static char s_capture_filepath[128] = {0};

static uint8_t *s_capture_mem_buf = NULL;
static size_t s_capture_mem_len = 0;
static size_t s_capture_mem_max_len = 0;
static volatile bool s_capture_to_mem = false;

void camera_capture_set_request(const char *filepath)
{
    strlcpy(s_capture_filepath, filepath, sizeof(s_capture_filepath));
    s_capture_to_mem = false;
    s_capture_requested = true;
}

bool camera_capture_is_requested(void)
{
    return s_capture_requested;
}

void camera_capture_save_frame(const uint8_t *buf, size_t len)
{
    if (buf && len > 0) {
        if (s_capture_to_mem) {
            if (len <= s_capture_mem_max_len && s_capture_mem_buf) {
                memcpy(s_capture_mem_buf, buf, len);
                s_capture_mem_len = len;
                ESP_LOGI(TAG, "Frame captured and saved to memory buffer (%d bytes)", (int)len);
            } else {
                ESP_LOGE(TAG, "Mem capture buffer too small (need %d, got %d)", (int)len, (int)s_capture_mem_max_len);
                s_capture_mem_len = 0;
            }
        } else {
            FILE *f = fopen(s_capture_filepath, "wb");
            if (f) {
                size_t written = fwrite(buf, 1, len, f);
                int flush_err = fflush(f);
                int close_err = fclose(f);
                if (written != len || flush_err != 0 || close_err != 0) {
                    ESP_LOGE(TAG, "Short/failed write to %s: wrote %d of %d bytes (flush=%d close=%d). "
                                  "File is truncated - check SPIFFS free space.",
                             s_capture_filepath, (int)written, (int)len, flush_err, close_err);
                } else {
                    ESP_LOGI(TAG, "Frame captured and saved to %s (%d bytes)", s_capture_filepath, (int)written);
                }
            } else {
                ESP_LOGE(TAG, "Failed to open %s for frame capture", s_capture_filepath);
            }
        }
    }
    s_capture_requested = false;
}

esp_err_t camera_capture_photo(const char *filepath)
{
    if (!uvc_is_initialized()) {
        ESP_LOGE(TAG, "Camera UVC is not initialized. Run uvc_init first.");
        return ESP_ERR_INVALID_STATE;
    }
    if (!filepath || filepath[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (uvc_is_streaming()) {
        // Capture from live stream
        camera_capture_set_request(filepath);

        // Wait for capture to complete (max 2 seconds)
        int timeout = 20;
        while (camera_capture_is_requested() && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout--;
        }
        if (camera_capture_is_requested()) {
            // Force clear request
            camera_capture_save_frame(NULL, 0);
            ESP_LOGE(TAG, "Timeout waiting for live frame capture");
            return ESP_ERR_TIMEOUT;
        }
    } else {
        // Temporarily start streaming to capture
        esp_err_t err = uvc_start_stream();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error starting camera stream: %s", esp_err_to_name(err));
            return err;
        }

        // Skip 4 frames, capture on 5th
        for (int i = 0; i < 4; i++) {
            void *fb = uvc_get_stream_fb();
            if (fb) {
                uvc_return_stream_fb(fb);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        camera_capture_set_request(filepath);

        void *fb = uvc_get_stream_fb();
        if (fb) {
            uvc_return_stream_fb(fb);
        }

        uvc_stop_stream();
    }

    return ESP_OK;
}

esp_err_t camera_capture_photo_mem(uint8_t *buf, size_t max_len, size_t *out_len)
{
    if (!uvc_is_initialized()) {
        ESP_LOGE(TAG, "Camera UVC is not initialized. Run uvc_init first.");
        return ESP_ERR_INVALID_STATE;
    }
    if (!buf || max_len == 0 || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    s_capture_mem_buf = buf;
    s_capture_mem_max_len = max_len;
    s_capture_mem_len = 0;
    s_capture_to_mem = true;

    if (uvc_is_streaming()) {
        s_capture_requested = true;
        // Wait for capture to complete (max 2 seconds)
        int timeout = 20;
        while (s_capture_requested && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout--;
        }
        if (s_capture_requested) {
            s_capture_requested = false;
            s_capture_to_mem = false;
            ESP_LOGE(TAG, "Timeout waiting for live frame capture to memory");
            return ESP_ERR_TIMEOUT;
        }
    } else {
        // Temporarily start streaming to capture
        esp_err_t err = uvc_start_stream();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error starting camera stream: %s", esp_err_to_name(err));
            s_capture_to_mem = false;
            return err;
        }

        // Skip 4 frames, capture on 5th
        for (int i = 0; i < 4; i++) {
            void *fb = uvc_get_stream_fb();
            if (fb) {
                uvc_return_stream_fb(fb);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        s_capture_requested = true;

        void *fb = uvc_get_stream_fb();
        if (fb) {
            uvc_return_stream_fb(fb);
        }

        uvc_stop_stream();
    }

    s_capture_to_mem = false;
    *out_len = s_capture_mem_len;
    return s_capture_mem_len > 0 ? ESP_OK : ESP_FAIL;
}

int cmd_camera_capture(int argc, char **argv)
{
    if (!uvc_is_initialized()) {
        printf("Error: Camera UVC is not initialized. Run uvc_init first.\n");
        return 1;
    }

    char filepath[128] = "/spiffs/photo.jpg";
    if (argc > 1) {
        if (argv[1][0] == '/') {
            strlcpy(filepath, argv[1], sizeof(filepath));
        } else {
            snprintf(filepath, sizeof(filepath), "/spiffs/%s", argv[1]);
        }
    }

    printf("Capturing photo to %s...\n", filepath);

    esp_err_t err = camera_capture_photo(filepath);
    if (err != ESP_OK) {
        printf("Error: capture failed (%s)\n", esp_err_to_name(err));
        return 1;
    }

    printf("Photo captured successfully!\n");
    return 0;
}
