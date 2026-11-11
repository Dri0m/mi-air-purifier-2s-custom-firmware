/* wmpt.c - Marvell WMPT partition table: parse, select like boot2, write
 * the ping-pong copy safely.  Portable (uses ota_flash.h only).
 *
 * boot2 behaviour this mirrors (disassembly of evidence/dumps/re/boot2.bin):
 *  - 0x3ee: reads the 16-byte header at 0x4000 / 0x5000; on read error or
 *    magic != 'WMPT' it zeroes n and gen, so a blank/half-written table
 *    loses.  No CRC check.
 *  - 0x49a: backup (0x5000) wins only if its gen is strictly higher.
 *  - 0x4da..0x55a: first two entries with type == 1 (name not checked);
 *    the second boots only if its gen is strictly higher.
 *  - 0x4b8..0x562: GPIO43 low at reset forces the second appfw entry.
 * The prebuilt libpart's part_set_active_partition() (disassembled from
 * libpart.a) writes: erase other sector, header, entries, trailer - in that
 * order, so a power cut after the header leaves a table boot2 accepts with
 * 0xFF entries.  We write everything except the magic first, verify it, and
 * program the 4 magic bytes last. */
#include <string.h>
#include "ota_internal.h"
#include "ota_flash.h"
#include "ota_crc.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int wmpt_parse(const uint8_t *buf, size_t len, wmpt_table_t *t)
{
    if (len < WMPT_HDR || rd32(buf) != WMPT_MAGIC)
        return OTA_E_TABLE;
    uint16_t ver = (uint16_t)(buf[4] | buf[5] << 8);
    uint16_t n = (uint16_t)(buf[6] | buf[7] << 8);
    if (ver != WMPT_VERSION || n == 0 || n > WMPT_MAX_ENTRIES)
        return OTA_E_TABLE;
    if (ota_crc32(0, buf, 12) != rd32(buf + 12))
        return OTA_E_TABLE;
    size_t body = (size_t)WMPT_ENT * n;
    if (len < WMPT_HDR + body + 4)
        return OTA_E_TABLE;
    if (ota_crc32(0, buf + WMPT_HDR, body) != rd32(buf + WMPT_HDR + body))
        return OTA_E_TABLE;
    t->version = ver;
    t->n = n;
    t->gen = rd32(buf + 8);
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *p = buf + WMPT_HDR + WMPT_ENT * i;
        wmpt_entry_t *e = &t->e[i];
        e->type = p[0];
        e->device = p[1];
        memcpy(e->name, p + 2, 8);
        e->flag = p[10];
        e->pad1 = p[11];
        e->start = rd32(p + 12);
        e->size = rd32(p + 16);
        e->gen = rd32(p + 20);
    }
    return OTA_OK;
}

size_t wmpt_serialize(const wmpt_table_t *t, uint8_t *buf, size_t cap)
{
    size_t body = (size_t)WMPT_ENT * t->n;
    size_t total = WMPT_HDR + body + 4;
    if (t->n == 0 || t->n > WMPT_MAX_ENTRIES || cap < total)
        return 0;
    wr32(buf, WMPT_MAGIC);
    buf[4] = (uint8_t)t->version; buf[5] = (uint8_t)(t->version >> 8);
    buf[6] = (uint8_t)t->n;       buf[7] = (uint8_t)(t->n >> 8);
    wr32(buf + 8, t->gen);
    wr32(buf + 12, ota_crc32(0, buf, 12));
    for (unsigned i = 0; i < t->n; i++) {
        uint8_t *p = buf + WMPT_HDR + WMPT_ENT * i;
        const wmpt_entry_t *e = &t->e[i];
        p[0] = e->type;
        p[1] = e->device;
        memcpy(p + 2, e->name, 8);
        p[10] = e->flag;
        p[11] = e->pad1;
        wr32(p + 12, e->start);
        wr32(p + 16, e->size);
        wr32(p + 20, e->gen);
    }
    wr32(buf + WMPT_HDR + body, ota_crc32(0, buf + WMPT_HDR, body));
    return total;
}

static uint32_t header_gen(uint32_t addr, int *valid)
{
    uint8_t h[WMPT_HDR];
    *valid = 0;
    if (ota_flash_read(addr, h, sizeof h) != 0 || rd32(h) != WMPT_MAGIC)
        return 0;                       /* boot2: unreadable/bad magic -> gen 0 */
    *valid = 1;
    return rd32(h + 8);
}

static int overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
    return a < b + blen && b < a + alen;
}

int wmpt_load(wmpt_state_t *s)
{
    int v0, v1;
    uint32_t g0 = header_gen(WMPT_ADDR0, &v0);
    uint32_t g1 = header_gen(WMPT_ADDR1, &v1);
    if (!v0 && !v1)
        return OTA_E_TABLE;
    int sel = g1 > g0;                  /* exactly boot2's "hi" compare */
    s->addr = sel ? WMPT_ADDR1 : WMPT_ADDR0;
    s->other_addr = sel ? WMPT_ADDR0 : WMPT_ADDR1;

    /* The table boot2 uses must be fully valid before we derive anything
     * from it (boot2 itself does not check its CRCs). */
    uint8_t *buf = ota_scratch;
    if (ota_flash_read(s->addr, buf, WMPT_MAX_BYTES) != 0)
        return OTA_E_FLASH;
    if (wmpt_parse(buf, WMPT_MAX_BYTES, &s->t) != OTA_OK) {
        OTA_LOG("table @%x invalid\r\n", (unsigned)s->addr);
        return OTA_E_TABLE;
    }
    if (s->t.gen >= 0xFFFF0000u)
        return OTA_E_TABLE;

    int k = 0;
    for (int i = 0; i < s->t.n && k < 2; i++)
        if (s->t.e[i].type == WMPT_TYPE_APPFW)
            s->app[k++] = i;
    if (k < 2)
        return OTA_E_LAYOUT;

    for (int j = 0; j < 2; j++) {
        const wmpt_entry_t *e = &s->t.e[s->app[j]];
        if (e->device != 0 || e->start % OTA_FLASH_SECTOR || e->size % OTA_FLASH_SECTOR ||
            e->size < 0x10000u || e->start < OTA_PROTECT_END ||
            e->start > OTA_FLASH_SIZE || e->size > OTA_FLASH_SIZE - e->start ||
            e->gen >= 0xFFFF0000u)
            return OTA_E_LAYOUT;
        for (int i = 0; i < s->t.n; i++) {
            const wmpt_entry_t *o = &s->t.e[i];
            if (i != s->app[j] && overlaps(e->start, e->size, o->start, o->size))
                return OTA_E_LAYOUT;
        }
    }
#ifndef OTA_ALLOW_ANY_LAYOUT
    /* Belt and braces: this unit's verified layout. */
    if (s->t.e[s->app[0]].start != OTA_SLOT_A || s->t.e[s->app[1]].start != OTA_SLOT_B ||
        s->t.e[s->app[0]].size != OTA_SLOT_SIZE || s->t.e[s->app[1]].size != OTA_SLOT_SIZE)
        return OTA_E_LAYOUT;
#endif
    s->active = s->t.e[s->app[1]].gen > s->t.e[s->app[0]].gen;
    return OTA_OK;
}

/* Write nt (gen = current + 1) to the ping-pong sector.  Power-cut safe:
 * until the final 4-byte magic program completes, boot2 sees no table there
 * and keeps using the current one. */
int wmpt_write_next(const wmpt_state_t *s, wmpt_table_t *nt)
{
    uint8_t *buf = ota_scratch;
    uint8_t *chk = ota_scratch + 512;
    nt->version = WMPT_VERSION;
    nt->gen = s->t.gen + 1;
    size_t len = wmpt_serialize(nt, buf, 512);
    if (len == 0 || len > 512)
        return OTA_E_TABLE;

    int rc = OTA_E_COMMIT;
    ota_guard_arm(s->other_addr, s->other_addr + OTA_FLASH_SECTOR);
    if (ota_guarded_erase(s->other_addr) != 0)
        goto out;
    if (ota_guarded_write(s->other_addr + 4, buf + 4, (uint32_t)len - 4) != 0)
        goto undo;
    if (ota_flash_read(s->other_addr, chk, (uint32_t)len) != 0 ||
        memcmp(chk + 4, buf + 4, len - 4) != 0 ||
        rd32(chk) != 0xFFFFFFFFu)
        goto undo;
    if (ota_guarded_write(s->other_addr, buf, 4) != 0)
        goto undo;
    if (ota_flash_read(s->other_addr, chk, (uint32_t)len) != 0 ||
        memcmp(chk, buf, len) != 0)
        goto undo;
    rc = OTA_OK;
    goto out;
undo:
    /* Remove whatever is there so boot2 cannot pick a damaged table. */
    (void)ota_guarded_erase(s->other_addr);
out:
    ota_guard_disarm();
    return rc;
}
