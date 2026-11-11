/* diag_reason.c - PMU reset cause -> short word (portable, host-tested).
 *
 * 88MW300 PMU LAST_RST_CAUSE (PMU+0x08, WMSDK regs/pmu_reg.h):
 *   bit 0 BROWNOUT_VBAT, 1 BROWNOUT_V12, 2 BROWNOUT_AV18,
 *   bit 3 CM3_SYSRESETREQ, 4 CM3_LOCKUP, 5 WDT_RST.
 * The SDK's boot_init() latches it into g_rst_cause before main() and
 * clears it (boot_flags.h boot_store_reset_cause(); boot2 never touches
 * it, evidence/platform-stock-parity.md section 4).  NVIC_SystemReset()
 * (hal_reboot / arch_reboot, platform_reboot, fatal_reset) sets bit 3, so
 * the register cannot tell an HA Restart from an OTA reboot or a crash:
 * the Restart button adds a tag in retention RAM (diag_set_reboot_tag). */
#include "diag.h"

const char *diag_reason_from_cause(uint32_t cause, uint8_t tag)
{
	if (cause & (1u << 5))
		return "watchdog";
	if (cause & (1u << 4))
		return "lockup";
	if (cause & (1u << 3))
		return tag == DIAG_REBOOT_RESTART ? "restart" : "software";
	if (cause & 7u)
		return "brownout";
	return "power_on";
}
