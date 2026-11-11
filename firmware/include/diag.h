/* diag.h - device diagnostics for the HA diagnostic entities (U2) and the
 * firmware build identity (U1).
 *
 * Device implementation: src/diag/diag_mw300.c.  Host (ESPHome API tests):
 * src/host/diag_stub.c.  All calls from the app task. */
#ifndef DIAG_H
#define DIAG_H
#include <stdint.h>

/* Build identity.  FW_VERSION (`git describe --always --dirty --tags`, or
 * "unknown") comes from the generated build/gen/fw_version.h that the
 * device Makefile force-includes.  fw_build_time is the link time,
 * "YYYY-MM-DD HH:MM:SS UTC", generated into build/gen/build_info.c on
 * every link (the same second is the MRVL header timestamp); a weak
 * "unknown" default lives in src/ota/ota_server.c for host builds. */
extern const char fw_build_time[];
const char *diag_fw_version(void);

/* Capture the reset cause and the reboot tag of the previous run, then
 * clear the tag.  Idempotent; call early in main() (also called lazily). */
void diag_init(void);

/* Why this boot happened, one short word:
 *   "power_on"  no PMU cause bit (power cycle / reset pin)
 *   "brownout"  VBAT / V12 / AV18 brown-out (PMU bits 0-2)
 *   "watchdog"  WDT reset (bit 5)
 *   "lockup"    Cortex-M4 lockup (bit 4)
 *   "restart"   SYSRESETREQ (bit 3) after the HA Restart button
 *   "software"  SYSRESETREQ otherwise: OTA reboot, crash/assert reset,
 *               hang guard, Wi-Fi reset combo
 * Highest priority first: watchdog, lockup, software/restart, brownout. */
const char *diag_reset_reason(void);
/* The mapping itself (src/diag/diag_reason.c, portable): PMU
 * LAST_RST_CAUSE bits + the previous run's reboot tag (0 = none). */
const char *diag_reason_from_cause(uint32_t cause, uint8_t tag);

/* Tag the next reset as deliberate (survives SYSRESETREQ in retention RAM;
 * read and cleared by the next boot's diag_init()). */
#define DIAG_REBOOT_RESTART 1   /* HA Restart button */
void diag_set_reboot_tag(uint8_t code);

/* Seconds since boot (does not wrap at 49 days as long as it is called
 * at least once per 49 days). */
uint32_t diag_uptime_s(void);
/* Free heap bytes right now. */
uint32_t diag_free_heap(void);
#endif
