#ifndef VOIP_CLIENT_H
#define VOIP_CLIENT_H

#include "error.h"
#include "wxvoip_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OS and HTTPS adaptation layers implemented in voip_os_impl.c / voip_https_impl.c */
extern wxvoip_os_impl_t       voip_os_impl;
extern wxvoip_network_https_impl_t voip_network_stack;

/**
 * @brief Initialize the WeChat Cloud VoIP SDK, register the device if needed,
 *        and place a VoIP call to a WeChat user (by OpenID).
 *
 * Requires an active WiFi/network connection.
 *
 * @param device_id  Device serial number / ID (SN)
 * @param model_id   Device model ID
 * @param wxa_appid  Mini-program AppID used for the call
 * @param sn_ticket  snTicket obtained from the WeChat OpenAPI (used to register)
 * @param openid     OpenID of the WeChat user to call
 * @param payload    Third-party cloud SDK stream identifier (may be NULL/"")
 * @param video      true for an audio+video call, false for audio-only
 * @return WXERROR_OK on success
 */
wx_error_t voip_client_call(const char *device_id, const char *model_id,
                            const char *wxa_appid, const char *sn_ticket,
                            const char *openid, const char *payload, bool video);

/**
 * @brief Console command: place a test VoIP call to a WeChat user.
 *
 * Usage: voip_call <device_id> <model_id> <appid> <sn_ticket> <openid> [payload]
 */
int cmd_voip_call(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif // VOIP_CLIENT_H
