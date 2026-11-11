/* ota_image.c - verify an MRVL appfw image by reading it back from flash.
 *
 * Rules (fw-boot-ota-build.md 2.2, boot2 loader 0x7f8..0x970, mrvl_pack.py):
 *  - header 'MRVL', sig 0x2E9CF17B, 1..9 segments (boot2 reads 9 headers);
 *  - every segment: type 2 (load), non-empty, after the header table,
 *    inside the declared image, not overlapping another segment;
 *  - XIP segment (laddr in 0x1F000000..): executes in place, so
 *    laddr - 0x1F000000 must equal its file offset;
 *  - RAM segment: entirely inside SRAM0 below boot2 (0x00100000..0x0015C000;
 *    boot2 runs at 0x0015C000 while it copies) or SRAM1 below 0x20010C6C
 *    (boot2's stack is at the SRAM1 top; same limit as mrvl_pack.py);
 *  - entry: Thumb bit set, inside an XIP segment;
 *  - each segment's CRC-32 (init 0, no xorout) recomputed from flash. */
#include <string.h>
#include "ota_internal.h"
#include "ota_flash.h"
#include "ota_crc.h"

#define SRAM0_LO 0x00100000u
#define SRAM0_HI 0x0015C000u    /* boot2 load address: never overwrite it */
#define SRAM1_LO 0x20000000u
/* boot2's stack is at the top of SRAM1 (SP = 0x20020000) while it copies
 * the segments; the stock image's SRAM1 segment ends at 0x20010C6C, the
 * only proven-safe limit.  Same rule as tools/mrvl_pack.py
 * (SRAM1_LOAD_LIMIT): an image that loads further could crash boot2 on
 * every boot, with no fallback. */
#define SRAM1_HI 0x20010C6Cu
#define XIP_SPAN 0x01000000u

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int within(uint32_t a, uint32_t len, uint32_t lo, uint32_t hi)
{
    return a >= lo && a <= hi && len <= hi - a;
}

int mrvl_verify_flash(uint32_t slot_start, uint32_t image_len, uint32_t slot_size,
                      const uint8_t *magic_override, mrvl_info_t *info)
{
    uint8_t hdr[20 + 20 * MRVL_MAX_SEGS];
    struct { uint32_t off, len, laddr, crc; } seg[MRVL_MAX_SEGS];

    if (image_len < sizeof hdr || image_len > slot_size)
        return OTA_E_IMAGE;
    if (ota_flash_read(slot_start, hdr, sizeof hdr) != 0)
        return OTA_E_FLASH;
    if (magic_override)
        memcpy(hdr, magic_override, 4);
    if (rd32(hdr) != MRVL_MAGIC || rd32(hdr + 4) != MRVL_SIG)
        return OTA_E_IMAGE;
    uint32_t n = rd32(hdr + 12), entry = rd32(hdr + 16);
    if (n == 0 || n > MRVL_MAX_SEGS)
        return OTA_E_IMAGE;
    uint32_t hdr_end = 20 + 20 * n, end = 0;
    int entry_ok = 0;

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = hdr + 20 + 20 * i;
        uint32_t type = rd32(p);
        seg[i].off = rd32(p + 4);
        seg[i].len = rd32(p + 8);
        seg[i].laddr = rd32(p + 12);
        seg[i].crc = rd32(p + 16);
        if (type != MRVL_SEG_LOAD || seg[i].len == 0 || seg[i].off < hdr_end ||
            seg[i].off > image_len || seg[i].len > image_len - seg[i].off)
            return OTA_E_IMAGE;
        for (uint32_t j = 0; j < i; j++)
            if (seg[i].off < seg[j].off + seg[j].len && seg[j].off < seg[i].off + seg[i].len)
                return OTA_E_IMAGE;
        uint32_t a = seg[i].laddr;
        if (a >= MRVL_XIP_BASE && a - MRVL_XIP_BASE < XIP_SPAN) {
            if (a - MRVL_XIP_BASE != seg[i].off)
                return OTA_E_IMAGE;
            uint32_t ep = entry & ~1u;
            if ((entry & 1u) && ep >= a && ep - a < seg[i].len)
                entry_ok = 1;
        } else if (!within(a, seg[i].len, SRAM0_LO, SRAM0_HI) &&
                   !within(a, seg[i].len, SRAM1_LO, SRAM1_HI)) {
            return OTA_E_IMAGE;
        }
        if (seg[i].off + seg[i].len > end)
            end = seg[i].off + seg[i].len;
    }
    if (!entry_ok)
        return OTA_E_IMAGE;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t crc = 0, done = 0;
        while (done < seg[i].len) {
            uint32_t chunk = seg[i].len - done;
            if (chunk > OTA_SCRATCH)
                chunk = OTA_SCRATCH;
            if (ota_flash_read(slot_start + seg[i].off + done, ota_scratch, chunk) != 0)
                return OTA_E_FLASH;
            crc = ota_crc32(crc, ota_scratch, chunk);
            done += chunk;
        }
        if (crc != seg[i].crc) {
            OTA_LOG("seg%u crc %08x != %08x\r\n", (unsigned)i, (unsigned)crc, (unsigned)seg[i].crc);
            return OTA_E_CRC;
        }
    }
    if (info) {
        info->time = rd32(hdr + 8);
        info->nsegs = n;
        info->entry = entry;
        info->end = end;
    }
    return OTA_OK;
}
