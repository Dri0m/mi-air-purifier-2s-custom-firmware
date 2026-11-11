/* ota_flash.h - tiny flash backend used by the OTA module (internal).
 *
 * Device: ota_flash_mw300.c (WMSDK flash_drv_* on the internal QSPI flash).
 * Host tests: tests/ota/flash_sim.c (2 MiB simulated NOR flash file).
 *
 * Addresses are absolute flash offsets (0 .. 2 MiB).  Erase works on whole
 * 4 KiB sectors only; write programs bytes (NOR: can only clear bits) and
 * may cross page boundaries.  All calls return 0 on success, <0 on error.
 * The OTA core never calls write/erase outside the window it has armed
 * (see ota_guard_* in ota.c), so a backend does not need its own policy. */
#ifndef OTA_FLASH_H
#define OTA_FLASH_H
#include <stdint.h>

#define OTA_FLASH_SIZE    0x200000u
#define OTA_FLASH_SECTOR  0x1000u

int ota_flash_open(void);
int ota_flash_read(uint32_t addr, void *buf, uint32_t len);
int ota_flash_write(uint32_t addr, const void *buf, uint32_t len);
int ota_flash_erase_sector(uint32_t addr);          /* addr 4 KiB aligned */

/* Flash offset currently mapped at the XIP window 0x1F000000, i.e. the start
 * of the appfw slot boot2 actually booted (after any CRC fallback or the
 * GPIO43 strap).  Returns 0 and sets *offset, or <0 if unknown. */
int ota_flash_running_offset(uint32_t *offset);

/* boot2's boot flags word (bit0-1 = appfw index booted, 0x20 = fell back
 * because the preferred image was bad, 0x100 = backup table used), or -1
 * if unknown. */
int32_t ota_flash_boot_flags(void);
#endif
