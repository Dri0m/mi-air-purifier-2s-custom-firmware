/* sha256_mbedtls.c - builds the vendored mbed TLS 2.28.1 SHA-256
 * (crypto/mbedtls/, Apache-2.0, unmodified) with a minimal config, so
 * module.mk needs no extra include paths or flags. */
#define MBEDTLS_CONFIG_FILE "esphome_mbedtls_config.h"
#include "mbedtls/sha256.c"

void mbedtls_platform_zeroize(void *buf, size_t len)
{
	volatile unsigned char *p = buf;
	while (len--)
		*p++ = 0;
}
