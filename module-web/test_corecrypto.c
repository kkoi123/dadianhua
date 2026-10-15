#include <errno.h>
#include <limits.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_hex(const unsigned char *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        printf("%02x", (unsigned int)data[i]);
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    unsigned char output[64];
    memset(output, 0, sizeof(output));
    if (argc == 3 && strcmp(argv[1], "sha256") == 0) {
        if (mbedtls_sha256_ret((const unsigned char *)argv[2], strlen(argv[2]), output, 0) != 0) {
            return 1;
        }
        print_hex(output, 32);
    } else if (argc == 6 && strcmp(argv[1], "pbkdf2") == 0) {
        char *end = NULL;
        errno = 0;
        unsigned long rounds = strtoul(argv[4], &end, 10);
        if (errno != 0 || *end != '\0' || rounds == 0 || rounds > UINT_MAX) {
            return 2;
        }
        errno = 0;
        unsigned long length = strtoul(argv[5], &end, 10);
        if (errno != 0 || *end != '\0' || length == 0 || length > sizeof(output)) {
            return 2;
        }
        mbedtls_md_context_t context;
        mbedtls_md_init(&context);
        const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        int result = info == NULL ? -1 : mbedtls_md_setup(&context, info, 1);
        if (result == 0) {
            result = mbedtls_pkcs5_pbkdf2_hmac(&context,
                (const unsigned char *)argv[2], strlen(argv[2]),
                (const unsigned char *)argv[3], strlen(argv[3]),
                (unsigned int)rounds, (uint32_t)length, output);
        }
        mbedtls_md_free(&context);
        if (result != 0) {
            mbedtls_platform_zeroize(output, sizeof(output));
            return 1;
        }
        print_hex(output, (size_t)length);
    } else {
        fputs("usage: test-corecrypto sha256 text | pbkdf2 password salt rounds length\n", stderr);
        return 2;
    }
    mbedtls_platform_zeroize(output, sizeof(output));
    return 0;
}
