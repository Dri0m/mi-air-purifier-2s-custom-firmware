/* diag_stub.h - host-only test hooks of diag_stub.c: fake include/diag.h,
 * the two ota.h calls and hal_reboot() used by the ESPHome diagnostic
 * entities (api_entities.c).  Not for the device. */
#ifndef DIAG_STUB_H
#define DIAG_STUB_H
#include <stdint.h>

struct diag_stub {
	const char *version;    /* diag_fw_version() */
	uint32_t pmu_cause;     /* reset cause bits -> diag_reset_reason() */
	uint8_t prev_tag;       /* reboot tag of the "previous run" */
	uint32_t uptime_s, heap;
	char running;           /* ota_get_info(): 'A' / 'B' */
	uint8_t trial, can_revert, in_progress, update_pending;
	int reboots;            /* hal_reboot() calls */
	uint8_t last_tag;       /* last diag_set_reboot_tag() */
};
/* Defaults: version "0.1.0-host", cause 0 (power_on), uptime 0, heap
 * 100000, slot A, not a trial, no upload. */
extern struct diag_stub diag_stub;
#endif
