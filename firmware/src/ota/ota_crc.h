/* ota_crc.h - checksums used by OTA (internal, portable). */
#ifndef OTA_CRC_H
#define OTA_CRC_H
#include <stddef.h>
#include <stdint.h>

/* CRC-32 reflected 0xEDB88320, init 0, NO final xor: the boot2 HW engine
 * (CRC32_IEEE), WMPT table CRCs and MRVL segment CRCs.  Chainable:
 * crc = ota_crc32(crc, part1); crc = ota_crc32(crc, part2); start with 0. */
uint32_t ota_crc32(uint32_t crc, const void *data, size_t len);

/* MD5 (RFC 1321), streaming. */
typedef struct {
    uint32_t state[4];
    uint32_t count_lo, count_hi;          /* bytes */
    uint8_t buf[64];
} ota_md5_ctx;

void ota_md5_init(ota_md5_ctx *c);
void ota_md5_update(ota_md5_ctx *c, const void *data, size_t len);
void ota_md5_final(ota_md5_ctx *c, uint8_t out[16]);
#endif
