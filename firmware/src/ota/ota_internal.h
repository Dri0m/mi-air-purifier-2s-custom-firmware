/* ota_internal.h - shared between the OTA sources and host tests. */
#ifndef OTA_INTERNAL_H
#define OTA_INTERNAL_H
#include <stddef.h>
#include <stdint.h>
#include "ota_ext.h"

#ifdef OTA_LOG_FN
int OTA_LOG_FN(const char *fmt, ...);
#define OTA_LOG(...) ((void)OTA_LOG_FN("[ota] " __VA_ARGS__))
#else
#define OTA_LOG(...) ((void)0)
#endif

/* ---- fixed flash layout facts (fw-boot-ota-build.md) ---- */
#define WMPT_ADDR0        0x4000u       /* primary table sector */
#define WMPT_ADDR1        0x5000u       /* backup table sector */
#define OTA_SLOT_A        0x00A000u
#define OTA_SLOT_B        0x08A000u
#define OTA_SLOT_SIZE     0x80000u
#define OTA_PROTECT_END   0x00A000u     /* boot2 + tables + PSM below this */

/* ---- WMPT ---- */
#define WMPT_MAGIC        0x54504D57u   /* 'WMPT' */
#define WMPT_VERSION      1u
#define WMPT_HDR          16u
#define WMPT_ENT          24u
#define WMPT_MAX_ENTRIES  16u
#define WMPT_MAX_BYTES    (WMPT_HDR + WMPT_ENT * WMPT_MAX_ENTRIES + 4u)
#define WMPT_TYPE_APPFW   1u

/* Boot-confirm flag kept in entry pad[0] (ignored by boot2 and libpart). */
#define WMPT_FLAG_NONE    0x00u
#define WMPT_FLAG_TRIAL   0x54u         /* 'T' committed, not booted yet */
#define WMPT_FLAG_BOOTED  0x42u         /* 'B' booted once, previous slot re-activated */
/* Confirmation mark kept in entry pad1: set by ota_mark_good() (or by a
 * commit with OTA_BOOT_CONFIRM=0).  An image whose entry lacks it was never
 * confirmed by this firmware - e.g. the first boot after a stock miIO OTA or
 * a UART install, which write pad1 = 0 - and boots as a trial too. */
#define WMPT_CONF_MARK    0x43u         /* 'C' */

typedef struct {
    uint8_t type, device;
    char name[8];
    uint8_t flag, pad1;
    uint32_t start, size, gen;
} wmpt_entry_t;

typedef struct {
    uint16_t version, n;
    uint32_t gen;
    wmpt_entry_t e[WMPT_MAX_ENTRIES];
} wmpt_table_t;

typedef struct {
    wmpt_table_t t;         /* table boot2 selects (fully validated copy) */
    uint32_t addr;          /* its sector */
    uint32_t other_addr;    /* ping-pong sector the next write goes to */
    int app[2];             /* entry indices of the first two type-1 entries */
    int active;             /* 0/1: appfw boot2 picks by gen */
} wmpt_state_t;

int wmpt_parse(const uint8_t *buf, size_t len, wmpt_table_t *t);
size_t wmpt_serialize(const wmpt_table_t *t, uint8_t *buf, size_t cap);
int wmpt_load(wmpt_state_t *s);
int wmpt_write_next(const wmpt_state_t *s, wmpt_table_t *nt);

/* ---- MRVL image ---- */
#define MRVL_MAGIC        0x4C56524Du   /* 'MRVL' */
#define MRVL_SIG          0x2E9CF17Bu
#define MRVL_MAX_SEGS     9u            /* boot2 reads 9 segment headers */
#define MRVL_XIP_BASE     0x1F000000u
#define MRVL_SEG_LOAD     2u

typedef struct {
    uint32_t time, nsegs, entry, end;   /* end = max(seg offset+len) */
} mrvl_info_t;

/* Verify an image in flash at slot_start: header, segments, CRCs read back
 * from flash.  image_len bounds all segments (use slot_size when unknown).
 * magic_override != NULL substitutes the first 4 bytes (not yet written). */
int mrvl_verify_flash(uint32_t slot_start, uint32_t image_len, uint32_t slot_size,
                      const uint8_t *magic_override, mrvl_info_t *info);

/* ---- guard (ota.c) ---- */
int ota_guarded_write(uint32_t addr, const void *buf, uint32_t len);
int ota_guarded_erase(uint32_t addr);
void ota_guard_arm(uint32_t lo, uint32_t hi);
void ota_guard_disarm(void);

/* Shared scratch buffer for verification reads (ota.c). */
#define OTA_SCRATCH 1024u
extern uint8_t ota_scratch[OTA_SCRATCH];

/* Host tests: forget all RAM state (simulates a reboot). */
void ota_reset_state(void);
void ota_server_reset_state(void);
#endif
