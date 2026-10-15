#ifndef QDC_AUDIO_CONFIG_H
#define QDC_AUDIO_CONFIG_H
/* SHA-1 is only for the RFC 6455 WebSocket handshake, not authentication. */
#define MBEDTLS_SHA1_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_DEPRECATED_REMOVED
#include <mbedtls/check_config.h>
#endif
