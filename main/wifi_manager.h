#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and join a WiFi network in STA mode.
 * 
 * @param ssid WiFi SSID
 * @param pass WiFi password
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t wifi_manager_join_sta(const char* ssid, const char* pass);

/**
 * @brief Start the WiFi auto-connection task in background.
 */
void wifi_manager_start_autoconnect(void);

/**
 * @brief WiFi join console command handler.
 */
int cmd_wifi_join(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
