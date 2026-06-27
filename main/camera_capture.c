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

void camera_capture_set_request(const char *filepath)
{
    strlcpy(s_capture_filepath, filepath, sizeof(s_capture_filepath));
    s_capture_requested = true;
}

bool camera_capture_is_requested(void)
{
    return s_capture_requested;
}

void camera_capture_save_frame(const uint8_t *buf, size_t len)
{
    if (buf && len > 0) {
        FILE *f = fopen(s_capture_filepath, "wb");
        if (f) {
            size_t written = fwrite(buf, 1, len, f);
            fclose(f);
            ESP_LOGI(TAG, "Frame captured and saved to %s (%d bytes)", s_capture_filepath, (int)written);
        } else {
            ESP_LOGE(TAG, "Failed to open %s for frame capture", s_capture_filepath);
        }
    }
    s_capture_requested = false;
}

int cmd_camera_capture(int argc, char **argv)
{
    if (!uvc_is_initialized()) {
        printf("Error: Camera UVC is not initialized. Run uvc_init first.\n");
        return 1;
    }

    char filepath[128] = "/spiffs/photo.jpg";
    if (argc > 1) {
        strlcpy(filepath, argv[1], sizeof(filepath));
    }

    printf("Capturing photo to %s...\n", filepath);
    
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
            printf("Error: Timeout waiting for live frame capture.\n");
            return 1;
        }
    } else {
        // Temporarily start streaming to capture
        esp_err_t err = uvc_start_stream();
        if (err != ESP_OK) {
            printf("Error starting camera stream: %s\n", esp_err_to_name(err));
            return 1;
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
    
    printf("Photo captured successfully!\n");
    return 0;
}
