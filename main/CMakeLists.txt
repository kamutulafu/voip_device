idf_component_register(SRCS "main.c" "console.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES espressif__esp_video espressif__usb_device_uvc espressif__tinyusb console nvs_flash)


