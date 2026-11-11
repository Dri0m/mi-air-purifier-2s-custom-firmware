/* hal_rfid.c - MFRC630 filter-tag reader: deliberately NOT used.
 *
 * Tag authentication (NTAG PWD_AUTH with a UID-derived password) is filter
 * DRM and is not implemented.  The reader is held in power-down (PDOWN =
 * GPIO19 high, the stock idle state) and the I2C0 pins are left as GPIO
 * inputs.  Filter hours are counted locally by the purifier logic.
 */
#include "hal.h"
#include "hal_internal.h"

void hal_rfid_init(void)
{
	hal_pin_gpio_out(PIN_RFID_PDOWN, true);  /* power-down asserted */
	hal_pin_gpio_in(PIN_I2C0_SDA);
	hal_pin_gpio_in(PIN_I2C0_SCL);
}

bool hal_rfid_poll(uint8_t uid[7], uint32_t *usage)
{
	(void)uid;
	(void)usage;
	return false;
}

int hal_rfid_write_usage(uint32_t used_s)
{
	(void)used_s;
	return -1;                       /* not available */
}
