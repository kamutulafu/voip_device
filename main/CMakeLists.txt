idf_component_register(SRCS "uvc_stream.c" "main.c" "console.c" "spiffs_storage.c" "camera_capture.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES espressif__esp_video espressif__usb_device_uvc espressif__tinyusb console nvs_flash spiffs esp_driver_uart zmodem)
