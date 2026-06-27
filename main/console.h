#ifndef MAIN_CONSOLE_H
#define MAIN_CONSOLE_H

#include "esp_err.h"
#include "esp_console.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the console REPL (Read-Eval-Print Loop)
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t console_init(void);

/**
 * @brief Register a command with the console
 * 
 * @param cmd Pointer to esp_console_cmd_t structure defining the command
 * @return esp_err_t ESP_OK on success
 */
esp_err_t console_register_cmd(const esp_console_cmd_t *cmd);

/**
 * @brief Start the console REPL
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t console_start(void);

#ifdef __cplusplus
}
#endif

#endif // MAIN_CONSOLE_H
