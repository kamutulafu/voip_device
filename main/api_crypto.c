#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/error.h"
#include "mbedtls/base64.h"
#include "mbedtls/version.h"

#include "device_config.h"
#include "api_crypto.h"

static const char *TAG = "api_crypto";

#define RSA_BLOCK_SIZE 128     // 1024-bit 密钥 -> 每段密文 128 字节

/**
 * 剥离 PKCS#1 v1.5 type-01（签名式）padding
 * 格式: 0x00 0x01 0xFF 0xFF ... 0xFF 0x00 || M
 */
static int strip_pkcs1_type1_padding(const unsigned char *block, size_t block_len,
                                      unsigned char *out_msg, size_t *out_len)
{
    if (block_len < 3 || block[0] != 0x00 || block[1] != 0x01) {
        return -1;
    }

    size_t i = 2;
    while (i < block_len && block[i] == 0xFF) {
        i++;
    }

    // 至少要有 1 个 0xFF 填充字节，且后面紧跟分隔符 0x00
    if (i <= 2 || i >= block_len || block[i] != 0x00) {
        return -1;
    }

    i++; // 跳过分隔符 0x00
    size_t msg_len = block_len - i;
    memcpy(out_msg, &block[i], msg_len);
    *out_len = msg_len;
    return 0;
}

char *api_crypto_rsa_decrypt(const char *b64_str) {
    if (!b64_str) return NULL;

    int ret;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    char *out_json = NULL;

    unsigned char *cipher_bin = NULL;
    size_t cipher_len = 0;
    size_t b64_len = strlen(b64_str);

    cipher_bin = malloc(b64_len);
    if (!cipher_bin) {
        ESP_LOGE(TAG, "malloc failed for cipher_bin");
        goto cleanup;
    }

    // 1. base64 解码密文
    ret = mbedtls_base64_decode(cipher_bin, b64_len, &cipher_len,
                                 (const unsigned char *)b64_str, b64_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "base64 decode cipher failed: -0x%04x", (unsigned int)(-ret));
        goto cleanup;
    }

    if (cipher_len == 0 || cipher_len % RSA_BLOCK_SIZE != 0) {
        ESP_LOGE(TAG, "cipher length %u is not a multiple of block size %d",
                 (unsigned int)cipher_len, RSA_BLOCK_SIZE);
        goto cleanup;
    }

    // 2. 解析公钥 (使用 device_config.h 中的 RSA_PUBLIC_KEY，已经是带头尾的 PEM)
    ret = mbedtls_pk_parse_public_key(&pk,
                                       (const unsigned char *)RSA_PUBLIC_KEY,
                                       strlen(RSA_PUBLIC_KEY) + 1); 
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_pk_parse_public_key failed: -0x%04x", (unsigned int)(-ret));
        char err_buf[100];
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "Error details: %s", err_buf);
        goto cleanup;
    }

    if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_RSA) {
        ESP_LOGE(TAG, "parsed key is not RSA");
        goto cleanup;
    }

    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(pk);
    size_t key_len = mbedtls_pk_get_len(&pk);
    if (key_len != RSA_BLOCK_SIZE) {
        ESP_LOGE(TAG, "unexpected RSA key size: %u bytes (expected %d)",
                 (unsigned int)key_len, RSA_BLOCK_SIZE);
        goto cleanup;
    }

    // 输出缓冲区分配
    out_json = malloc(cipher_len + 1);
    if (!out_json) {
        ESP_LOGE(TAG, "malloc failed for out_json");
        goto cleanup;
    }
    size_t plain_len = 0;

    // 3. 逐段做 RSA 公钥运算 c^e mod n，再剥离 type-01 padding，拼接明文
    size_t num_blocks = cipher_len / RSA_BLOCK_SIZE;
    for (size_t i = 0; i < num_blocks; i++) {
        unsigned char block_out[RSA_BLOCK_SIZE];

        ret = mbedtls_rsa_public(rsa, cipher_bin + i * RSA_BLOCK_SIZE, block_out);
        if (ret != 0) {
            ESP_LOGE(TAG, "mbedtls_rsa_public failed at block %u: -0x%04x",
                     (unsigned int)i, (unsigned int)(-ret));
            free(out_json);
            out_json = NULL;
            goto cleanup;
        }

        unsigned char msg[RSA_BLOCK_SIZE];
        size_t msg_len = 0;
        if (strip_pkcs1_type1_padding(block_out, RSA_BLOCK_SIZE, msg, &msg_len) != 0) {
            ESP_LOGE(TAG, "invalid PKCS#1 type-01 padding at block %u", (unsigned int)i);
            free(out_json);
            out_json = NULL;
            goto cleanup;
        }

        memcpy(out_json + plain_len, msg, msg_len);
        plain_len += msg_len;
    }

    // 4. 输出
    out_json[plain_len] = '\0';
    ESP_LOGI(TAG, "decrypt_device_config success, plain len=%u", (unsigned int)plain_len);

cleanup:
    mbedtls_pk_free(&pk);
    if (cipher_bin) free(cipher_bin);
    
    return out_json;
}
