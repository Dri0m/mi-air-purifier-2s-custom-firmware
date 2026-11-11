/* ota_flash_mw300.c - OTA flash backend on the 88MW300 (WMSDK flash driver).
 *
 * Uses flash_drv_* (sdk/src/core/util/flash.c -> drivers/mw300/mdev_iflash.c),
 * which handles running from XIP: erase/program run from RAM with the flash
 * controller switched to QSPI mode and IRQs masked, and flush the FLASHC
 * cache afterwards, so later reads (memory-mapped through 0x1F000000 for
 * addresses >= the XIP offset, QSPI otherwise) see the real flash content.
 * Buffers passed in must be in RAM (they are: static buffers in src/ota).
 *
 * We deliberately do not use libpart's part_set_active_partition(): it does
 * not check flash return codes, programs the table header (with the magic)
 * before the entries, picks the target sector from boot flags captured at
 * boot (stale after a first switch) and needs the HW CRC driver.  wmpt.c
 * reproduces its result byte-for-byte with a power-cut-safe write order.
 *
 * Requires (device Makefile): sdk/src/core/util/flash.c,
 * sdk/src/drivers/mw300/mdev_iflash.c, lowlevel/mw300_flash.c,
 * lowlevel/mw300_flashc.c, lowlevel/mw300_qspi.c, mdev core, wm_os (mutex). */
#include <stdint.h>
#include <flash.h>
#include <lowlevel_drivers.h>
#include "ota_flash.h"

#define BOOT_FLAGS_REG  ((volatile uint32_t *)0x480C0024u)  /* boot_flags.h, written by boot2 */
#define WRITE_CHUNK     1024u   /* bounds the IRQ-off window per program call */

static mdev_t *fl;

int ota_flash_open(void)
{
    if (fl)
        return 0;
    flash_drv_init();                   /* idempotent */
    fl = flash_drv_open(FL_INT);
    return fl ? 0 : -1;
}

static int range_ok(uint32_t addr, uint32_t len)
{
    return addr <= OTA_FLASH_SIZE && len <= OTA_FLASH_SIZE - addr;
}

int ota_flash_read(uint32_t addr, void *buf, uint32_t len)
{
    if (!range_ok(addr, len) || ota_flash_open() != 0)
        return -1;
    if (len == 0)
        return 0;
    return flash_drv_read(fl, (uint8_t *)buf, len, addr) == 0 ? 0 : -1;
}

int ota_flash_write(uint32_t addr, const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    if (!range_ok(addr, len) || ota_flash_open() != 0)
        return -1;
    while (len) {
        uint32_t n = len > WRITE_CHUNK ? WRITE_CHUNK : len;
        if (flash_drv_write(fl, p, n, addr) != 0)
            return -1;
        p += n;
        addr += n;
        len -= n;
    }
    return 0;
}

int ota_flash_erase_sector(uint32_t addr)
{
    if ((addr & (OTA_FLASH_SECTOR - 1)) || !range_ok(addr, OTA_FLASH_SECTOR) ||
        ota_flash_open() != 0)
        return -1;
    return flash_drv_erase(fl, addr, OTA_FLASH_SECTOR) == 0 ? 0 : -1;
}

int ota_flash_running_offset(uint32_t *offset)
{
    /* boot2 programs the FLASHC address offset so 0x1F000000 maps the slot
     * it booted (fw-boot-ota-build.md 1.5); mdev_iflash.c reads it the same
     * way. */
    if (FLASHC->FCACR.BF.OFFSET_EN) {
        *offset = FLASHC->FAOFFR.BF.OFFSET_VAL;
        return 0;
    }
    return -1;
}

int32_t ota_flash_boot_flags(void)
{
    /* First 64 bytes of NVRAM are reserved for the boot ROM/boot2 in the
     * SDK linker script, so the word is still intact at runtime. */
    return (int32_t)(*BOOT_FLAGS_REG & 0x7FFFFFFFu);
}
