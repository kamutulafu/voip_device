#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "console.h"
#include "spiffs_storage.h"
#include "camera_capture.h"
#include "uvc_stream.h"

int cmd_zmodem_send(int argc, char **argv);
int cmd_zmodem_recv(int argc, char **argv);

static const char *TAG = "main";

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize SPIFFS
    ESP_ERROR_CHECK(init_spiffs());

    ESP_LOGI(TAG, "Initializing console...");
    ESP_ERROR_CHECK(console_init());

    // Register UVC commands
    const esp_console_cmd_t cmd_uvc_init_cfg = {
        .command = "uvc_init",
        .help = "Initialize the I2C bus, esp_video pipeline, and USB UVC stack",
        .hint = NULL,
        .func = &cmd_uvc_init,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_uvc_init_cfg));

    const esp_console_cmd_t cmd_uvc_status_cfg = {
        .command = "uvc_status",
        .help = "Get the current initialization and streaming status of the UVC camera",
        .hint = NULL,
        .func = &cmd_uvc_status,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_uvc_status_cfg));

    // Register Camera Capture command
    const esp_console_cmd_t cmd_camera_capture_cfg = {
        .command = "camera_capture",
        .help = "Capture a photo from the camera and save it to the SPIFFS filesystem",
        .hint = "[filepath]",
        .func = &cmd_camera_capture,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_camera_capture_cfg));

    // Register Zmodem Send command
    const esp_console_cmd_t cmd_zmodem_send_cfg = {
        .command = "zmodem_send",
        .help = "Send a file using Zmodem protocol",
        .hint = "<filepath>",
        .func = &cmd_zmodem_send,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_zmodem_send_cfg));

    // Register Zmodem Receive command
    const esp_console_cmd_t cmd_zmodem_recv_cfg = {
        .command = "zmodem_recv",
        .help = "Receive files using Zmodem protocol",
        .hint = "[directory]",
        .func = &cmd_zmodem_recv,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_zmodem_recv_cfg));

    // Register ls command
    const esp_console_cmd_t cmd_ls_cfg = {
        .command = "ls",
        .help = "List files in the SPIFFS filesystem",
        .hint = "[directory]",
        .func = &cmd_ls,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_ls_cfg));

    ESP_LOGI(TAG, "Starting console REPL...");
    ESP_ERROR_CHECK(console_start());
}
