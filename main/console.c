#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "esp_log.h"
#include "esp_console.h"
#include "console.h"

static const char *TAG = "console";
static esp_console_repl_t *s_repl = NULL;
static bool s_initialized = false;

bool console_is_enabled(void)
{
    return s_initialized && (s_repl != NULL);
}

#include <sys/time.h>

int cmd_date(int argc, char **argv)
{
    if (argc >= 2) {
        time_t new_sec = 0;
        if (argc == 2 && strchr(argv[1], '-') == NULL && strchr(argv[1], ':') == NULL) {
            // Raw timestamp (e.g. date 1785000000)
            new_sec = (time_t)atoll(argv[1]);
        } else {
            // String format (e.g. date "2026-07-27 13:23:00" or date 2026-07-27 13:23:00)
            int year = 0, mon = 0, mday = 0, hour = 0, min = 0, sec = 0;
            char combined[64] = {0};
            if (argc >= 3) {
                snprintf(combined, sizeof(combined), "%s %s", argv[1], argv[2]);
            } else {
                snprintf(combined, sizeof(combined), "%s", argv[1]);
            }

            if (sscanf(combined, "%d-%d-%d %d:%d:%d", &year, &mon, &mday, &hour, &min, &sec) == 6) {
                struct tm tm_set = {
                    .tm_year = year - 1900,
                    .tm_mon  = mon - 1,
                    .tm_mday = mday,
                    .tm_hour = hour,
                    .tm_min  = min,
                    .tm_sec  = sec,
                };
                setenv("TZ", "UTC0", 1);
                tzset();
                new_sec = mktime(&tm_set);
            }
        }

        if (new_sec > 0) {
            struct timeval tv = { .tv_sec = new_sec, .tv_usec = 0 };
            if (settimeofday(&tv, NULL) == 0) {
                printf("RTC system time updated successfully!\n");
            } else {
                printf("Failed to set RTC time via settimeofday()\n");
                return 1;
            }
        } else {
            printf("Usage: date [epoch_seconds | \"YYYY-MM-DD HH:MM:SS\"]\n");
            return 1;
        }
    }

    time_t now = time(NULL);
    struct tm tm_now;
    gmtime_r(&now, &tm_now);

    char buf[64];
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_now);

    printf("Current system RTC time (UTC): %s\n", buf);
    printf("Raw epoch seconds: %lld\n", (long long)now);
    return 0;
}

esp_err_t console_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "child_help>";
    repl_config.max_cmdline_length = 256;
    repl_config.task_stack_size = 8192;

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
