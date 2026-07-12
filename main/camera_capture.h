#ifndef CAMERA_CAPTURE_H
#define CAMERA_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

void camera_capture_set_request(const char *filepath);
bool camera_capture_is_requested(void);
void camera_capture_save_frame(const uint8_t *buf, size_t len);

/**
 * @brief Capture a single photo from the camera and save it to @p filepath.
 *
 * Callable from application code (e.g. the voice flow). Requires the UVC/CSI
 * pipeline to be initialized (uvc_init). If the camera is not streaming it will
 * be temporarily started for the capture.
 *
 * @param filepath Absolute SPIFFS path, e.g. "/spiffs/face.jpg"
 * @return ESP_OK on success
 */
esp_err_t camera_capture_photo(const char *filepath);

/**
 * @brief Capture a single photo from the camera directly to a memory buffer.
 *
 * @param buf Output memory buffer (should be pre-allocated, e.g. 128KB in PSRAM)
 * @param max_len Size of @p buf
 * @param out_len Pointer to write the actual captured JPEG size to
 * @return ESP_OK on success
 */
esp_err_t camera_capture_photo_mem(uint8_t *buf, size_t max_len, size_t *out_len);

int cmd_camera_capture(int argc, char **argv);

#endif // CAMERA_CAPTURE_H
