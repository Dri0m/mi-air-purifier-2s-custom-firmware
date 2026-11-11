/* hal_sht.c - SHT3x on I2C1 @0x44 (fw-hardware-map.md section 3).
 *
 * Differences from stock, on purpose:
 *  - soft reset is the documented 0x30A2 (stock sends 0x302A, a typo);
 *  - both CRC-8s are checked (stock ignores them).
 * Same as stock: periodic mode 0x2126 (1 mps, high repeatability), fetch
 * 0xE000 + repeated start + 6-byte read, stock unit conversion, re-arm on
 * read error.  Stock reads every 20 s; we read every 2 s (the sensor
 * measures at 1 Hz regardless, so this only adds bus traffic).
 */
#include "hal.h"
#include "hal_internal.h"
#include "hal_proto.h"

#define SHT_ADDR 0x44

static uint32_t g_last_ms;
static bool g_armed;
static uint8_t g_fail;

static int sht_cmd(uint16_t cmd)
{
	uint8_t b[2] = { (uint8_t)(cmd >> 8), (uint8_t)cmd };
	return hal_i2c_xfer(I2C1_PORT, SHT_ADDR, b, 2, NULL, 0);
}

static void sht_arm(void)
{
	hal_i2c_lock(I2C1_PORT);
	(void)sht_cmd(0x30A2);           /* soft reset */
	hal_i2c_unlock(I2C1_PORT);
	hal_sleep_ms(2);                 /* tSR <= 1.5 ms */
	hal_i2c_lock(I2C1_PORT);
	g_armed = sht_cmd(0x2126) == HAL_I2C_OK;
	hal_i2c_unlock(I2C1_PORT);
}

void hal_sht_init(void)
{
	sht_arm();
	/* First data is ready ~1 s after arming; allow a read right after. */
	g_last_ms = hal_millis() - HAL_SHT_PERIOD_MS + 1100u;
}

bool hal_sht_poll(int16_t *temp_dC, uint8_t *rh)
{
	static const uint8_t fetch[2] = { 0xE0, 0x00 };
	uint8_t raw[6];
	uint32_t now = hal_millis();
	int err;

	if ((uint32_t)(now - g_last_ms) < HAL_SHT_PERIOD_MS)
		return false;
	g_last_ms = now;

	if (!g_armed) {
		sht_arm();
		return false;
	}

	hal_i2c_lock(I2C1_PORT);
	/* Fetch 0xE000, RESTART, read 6.  Stock SHT_Sensor_ReadData
	 * (1f00cda4) queues the read commands right behind the two command
	 * bytes in one IC_ENABLE window, so the DesignWare block issues a
	 * repeated start, not STOP + START. */
	err = hal_i2c_xfer(I2C1_PORT, SHT_ADDR, fetch, 2, raw, 6);
	hal_i2c_unlock(I2C1_PORT);

	if (err == HAL_I2C_OK && sht_decode(raw, temp_dC, rh)) {
		g_fail = 0;
		return true;
	}
	/* Stock re-sends 0x2126 on a read error; after a few failures do a
	 * full soft reset + re-arm. */
	if (++g_fail >= 3) {
		g_fail = 0;
		g_armed = false;
	} else {
		hal_i2c_lock(I2C1_PORT);
		(void)sht_cmd(0x2126);
		hal_i2c_unlock(I2C1_PORT);
	}
	return false;
}
