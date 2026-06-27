#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "console.h"

static const char *TAG = "main";

static int cmd_hello_func(int argc, char **argv)
{
    printf("Hello from ESP32 Console!\n");
    return 0;
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initializing console...");
    ESP_ERROR_CHECK(console_init());

    // Register a simple hello test command
    const esp_console_cmd_t cmd_hello = {
        .command = "hello",
        .help = "Print hello message",
        .hint = NULL,
        .func = &cmd_hello_func,
    };
    ESP_ERROR_CHECK(console_register_cmd(&cmd_hello));

    ESP_LOGI(TAG, "Starting console REPL...");
    ESP_ERROR_CHECK(console_start());
}
