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

int cmd_uvc_init(int argc, char **argv);
int cmd_uvc_status(int argc, char **argv);

esp_err_t uvc_set_test_pattern(bool enable);
bool uvc_is_test_pattern_enabled(void);
int cmd_camera_test_pattern(int argc, char **argv);

#endif // UVC_STREAM_H
