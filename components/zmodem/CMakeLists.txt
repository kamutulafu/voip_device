idf_component_register(SRCS "zcore.c" "zdevice.c" "zstart.c" "sz.c" "rz.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_uart console esp_driver_usb_serial_jtag)

