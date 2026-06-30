# WeChat Cloud VoIP device-side SDK (Lite) wrapper component.
#
# This component only exposes the SDK public headers and the vendor-provided
# prebuilt static library. The OS/TLS/HTTPS adaptation layer is implemented in
# the application (see main/voip_*.c).
#
# The prebuilt archive was built for riscv32-esp-elf 14.2.0 (matches ESP-IDF 5.5
# on the ESP32-P4 target). If you switch toolchain/target, replace the .a from
# the matching folder of the original SDK.

idf_component_register(INCLUDE_DIRS "include")

set(WXVOIP_LIB "${CMAKE_CURRENT_SOURCE_DIR}/lib/libwxcloudvoip_client.a")

add_prebuilt_library(wxcloudvoip_client "${WXVOIP_LIB}")

target_link_libraries(${COMPONENT_LIB} INTERFACE wxcloudvoip_client)
