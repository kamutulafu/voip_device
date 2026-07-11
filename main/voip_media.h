#ifndef VOIP_MEDIA_H
#define VOIP_MEDIA_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Called right after a VoIP call request succeeds.
 *
 * Reads the server token that the VoIP SDK persisted to NVS, parses the media
 * proxy server IP from the payload, reports the token to the server (so the
 * cloud server joins the WeChat room), and starts pushing A/V media to it.
 *
 * @param payload The same payload passed to wx_cloudvoip_client_call
 *                (here a raw proxy server IP, e.g. "101.42.103.144").
 */
void voip_media_on_call_connected(const char *payload);

/**
 * @brief Query the current call status from the proxy server.
 *
 * @param payload The payload used to establish the call (contains proxy IP).
 * @return The server VoIP status code (e.g. 2 for Talking, 3 for Rejected, 9 for Timeout).
 *         Returns -1 if the server cannot be reached.
 */
int voip_get_call_status(const char *payload);

#ifdef __cplusplus
}
#endif

#endif // VOIP_MEDIA_H
