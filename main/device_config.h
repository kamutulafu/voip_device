#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

/* =========================================================================
 * 1. Global Device Configuration
 * ========================================================================= */

#define DEVICE_ID           "RD2600000001"
#define BACKEND_BASE_URL    "https://gateway.tdskynet.com"

/* =========================================================================
 * 2. WeChat Cloud VoIP Configuration
 * ========================================================================= */

#define VOIP_MODEL_ID       "YiROQwsClOLubDM-ej_isQ"
#define VOIP_APPID          "wx769bf6a5775ba85e"
#define VOIP_OPENID         "o1s5V7MPSGlDymTQKCqKgcYCPd6Q"
#define VOIP_PAYLOAD        "180.76.139.70"

/* =========================================================================
 * 3. RSA Key Pairs (PEM Format)
 * ========================================================================= */

#define RSA_PUBLIC_KEY \
    "-----BEGIN PUBLIC KEY-----\n" \
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCusfrly8DgJ91piX7pitfbYA/m\n" \
    "flG5l1aR+7bHUE39/VrQfhk2FxEu/W+KkjUN2CV2kVnlKQ/loLztBqA993Vv3JYC\n" \
    "gC5QJ7ebQx5cUiqxCsTlwSGK8b1hm6pIkxWwu2huPDUjsSIXwtyRQ3BIU+hEWjEl\n" \
    "VQYH3hApZK3v3sh6YQIDAQAB\n" \
    "-----END PUBLIC KEY-----\n"

#define RSA_PRIVATE_KEY \
    "-----BEGIN RSA PRIVATE KEY-----\n" \
    "MIICdwIBADANBgkqhkiG9w0BAQEFAASCAmEwggJdAgEAAoGBAK6x+uXLwOAn3WmJ\n" \
    "fumK19tgD+Z+UbmXVpH7tsdQTf39WtB+GTYXES79b4qSNQ3YJXaRWeUpD+WgvO0G\n" \
    "oD33dW/clgKALlAnt5tDHlxSKrEKxOXBIYrxvWGbqkiTFbC7aG48NSOxIhfC3JFD\n" \
    "cEhT6ERaMSVVBgfeEClkre/eyHphAgMBAAECgYEAgLYFY5YRz5XPnlh9t1hi3fET\n" \
    "BgH/+Lu2Puy0qHlUXVRzurWNobqxIGv96Jz8leyw/YDuONdeLROW3xRIsB9I2B1V\n" \
    "/10yyfG/Yl46/p2Io9P2ks5eZzQbj6LzmfHF3uyQzW0Ml9WQLgUl29kHJPWnrjUY\n" \
    "YOdvbEV4qtI4jZ6JPoECQQD0LpTyaopb+/Cfy0EBJniyGk1HvbyhN9Hz1pmTeDrR\n" \
    "PfSksD7odQ4rdjwpQTNN/SuFld6oeU9irbXbVdQzsz8JAkEAtyZ05//mIjVESNk0\n" \
    "W+5xpPeKgR++kM5+SMq4CTZTz8PCT7ZSjOS1PoWeOB92l5ilHwITrs52vFC7ONMZ\n" \
    "SY7emQJAKIHxw6VY/pl0+Y1GY2J2c1VZrKUVPcl80u6u23/+gee9RfTW+skwaJVc\n" \
    "tZtTX4S4S5jpLxmwybX3jUNXyJvbwQJBAKX9EI8C+YufQxfS4wU+gXjFcJ2+K3QJ\n" \
    "8aH/N/QBbMwr2vtrfj17OlhDuTWcLlsOWPhVZYlUTYA2mrfRemWUOmECQDGJtL9V\n" \
    "xpsPG330tNZglikRR02OIXtjS3NYR5nz3OKGu/7gcWsyFhLbGv/r+mbGDezGK6qF\n" \
    "RgUpN+b+wbgJYx4=\n" \
    "-----END RSA PRIVATE KEY-----\n"

#endif // DEVICE_CONFIG_H
