#ifndef QDC_WEB_MBEDTLS_CONFIG_H
#define QDC_WEB_MBEDTLS_CONFIG_H

/* Only the portable digest / HMAC / password KDF is compiled. */
#define MBEDTLS_SHA256_C
#define MBEDTLS_MD_C
#define MBEDTLS_PKCS5_C
#define MBEDTLS_DEPRECATED_REMOVED

#include <mbedtls/check_config.h>
#endif
