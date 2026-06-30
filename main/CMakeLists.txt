idf_component_register(SRCS "uvc_stream.c" "main.c" "console.c" "spiffs_storage.c" "camera_capture.c" "audio_driver.c" "wifi_manager.c" "asr_xfyun.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES espressif__esp_video espressif__usb_device_uvc espressif__tinyusb console nvs_flash spiffs esp_driver_uart zmodem esp_driver_i2s esp_driver_gpio esp_driver_i2c esp_wifi esp_netif esp_event esp_websocket_client esp-tls mbedtls json)
