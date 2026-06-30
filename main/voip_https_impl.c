/*
 * WeChat Cloud VoIP SDK - HTTPS adaptation layer for ESP-IDF.
 *
 * Ported from the SDK demo (device/v4/demo/src/freertos/esp32_tls_impl.c).
 * Instead of pinning hard-coded DigiCert root certificates, this version uses
 * the ESP-IDF certificate bundle (esp_crt_bundle), which already contains the
 * WeChat backend root CAs and is maintained/updated with the IDF.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

#include "wxvoip_network_impl.h"

static const char *TAG = "voip_https";

#define HTTPS_SEND_RECV_BUFFER_SIZE 2048
#define HTTPS_READ_WRITE_TIMEOUT    5000

#define POST_FMT \
    "POST %s HTTP/1.1\r\nAccept: */*\r\nContent-Length: %d\r\nContent-Type: application/json; " \
    "charset=utf-8\r\nHost: %s\r\nConnection: Keep-Alive\r\n\r\n%s"

static uint32_t gettime_ms(void)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static struct esp_tls *tls_connect(const char *host, int port)
{
    (void)port;
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .non_block = true,
        .timeout_ms = 3 * 1000,
    };

    /* esp_tls_conn_http_new expects an https:// prefixed URL. */
    char *url = (char *)host;
    bool allocated = false;
    if (strncmp(host, "https://", 8) != 0) {
        url = calloc(strlen(host) + 8 + 1, 1);
        if (!url) {
            return NULL;
        }
        strcat(url, "https://");
        strcat(url, host);
        allocated = true;
    }

    struct esp_tls *tls = esp_tls_conn_http_new(url, &cfg);

    if (allocated) {
        free(url);
    }

    if (tls == NULL) {
        ESP_LOGE(TAG, "TLS connection to %s failed", host);
    }
    return tls;
}

static int tls_read(struct esp_tls *tls, uint8_t *data, size_t len)
{
    int ret = esp_tls_conn_read(tls, (char *)data, len);
    if (ret == ESP_TLS_ERR_SSL_WANT_WRITE || ret == ESP_TLS_ERR_SSL_WANT_READ ||
        ret == ESP_TLS_ERR_SSL_TIMEOUT) {
        return 0;
    }
    if (ret < 0) {
        ESP_LOGE(TAG, "tls_read failed: %d", ret);
        return NETWORK_ERR_READ_FAIL;
    }
    return ret;
}

static int tls_write(struct esp_tls *tls, const uint8_t *data, size_t len)
{
    int ret = esp_tls_conn_write(tls, data, len);
    if (ret == ESP_TLS_ERR_SSL_WANT_WRITE || ret == ESP_TLS_ERR_SSL_WANT_READ ||
        ret == ESP_TLS_ERR_SSL_TIMEOUT) {
        return 0;
    }
    if (ret < 0) {
        ESP_LOGE(TAG, "tls_write failed: %d", ret);
        return NETWORK_ERR_WRITE_FAIL;
    }
    return ret;
}

static int parse_content_length(const char *http_header)
{
    const char *p = strstr(http_header, "Content-Length:");
    if (!p) {
        return 0;
    }
    p += strlen("Content-Length:");
    while (*p && (*p < '0' || *p > '9')) {
        p++;
    }
    char tmp[32] = {0};
    int i = 0;
    while (*p >= '0' && *p <= '9' && i < (int)sizeof(tmp) - 1) {
        tmp[i++] = *p++;
    }
    return atoi(tmp);
}

static int voip_https_post_with_resp(wxvoip_network_https_impl_t *stack,
                                     const char *host, int port, const char *path,
                                     const char *body, char **resp)
{
    (void)stack;

    struct esp_tls *tls = tls_connect(host, port);
    if (!tls) {
        return NETWORK_ERR_CONNECT;
    }

    uint8_t *buf = malloc(HTTPS_SEND_RECV_BUFFER_SIZE);
    if (!buf) {
        esp_tls_conn_destroy(tls);
        return -1;
    }

    int len = snprintf((char *)buf, HTTPS_SEND_RECV_BUFFER_SIZE, POST_FMT,
                       path, (int)strlen(body), host, body);
    if (len <= 0 || len >= HTTPS_SEND_RECV_BUFFER_SIZE) {
        ESP_LOGE(TAG, "request too large for buffer");
        free(buf);
        esp_tls_conn_destroy(tls);
        return -1;
    }

    /* Send the request (handles partial writes in non-blocking mode). */
    int written = 0;
    int ret;
    uint32_t start = gettime_ms();
    do {
        ret = tls_write(tls, buf + written, len - written);
        if (ret < 0) {
            goto fail;
        }
        written += ret;
        if (written >= len) {
            break;
        }
    } while (gettime_ms() - start < HTTPS_READ_WRITE_TIMEOUT);

    if (written < len) {
        ret = NETWORK_ERR_WRITE_FAIL;
        goto fail;
    }

    /* Read the HTTP response: locate the header end, parse Content-Length, then
     * read until the full body is received. */
    memset(buf, 0, HTTPS_SEND_RECV_BUFFER_SIZE);
    int cap = HTTPS_SEND_RECV_BUFFER_SIZE - 1;
    int total_read = 0;
    int content_length = 0;
    ret = -1;

    start = gettime_ms();
    do {
        int r = tls_read(tls, buf + total_read, cap - total_read);
        if (r < 0) {
            ret = r;
            break;
        }
        total_read += r;
        if (total_read >= cap) {
            ESP_LOGE(TAG, "response exceeds %d byte buffer", HTTPS_SEND_RECV_BUFFER_SIZE);
            ret = -1;
            break;
        }

        char *head_eof = strstr((const char *)buf, "\r\n\r\n");
        if (head_eof) {
            if (content_length == 0) {
                content_length = parse_content_length((char *)buf);
            }
            if (content_length != 0) {
                int head_len = head_eof - (char *)buf + 4;
                if (total_read - head_len >= content_length) {
                    char *resp_buf = malloc(content_length + 1);
                    if (!resp_buf) {
                        ret = -1;
                        break;
                    }
                    memcpy(resp_buf, head_eof + 4, content_length);
                    resp_buf[content_length] = '\0';
                    *resp = resp_buf;
                    ret = NETWORK_RET_SUCCESS;
                    break;
                }
            }
        }
    } while (gettime_ms() - start < HTTPS_READ_WRITE_TIMEOUT);

fail:
    esp_tls_conn_destroy(tls);
    free(buf);
    return ret;
}

wxvoip_network_https_impl_t voip_network_stack = {
    .post_with_resp = voip_https_post_with_resp,
    .buffer_size = 2048,
};
