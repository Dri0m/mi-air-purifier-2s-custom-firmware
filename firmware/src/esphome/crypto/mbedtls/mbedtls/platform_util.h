/* platform_util.h - shim (ours) replacing mbed TLS platform_util.h for the
 * vendored sha256.c.  Parameter validation is off (as in the default mbed
 * TLS config); zeroize is implemented in sha256_mbedtls.c. */
#ifndef MBEDTLS_PLATFORM_UTIL_H
#define MBEDTLS_PLATFORM_UTIL_H
#include <stddef.h>
#define MBEDTLS_INTERNAL_VALIDATE_RET(cond, ret) do { } while (0)
#define MBEDTLS_INTERNAL_VALIDATE(cond) do { } while (0)
void mbedtls_platform_zeroize(void *buf, size_t len);
#endif
