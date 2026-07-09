#ifndef API_CRYPTO_H
#define API_CRYPTO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decrypts a base64 encoded RSA ciphertext string using the device's private key.
 * 
 * @param b64_str The null-terminated base64 string containing the RSA ciphertext.
 * @return char* The decrypted plaintext string. The caller must free() this string when done.
 *               Returns NULL on error.
 */
char *api_crypto_rsa_decrypt(const char *b64_str);

#ifdef __cplusplus
}
#endif

#endif // API_CRYPTO_H
