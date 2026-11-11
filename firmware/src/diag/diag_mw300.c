/* diag_mw300.c - include/diag.h on the 88MW300 (device only). */
#include <stdbool.h>
#include <FreeRTOS.h>
#include <task.h>
#include <boot_flags.h>              /* boot_reset_cause() = g_rst_cause */
#include "diag.h"

#ifndef FW_VERSION
#define FW_VERSION "unknown"         /* build/gen/fw_version.h missing */
#endif

/* Reboot tag in retention RAM (NVRAM 0x480C0000, linker section
 * .nvram_uninit.*): placed after the SDK's own .nvram_uninit variables
 * (pm4_sig, hwrtc), so their addresses do not move, and outside the
 * .nvram range that mdev_startup.c zero-fills on every non-PM4 boot.
 * Garbage after a power cycle is rejected by the magic + complement. */
#define TAG_MAGIC 0xD1A60000u
static volatile uint32_t reboot_tag[2]
	__attribute__((section(".nvram_uninit.diag"), used));

static bool inited;
static uint8_t prev_tag;
static const char *reason;

void diag_init(void)
{
	uint32_t a, b;
	if (inited)
		return;
	inited = true;
	a = reboot_tag[0];
	b = reboot_tag[1];
	if ((a & 0xFFFF0000u) == TAG_MAGIC && b == ~a)
		prev_tag = (uint8_t)a;
	reboot_tag[0] = 0;
	reboot_tag[1] = 0;
	reason = diag_reason_from_cause(boot_reset_cause(), prev_tag);
}

const char *diag_reset_reason(void)
{
	diag_init();
	return reason;
}

void diag_set_reboot_tag(uint8_t code)
{
	uint32_t a = TAG_MAGIC | code;
	reboot_tag[0] = a;
	reboot_tag[1] = ~a;
}

const char *diag_fw_version(void)
{
	return FW_VERSION;
}

uint32_t diag_uptime_s(void)
{
	static TickType_t last;
	static uint64_t ms;
	TickType_t now = xTaskGetTickCount();
	ms += (uint64_t)(TickType_t)(now - last) * portTICK_PERIOD_MS;
	last = now;
	return (uint32_t)(ms / 1000u);
}

uint32_t diag_free_heap(void)
{
	return (uint32_t)xPortGetFreeHeapSize();
}
