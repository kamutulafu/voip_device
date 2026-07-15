#ifndef APP_BUILD_H
#define APP_BUILD_H

#include "sdkconfig.h"

/*
 * App build type macros (from menuconfig: ChildHelp App build).
 *
 * Debug   → APP_BUILD_DEBUG=1   → UART msh console + test commands
 * Release → APP_BUILD_RELEASE=1 → no msh; wake button / voice / WiFi only
 */

#if defined(CONFIG_APP_BUILD_DEBUG) && CONFIG_APP_BUILD_DEBUG
#  define APP_BUILD_DEBUG     1
#  define APP_BUILD_RELEASE   0
#  define APP_BUILD_TYPE_STR  "Debug"
#elif defined(CONFIG_APP_BUILD_RELEASE) && CONFIG_APP_BUILD_RELEASE
#  define APP_BUILD_DEBUG     0
#  define APP_BUILD_RELEASE   1
#  define APP_BUILD_TYPE_STR  "Release"
#else
/* Fallback before first reconfigure: treat as Debug so tree still builds. */
#  ifndef CONFIG_APP_BUILD_DEBUG
#    warning "APP_BUILD_TYPE not set in sdkconfig; defaulting to Debug. Run idf.py reconfigure / menuconfig."
#  endif
#  define APP_BUILD_DEBUG     1
#  define APP_BUILD_RELEASE   0
#  define APP_BUILD_TYPE_STR  "Debug(default)"
#endif

#endif /* APP_BUILD_H */
