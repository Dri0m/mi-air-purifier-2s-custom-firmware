/* esphome_mbedtls_config.h - minimal mbed TLS configuration for the
 * vendored SHA-256 only (ours, not part of mbed TLS). */
#ifndef ESPHOME_MBEDTLS_CONFIG_H
#define ESPHOME_MBEDTLS_CONFIG_H
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER          /* smaller code, ~same speed on M4 */
#define MBEDTLS_DEPRECATED_REMOVED
#endif
