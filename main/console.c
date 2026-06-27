#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_console.h"
#include "console.h"

static const char *TAG = "console";
static esp_console_repl_t *s_repl = NULL;
static bool s_initialized = false;

esp_err_t console_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "child_help>";
    repl_config.max_cmdline_length = 256;

    // Register help command
    esp_console_register_help_command();

    // Initialize UART console REPL
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&uart_config, &repl_config, &s_repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create REPL: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t console_register_cmd(const esp_console_cmd_t *cmd)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Console not initialized. Call console_init first.");
        return ESP_ERR_INVALID_STATE;
    }
    return esp_console_cmd_register(cmd);
}

esp_err_t console_start(void)
{
    if (!s_initialized || s_repl == NULL) {
        ESP_LOGE(TAG, "Console REPL not created.");
        return ESP_ERR_INVALID_STATE;
    }
    return esp_console_start_repl(s_repl);
}
