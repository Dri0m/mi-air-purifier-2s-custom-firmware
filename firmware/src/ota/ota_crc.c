/* ota_crc.c - CRC-32 (init 0, no xorout) and MD5, portable C. */
#include "ota_crc.h"
#include <string.h>

/* 16-entry nibble table: 64 bytes of flash, ~2x slower than a byte table but
 * still far faster than the flash reads it follows. */
static const uint32_t crc_nib[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t ota_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc_nib[crc & 0x0F];
        crc = (crc >> 4) ^ crc_nib[crc & 0x0F];
    }
    return crc;
}

/* ---------------------------------------------------------------- MD5 */

#define F1(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) ((x) ^ (y) ^ (z))
#define F4(x, y, z) ((y) ^ ((x) | ~(z)))
#define STEP(f, a, b, c, d, x, s, t) \
    ((a) += f((b), (c), (d)) + (x) + (t), \
     (a) = ((a) << (s)) | ((a) >> (32 - (s))), (a) += (b))

static void md5_block(uint32_t st[4], const uint8_t blk[64])
{
    uint32_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = (uint32_t)blk[4 * i] | (uint32_t)blk[4 * i + 1] << 8 |
               (uint32_t)blk[4 * i + 2] << 16 | (uint32_t)blk[4 * i + 3] << 24;
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];

    STEP(F1, a, b, c, d, x[0], 7, 0xd76aa478u);
    STEP(F1, d, a, b, c, x[1], 12, 0xe8c7b756u);
    STEP(F1, c, d, a, b, x[2], 17, 0x242070dbu);
    STEP(F1, b, c, d, a, x[3], 22, 0xc1bdceeeu);
    STEP(F1, a, b, c, d, x[4], 7, 0xf57c0fafu);
    STEP(F1, d, a, b, c, x[5], 12, 0x4787c62au);
    STEP(F1, c, d, a, b, x[6], 17, 0xa8304613u);
    STEP(F1, b, c, d, a, x[7], 22, 0xfd469501u);
    STEP(F1, a, b, c, d, x[8], 7, 0x698098d8u);
    STEP(F1, d, a, b, c, x[9], 12, 0x8b44f7afu);
    STEP(F1, c, d, a, b, x[10], 17, 0xffff5bb1u);
    STEP(F1, b, c, d, a, x[11], 22, 0x895cd7beu);
    STEP(F1, a, b, c, d, x[12], 7, 0x6b901122u);
    STEP(F1, d, a, b, c, x[13], 12, 0xfd987193u);
    STEP(F1, c, d, a, b, x[14], 17, 0xa679438eu);
    STEP(F1, b, c, d, a, x[15], 22, 0x49b40821u);

    STEP(F2, a, b, c, d, x[1], 5, 0xf61e2562u);
    STEP(F2, d, a, b, c, x[6], 9, 0xc040b340u);
    STEP(F2, c, d, a, b, x[11], 14, 0x265e5a51u);
    STEP(F2, b, c, d, a, x[0], 20, 0xe9b6c7aau);
    STEP(F2, a, b, c, d, x[5], 5, 0xd62f105du);
    STEP(F2, d, a, b, c, x[10], 9, 0x02441453u);
    STEP(F2, c, d, a, b, x[15], 14, 0xd8a1e681u);
    STEP(F2, b, c, d, a, x[4], 20, 0xe7d3fbc8u);
    STEP(F2, a, b, c, d, x[9], 5, 0x21e1cde6u);
    STEP(F2, d, a, b, c, x[14], 9, 0xc33707d6u);
    STEP(F2, c, d, a, b, x[3], 14, 0xf4d50d87u);
    STEP(F2, b, c, d, a, x[8], 20, 0x455a14edu);
    STEP(F2, a, b, c, d, x[13], 5, 0xa9e3e905u);
    STEP(F2, d, a, b, c, x[2], 9, 0xfcefa3f8u);
    STEP(F2, c, d, a, b, x[7], 14, 0x676f02d9u);
    STEP(F2, b, c, d, a, x[12], 20, 0x8d2a4c8au);

    STEP(F3, a, b, c, d, x[5], 4, 0xfffa3942u);
    STEP(F3, d, a, b, c, x[8], 11, 0x8771f681u);
    STEP(F3, c, d, a, b, x[11], 16, 0x6d9d6122u);
    STEP(F3, b, c, d, a, x[14], 23, 0xfde5380cu);
    STEP(F3, a, b, c, d, x[1], 4, 0xa4beea44u);
    STEP(F3, d, a, b, c, x[4], 11, 0x4bdecfa9u);
    STEP(F3, c, d, a, b, x[7], 16, 0xf6bb4b60u);
    STEP(F3, b, c, d, a, x[10], 23, 0xbebfbc70u);
    STEP(F3, a, b, c, d, x[13], 4, 0x289b7ec6u);
    STEP(F3, d, a, b, c, x[0], 11, 0xeaa127fau);
    STEP(F3, c, d, a, b, x[3], 16, 0xd4ef3085u);
    STEP(F3, b, c, d, a, x[6], 23, 0x04881d05u);
    STEP(F3, a, b, c, d, x[9], 4, 0xd9d4d039u);
    STEP(F3, d, a, b, c, x[12], 11, 0xe6db99e5u);
    STEP(F3, c, d, a, b, x[15], 16, 0x1fa27cf8u);
    STEP(F3, b, c, d, a, x[2], 23, 0xc4ac5665u);

    STEP(F4, a, b, c, d, x[0], 6, 0xf4292244u);
    STEP(F4, d, a, b, c, x[7], 10, 0x432aff97u);
    STEP(F4, c, d, a, b, x[14], 15, 0xab9423a7u);
    STEP(F4, b, c, d, a, x[5], 21, 0xfc93a039u);
    STEP(F4, a, b, c, d, x[12], 6, 0x655b59c3u);
    STEP(F4, d, a, b, c, x[3], 10, 0x8f0ccc92u);
    STEP(F4, c, d, a, b, x[10], 15, 0xffeff47du);
    STEP(F4, b, c, d, a, x[1], 21, 0x85845dd1u);
    STEP(F4, a, b, c, d, x[8], 6, 0x6fa87e4fu);
    STEP(F4, d, a, b, c, x[15], 10, 0xfe2ce6e0u);
    STEP(F4, c, d, a, b, x[6], 15, 0xa3014314u);
    STEP(F4, b, c, d, a, x[13], 21, 0x4e0811a1u);
    STEP(F4, a, b, c, d, x[4], 6, 0xf7537e82u);
    STEP(F4, d, a, b, c, x[11], 10, 0xbd3af235u);
    STEP(F4, c, d, a, b, x[2], 15, 0x2ad7d2bbu);
    STEP(F4, b, c, d, a, x[9], 21, 0xeb86d391u);

    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

void ota_md5_init(ota_md5_ctx *c)
{
    c->state[0] = 0x67452301u;
    c->state[1] = 0xefcdab89u;
    c->state[2] = 0x98badcfeu;
    c->state[3] = 0x10325476u;
    c->count_lo = c->count_hi = 0;
}

void ota_md5_update(ota_md5_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t used = c->count_lo & 63;
    uint32_t old = c->count_lo;
    c->count_lo += (uint32_t)len;
    if (c->count_lo < old)
        c->count_hi++;
    if (used) {
        size_t n = 64 - used;
        if (n > len)
            n = len;
        memcpy(c->buf + used, p, n);
        p += n;
        len -= n;
        if (used + n < 64)
            return;
        md5_block(c->state, c->buf);
    }
    while (len >= 64) {
        md5_block(c->state, p);
        p += 64;
        len -= 64;
    }
    memcpy(c->buf, p, len);
}

void ota_md5_final(ota_md5_ctx *c, uint8_t out[16])
{
    uint32_t lo = c->count_lo << 3, hi = (c->count_hi << 3) | (c->count_lo >> 29);
    size_t used = c->count_lo & 63;
    c->buf[used++] = 0x80;
    if (used > 56) {
        memset(c->buf + used, 0, 64 - used);
        md5_block(c->state, c->buf);
        used = 0;
    }
    memset(c->buf + used, 0, 56 - used);
    for (int i = 0; i < 4; i++) {
        c->buf[56 + i] = (uint8_t)(lo >> (8 * i));
        c->buf[60 + i] = (uint8_t)(hi >> (8 * i));
    }
    md5_block(c->state, c->buf);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[4 * i + j] = (uint8_t)(c->state[i] >> (8 * j));
}
