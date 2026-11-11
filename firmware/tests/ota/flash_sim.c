/* flash_sim.c - simulated W25Q16 (2 MiB NOR, 4 KiB sectors) implementing
 * ota_flash.h, with power-cut and fault injection, plus a boot2 model. */
#include <stdio.h>
#include <string.h>
#include "sim.h"

uint8_t sim_flash[OTA_FLASH_SIZE];
long sim_budget = -1;
int sim_dead;
long sim_ops;
long sim_nor_violations;
int sim_cut_mode;
long sim_running = -1;
int32_t sim_boot_flags = -1;
uint32_t sim_flip_addr;
long sim_fail_op = -1;

void sim_faults_clear(void)
{
    sim_budget = -1;
    sim_dead = 0;
    sim_flip_addr = 0;
    sim_fail_op = -1;
}

int sim_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(sim_flash, 1, sizeof sim_flash, f);
    fclose(f);
    return n == sizeof sim_flash ? 0 : -1;
}

int sim_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(sim_flash, 1, sizeof sim_flash, f);
    fclose(f);
    return n == sizeof sim_flash ? 0 : -1;
}

int ota_flash_open(void) { return 0; }

int ota_flash_read(uint32_t addr, void *buf, uint32_t len)
{
    if (addr > OTA_FLASH_SIZE || len > OTA_FLASH_SIZE - addr)
        return -1;
    memcpy(buf, sim_flash + addr, len);
    return 0;
}

/* Returns 1 if this op may proceed, 0 if power is (now) gone. */
static int op_begin(int *partial)
{
    *partial = 0;
    if (sim_dead)
        return 0;
    if (sim_budget == 0) {
        sim_dead = 1;
        *partial = 1;
        return 0;
    }
    if (sim_budget > 0)
        sim_budget--;
    return 1;
}

static void program(uint32_t addr, const uint8_t *p, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint8_t v = p[i];
        if (sim_flip_addr && addr + i == sim_flip_addr) {
            v ^= 1;
            sim_flip_addr = 0;
        }
        if (v & ~sim_flash[addr + i])
            sim_nor_violations++;
        sim_flash[addr + i] &= v;
    }
}

int ota_flash_write(uint32_t addr, const void *buf, uint32_t len)
{
    int partial;
    if (addr > OTA_FLASH_SIZE || len > OTA_FLASH_SIZE - addr)
        return -1;
    if (sim_fail_op >= 0 && sim_ops == sim_fail_op) {
        sim_ops++;
        return -1;
    }
    if (!op_begin(&partial)) {
        if (partial)
            program(addr, buf, len / 2);    /* cut half way through */
        return -1;
    }
    sim_ops++;
    program(addr, buf, len);
    return 0;
}

int ota_flash_erase_sector(uint32_t addr)
{
    int partial;
    if (addr % OTA_FLASH_SECTOR || addr >= OTA_FLASH_SIZE)
        return -1;
    if (sim_fail_op >= 0 && sim_ops == sim_fail_op) {
        sim_ops++;
        return -1;
    }
    if (!op_begin(&partial)) {
        if (partial) {
            uint32_t h = OTA_FLASH_SECTOR / 2;
            memset(sim_flash + addr + (sim_cut_mode ? h : 0), 0xFF, h);
        }
        return -1;
    }
    sim_ops++;
    memset(sim_flash + addr, 0xFF, OTA_FLASH_SECTOR);
    return 0;
}

int ota_flash_running_offset(uint32_t *offset)
{
    if (sim_running < 0)
        return -1;
    *offset = (uint32_t)sim_running;
    return 0;
}

int32_t ota_flash_boot_flags(void) { return sim_boot_flags; }

/* ------------------------------------------------------------ boot2 model */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* CRC exactly as boot2's HW engine (CRC32_IEEE, init 0, no xorout),
 * implemented bitwise here, independently of ota_crc.c. */
static uint32_t crc_bitwise(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

static int image_boots(uint32_t start)
{
    const uint8_t *s = sim_flash + start;
    if (start + 200 > OTA_FLASH_SIZE || rd32(s) != 0x4C56524Du || rd32(s + 4) != 0x2E9CF17Bu)
        return 0;
    uint32_t n = rd32(s + 12);
    if (n == 0 || n > 9)
        return 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *h = s + 20 + 20 * i;
        uint32_t type = rd32(h), off = rd32(h + 4), len = rd32(h + 8), crc = rd32(h + 16);
        if (type != 2 || off > OTA_FLASH_SIZE - start || len > OTA_FLASH_SIZE - start - off)
            return 0;
        if (crc_bitwise(s + off, len) != crc)
            return 0;
    }
    return 1;
}

int sim_boot2(uint32_t *slot, int *fell_back)
{
    static const uint32_t taddr[2] = { 0x4000, 0x5000 };
    uint32_t gen[2], n[2];
    for (int t = 0; t < 2; t++) {
        const uint8_t *h = sim_flash + taddr[t];
        int ok = rd32(h) == 0x54504D57u;          /* magic only, no CRC */
        gen[t] = ok ? rd32(h + 8) : 0;
        n[t] = ok ? (uint32_t)(h[6] | h[7] << 8) : 0;
    }
    int sel = gen[1] > gen[0];
    const uint8_t *tb = sim_flash + taddr[sel];
    uint32_t start[2] = { 0, 0 }, egen[2] = { 0, 0 };
    int found = 0;
    for (uint32_t i = 0; i < n[sel] && i < 160 && found < 2; i++) {
        const uint8_t *e = tb + 16 + 24 * i;
        if (e[0] == 1) {
            start[found] = rd32(e + 12);
            egen[found] = rd32(e + 20);
            found++;
        }
    }
    if (found < 2)
        return -1;
    int idx = egen[1] > egen[0];
    *fell_back = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (start[idx] < OTA_FLASH_SIZE && image_boots(start[idx])) {
            *slot = start[idx];
            return 0;
        }
        idx ^= 1;
        *fell_back = 1;
    }
    return -1;
}
