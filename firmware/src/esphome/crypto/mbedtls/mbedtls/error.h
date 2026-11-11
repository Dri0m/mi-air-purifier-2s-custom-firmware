/* error.h - shim (ours) replacing mbed TLS error.h for the vendored
 * sha256.c: only the one code it uses. */
#ifndef MBEDTLS_ERROR_H
#define MBEDTLS_ERROR_H
#define MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED -0x006E
#endif
