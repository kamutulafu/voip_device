#ifndef CAMERA_CAPTURE_H
#define CAMERA_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void camera_capture_set_request(const char *filepath);
bool camera_capture_is_requested(void);
void camera_capture_save_frame(const uint8_t *buf, size_t len);

int cmd_camera_capture(int argc, char **argv);

#endif // CAMERA_CAPTURE_H
