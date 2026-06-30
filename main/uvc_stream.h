#ifndef UVC_STREAM_H
#define UVC_STREAM_H

#include "esp_err.h"
#include <stdbool.h>

// UVC module API
bool uvc_is_initialized(void);
bool uvc_is_streaming(void);
esp_err_t uvc_start_stream(void);
void uvc_stop_stream(void);
void *uvc_get_stream_fb(void);
void uvc_return_stream_fb(void *fb);

#include "driver/i2c_master.h"
i2c_master_bus_handle_t uvc_get_i2c_bus_handle(void);
void uvc_set_i2c_bus_handle(i2c_master_bus_handle_t handle);

/**
 * @brief Ensure the I2C bus and the esp_video pipeline (CSI/ISP) are initialized.
 *        Idempotent: safe to call multiple times and from multiple features
 *        (UVC streaming, VoIP media push). Does not open any V4L2 device.
 */
esp_err_t camera_ensure_video_init(void);

int cmd_uvc_init(int argc, char **argv);
int cmd_uvc_status(int argc, char **argv);

esp_err_t uvc_set_test_pattern(bool enable);
bool uvc_is_test_pattern_enabled(void);
int cmd_camera_test_pattern(int argc, char **argv);

#endif // UVC_STREAM_H
