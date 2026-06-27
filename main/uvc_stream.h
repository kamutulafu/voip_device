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

int cmd_uvc_init(int argc, char **argv);
int cmd_uvc_status(int argc, char **argv);

#endif // UVC_STREAM_H
