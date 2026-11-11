/*
 * flashwr_core.h - portable core of the guarded 88MW300 flash writer.
 *
 * The same flashwr_core.c runs in two places:
 *   - mw300_flashwr.c: the RAM stub loaded by the UART boot ROM (bit-banged
 *     SPI on GPIO28..33, bit-banged 115200 TX on GPIO2);
 *   - the host tests (w25q_model.c): a simulated W25Q16 that records every
 *     opcode it receives and fails on anything outside the allowlist.
 *
 * Image layout as loaded at LOAD_BASE (0x00110000):
 *   0x0000          stub code/data/bss (must end below FW_PARAM_OFF)
 *   FW_PARAM_OFF    struct fw_params (32 bytes)
 *   FW_PAYLOAD_OFF  payload (WRITE only), fw_params.length bytes
 *
 * HARD GUARDS (enforced here, independently of the host):
 *   WRITE  erase span [offset, offset + roundup(length, 4K)) must lie inside
 *          appfw slot B [0x8A000, 0x10A000), or be exactly one of the table
 *          sectors 0x4000 / 0x5000; offset must be 4 KB aligned.
 *          A table payload must be a WMPT table with the stock layout, may
 *          not overwrite the table boot2 currently selects, must carry a
 *          higher gen, and must select a slot holding a CRC-valid MRVL image.
 *          A slot-B write is refused while boot2 would boot slot B.
 *   CRC    read-only CRC over [0x4000, 0x6000) or [0xA000, 0x10A000).
 *   READ   raw read of the table sectors [0x4000, 0x6000) only.
 * boot2 (0x0-0x3FFF), PSM (0x6000-0x9FFF), slot A and wififw and above are
 * never erased or programmed; PSM is never read.
 *
 * The only SPI opcodes the core can send are listed in FW_CMD_*; the send
 * function traps on anything else.  There is no status-register write,
 * chip/block erase, OTP/security-register or lock command in the code.
 */
#ifndef FLASHWR_CORE_H
#define FLASHWR_CORE_H

#include <stdint.h>

#define FW_PARAM_OFF     0x0FC0u
#define FW_PAYLOAD_OFF   0x1000u
#define FW_PARAM_MAGIC   0x52574C46u          /* "FLWR" little endian */
#define FW_PARAM_VERSION 1u

#define FW_OP_WRITE 1u
#define FW_OP_CRC   2u
#define FW_OP_READ  3u

#define FW_FLAG_DRYRUN 1u                     /* all checks, no erase/program */

#define FW_FLASH_SIZE   0x200000u
#define FW_SECTOR       0x1000u
#define FW_PAGE         0x100u
#define FW_JEDEC_W25Q16 0xEF4015u

#define FW_BOOT2_END    0x4000u
#define FW_TABLE0       0x4000u
#define FW_TABLE1       0x5000u
#define FW_TABLES_END   0x6000u
#define FW_PSM_START    0x6000u
#define FW_PSM_END      0xA000u
#define FW_SLOTA        0x0A000u
#define FW_SLOTB        0x8A000u
#define FW_SLOT_SIZE    0x80000u
#define FW_SLOTB_END    0x10A000u
#define FW_MAX_PAYLOAD  0x10000u              /* 64 KB per session, hard cap */

#define FW_XIP_BASE     0x1F000000u
#define FW_MRVL_MAGIC   0x4C56524Du
#define FW_MRVL_SIG     0x2E9CF17Bu
#define FW_WMPT_MAGIC   0x54504D57u
#define FW_WMPT_N       8u
#define FW_WMPT_LEN     (16u + 24u * FW_WMPT_N + 4u)   /* 0xD4 */

/* SPI opcodes the core may send (W25Q16).  Nothing else exists. */
#define FW_CMD_WREN     0x06u   /* write enable */
#define FW_CMD_PP       0x02u   /* page program */
#define FW_CMD_SE4K     0x20u   /* 4 KB sector erase */
#define FW_CMD_RDSR1    0x05u   /* read status 1 */
#define FW_CMD_RDSR2    0x35u   /* read status 2 */
#define FW_CMD_READ     0x03u   /* read data */
#define FW_CMD_JEDEC    0x9Fu   /* JEDEC ID */
#define FW_CMD_EXITCRM  0xFFu   /* exit continuous read mode */
#define FW_CMD_RSTEN    0x66u   /* enable reset */
#define FW_CMD_RST      0x99u   /* reset device */
#define FW_CMD_RELPD    0xABu   /* release power-down */

struct fw_params {
	uint32_t magic;
	uint32_t version;
	uint32_t op;
	uint32_t offset;
	uint32_t length;
	uint32_t crc;      /* WRITE: crc32 of the payload */
	uint32_t flags;
	uint32_t pcrc;     /* crc32 of the 28 bytes above */
};

/* Reported as "REFUSED <n>" / "ERROR <n>"; uartboot_flash.FW_ERRORS has
 * the names in this order (test_flashwr.py checks it). */
enum {
	FW_OK = 0,
	FW_E_PARAMS,       /* bad magic/version/param CRC */
	FW_E_OP,
	FW_E_LEN,
	FW_E_ALIGN,
	FW_E_RANGE,
	FW_E_TABLE,        /* table payload is not a stock-layout WMPT table */
	FW_E_ACTIVE_TABLE, /* would overwrite the table boot2 selects */
	FW_E_TABLE_GEN,    /* new table gen not above the active one */
	FW_E_IMAGE,        /* slot selected by the new table has no valid image */
	FW_E_SLOT_ACTIVE,  /* slot B is the slot boot2 boots */
	FW_E_LAYOUT,       /* active table not recognised */
	FW_E_PAYLOAD_CRC,
	FW_E_JEDEC,
	FW_E_PROTECTED,
	FW_E_WEL,
	FW_E_TIMEOUT,
	FW_E_VERIFY,
	FW_E_CMD,          /* internal: opcode outside the allowlist */
	FW_E_COUNT
};

/* ---- port, provided by the stub or by the host model ---- */
void fwp_cs(int high);             /* chip select level */
uint8_t fwp_xfer(uint8_t out);     /* clock one byte out/in, MSB first */
void fwp_putc(char c);             /* one byte on the report line */
void fwp_wait(void);               /* ~100 us pause between status polls */

/* ---- pure functions (no flash access) ---- */
uint32_t fw_crc32(uint32_t crc, const uint8_t *p, uint32_t n);
uint32_t fw_params_crc(const struct fw_params *p);
int fw_check_params(const struct fw_params *p);
int fw_check_range(uint32_t op, uint32_t offset, uint32_t length);
int fw_is_table_write(uint32_t op, uint32_t offset);
int fw_check_table(const uint8_t *t, uint32_t len);
/* boot2's choice: returns 0 (0x4000) or 1 (0x5000) from the two headers. */
int fw_boot2_pick_table(const uint8_t *hdr0, const uint8_t *hdr1);
/* For a structurally valid table: 0 = slot A boots, 1 = slot B boots. */
int fw_table_selects_b(const uint8_t *t);

/* ---- the whole run: report, checks, erase/program/verify ---- */
int fw_run(const struct fw_params *p, const uint8_t *payload);

#endif
