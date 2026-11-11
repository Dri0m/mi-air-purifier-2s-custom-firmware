/* hal_ext.c - weak defaults for HAL hooks that hal.h does not (yet)
 * declare.  A HAL overrides them with ordinary (strong) definitions. */
#include "hal.h"
#include "pur_int.h"

/* Without a reset cause we can't tell a soft reboot from a power loss;
 * stock comes up off after a power loss (6.1), so default to that. */
__attribute__((weak)) bool hal_reset_was_soft(void)
{
	return false;
}

__attribute__((weak)) int hal_rfid_write_usage(uint32_t used_s)
{
	(void)used_s;
	return -1;
}

/* Stock, unprovisioned: reboot (5.1, FUN_1f013c4c step 3). */
__attribute__((weak)) void hal_wifi_reset(void)
{
	hal_reboot();
}
