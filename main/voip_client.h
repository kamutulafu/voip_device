#ifndef VOIP_CLIENT_H
#define VOIP_CLIENT_H

#include <stdbool.h>
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
 * @brief Fetch a fresh snTicket from the backend provisioning service.
 *
 * Performs an HTTPS GET to:
 *   {VOIP_BACKEND_HOST}/wechat-service/api/device/getWecooperSnTicket?deviceId=..&sid=..
 * and extracts the snTicket from the JSON response.
 *
 * @param device_id  Device serial number / ID
 * @param sid        Service/model id (passed as the "sid" query parameter)
 * @param out        [out] Buffer that receives the NUL-terminated snTicket
 * @param out_size   Size of out in bytes
 * @return WXERROR_OK on success
 */
wx_error_t voip_fetch_sn_ticket(const char *device_id, const char *sid,
                                char *out, size_t out_size);

/**
 * @brief Console command: place a test VoIP call to a WeChat user.
 *
 * Most parameters default to the values baked into the firmware; the snTicket
 * is fetched automatically from the backend. All defaults can be overridden.
 *
 * Usage: voip_call [openid] [device_id] [model_id] [appid] [payload]
 */
int cmd_voip_call(int argc, char **argv);

/**
 * @brief Destroy the WeChat Cloud VoIP SDK and reset initialization state.
 */
void voip_client_destroy(void);

#ifdef __cplusplus
}
#endif

#endif // VOIP_CLIENT_H
