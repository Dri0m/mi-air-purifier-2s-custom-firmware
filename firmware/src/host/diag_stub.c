/* diag_stub.c - fake include/diag.h + ota_get_info() /
 * ota_trial_revert_armed() + hal_reboot() for the host ESPHome API tests
 * (tests/esphome).  Host-only; never link into the device image, nor with
 * src/ota or src/host/hal_fake.c.  hal_reboot() only counts and logs
 * "STUB reboot" to stderr. */
#include <stdio.h>
#include <string.h>
#include "diag.h"
#include "diag_stub.h"
#include "hal.h"
#include "ota.h"

struct diag_stub diag_stub = {
	.version = "0.1.0-host",
	.heap = 100000,
	.running = 'A',
};

const char fw_build_time[] = "2026-01-01 00:00:00 UTC";

const char *diag_fw_version(void) { return diag_stub.version; }
void diag_init(void) { }

const char *diag_reset_reason(void)
{
	return diag_reason_from_cause(diag_stub.pmu_cause, diag_stub.prev_tag);
}

void diag_set_reboot_tag(uint8_t code) { diag_stub.last_tag = code; }
uint32_t diag_uptime_s(void) { return diag_stub.uptime_s; }
uint32_t diag_free_heap(void) { return diag_stub.heap; }

int ota_get_info(ota_info_t *info)
{
	memset(info, 0, sizeof *info);
	info->running = info->next_boot = diag_stub.running;
	info->trial = diag_stub.trial;
	info->can_revert = diag_stub.can_revert;
	info->in_progress = diag_stub.in_progress;
	info->update_pending = diag_stub.update_pending;
	return OTA_OK;
}

int ota_trial_revert_armed(void)
{
	return diag_stub.trial && diag_stub.can_revert;
}

void hal_reboot(void)
{
	diag_stub.reboots++;
	fprintf(stderr, "STUB reboot %d -> 0\n", diag_stub.last_tag);
}
