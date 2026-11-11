/* hal_eeprom.c - 24C64 on I2C1 @0x50 (fw-hardware-map.md section 8).
 * 2-byte address (top 3 bits ignored by the part, stock masks with 0x1F),
 * 32-byte pages, 6 ms write cycle (stock waits a fixed 6 ms).
 */
#include <string.h>

#include "hal.h"
#include "hal_internal.h"

#define EE_ADDR       0x50
#define EE_SIZE       8192u
#define EE_PAGE       32u
#define EE_WRITE_MS   6u
#define EE_RETRIES    3

void hal_eeprom_init(void)
{
	/* Bus is set up by hal_i2c_init(I2C1_PORT). */
}

int hal_eeprom_read(uint16_t addr, void *buf, size_t len)
{
	uint8_t *p = buf;
	int err = 0;

	if ((size_t)addr + len > EE_SIZE)
		return -1;
	hal_i2c_lock(I2C1_PORT);
	while (len > 0 && err == 0) {
		/* Chunk so each transaction stays short (FIFO/scheduler lock). */
		size_t n = len > EE_PAGE ? EE_PAGE : len;
		uint8_t a[2] = { (uint8_t)((addr >> 8) & 0x1F), (uint8_t)addr };
		int tries = EE_RETRIES;

		do {
			err = hal_i2c_xfer(I2C1_PORT, EE_ADDR, a, 2, p, n);
			if (err == HAL_I2C_NACK)    /* busy in a write cycle */
				hal_sleep_ms(2);
		} while (err != 0 && --tries > 0);
		addr = (uint16_t)(addr + n);
		p += n;
		len -= n;
	}
	hal_i2c_unlock(I2C1_PORT);
	return err ? -1 : 0;
}

int hal_eeprom_write(uint16_t addr, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	uint8_t frame[2 + EE_PAGE];
	int err = 0;

	if ((size_t)addr + len > EE_SIZE)
		return -1;
	hal_i2c_lock(I2C1_PORT);
	while (len > 0 && err == 0) {
		/* Never cross a 32-byte page boundary in one write. */
		size_t room = EE_PAGE - (addr % EE_PAGE);
		size_t n = len > room ? room : len;
		int tries = EE_RETRIES;

		frame[0] = (uint8_t)((addr >> 8) & 0x1F);
		frame[1] = (uint8_t)addr;
		memcpy(&frame[2], p, n);
		do {
			err = hal_i2c_xfer(I2C1_PORT, EE_ADDR, frame, 2 + n,
					   NULL, 0);
			/* Write cycle (or NACK because a previous one is still
			 * running): wait the stock 6 ms either way. */
			hal_sleep_ms(EE_WRITE_MS);
		} while (err != 0 && --tries > 0);
		addr = (uint16_t)(addr + n);
		p += n;
		len -= n;
	}
	hal_i2c_unlock(I2C1_PORT);
	return err ? -1 : 0;
}
