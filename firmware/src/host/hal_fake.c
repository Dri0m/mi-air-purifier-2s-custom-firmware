/* hal_fake.c - fake HAL for host unit tests of the portable purifier
 * logic.  Everything is simulated in RAM; time only moves via
 * fake_advance().  See hal_fake.h. */
#include <stdio.h>
#include <string.h>
#include "hal.h"
#include "hal_fake.h"

struct hal_fake fake;

void fake_reset(bool keep_eeprom)
{
	static uint8_t ee[FAKE_EEPROM_SIZE];
	if (keep_eeprom)
		memcpy(ee, fake.eeprom, sizeof ee);
	memset(&fake, 0, sizeof fake);
	if (keep_eeprom)
		memcpy(fake.eeprom, ee, sizeof ee);
	else
		memset(fake.eeprom, 0xff, sizeof fake.eeprom);
	fake.now = 100000;
	fake.pm_raw = 5;
	fake.pm_period_ms = 1000;
	fake.sht_ok = true;
	fake.temp_dC = 215;
	fake.rh = 45;
	fake.light = 60;
	fake.fb_w = 128;
	fake.fb_h = 96;
}

void fake_advance(uint32_t ms) { fake.now += ms; }

void fake_ee_put32(uint16_t a, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		fake.eeprom[a + i] = (uint8_t)(v >> (8 * i));
}

uint32_t fake_ee_get32(uint16_t a)
{
	return (uint32_t)fake.eeprom[a] | (uint32_t)fake.eeprom[a + 1] << 8 |
	       (uint32_t)fake.eeprom[a + 2] << 16 |
	       (uint32_t)fake.eeprom[a + 3] << 24;
}

bool fake_fb_px(int x, int y)
{
	int stride = (fake.fb_w + 7) / 8;
	return fake.fb[y * stride + x / 8] & (0x80 >> (x & 7));
}

void fake_fb_print(void)
{
	for (int y = 0; y < fake.fb_h; y++) {
		for (int x = 0; x < fake.fb_w; x++)
			putchar(fake_fb_px(x, y) ? '#' : '.');
		putchar('\n');
	}
}

/* ---------- hal.h ---------- */

void hal_init(void) {}
uint32_t hal_millis(void) { return fake.now; }

void hal_fan_set_rpm(uint16_t rpm)
{
	fake.fan_cmds++;
	if (rpm != fake.fan_cmd && fake.fan_log_n < FAKE_LOG_MAX) {
		fake.fan_log_t[fake.fan_log_n] = fake.now;
		fake.fan_log[fake.fan_log_n++] = rpm;
	}
	fake.fan_cmd = rpm;
}

uint16_t hal_fan_get_rpm(void)
{
	return fake.fan_stalled ? 0 : fake.fan_cmd;
}

bool hal_pm_poll(uint16_t *pm25_raw)
{
	if (!fake.pm_active || fake.pm_broken)
		return false;
	if (fake.now - fake.pm_last < fake.pm_period_ms)
		return false;
	fake.pm_last = fake.now;
	fake.pm_frames++;
	*pm25_raw = fake.pm_raw;
	return true;
}

void hal_pm_set_active(bool on)
{
	if (on != fake.pm_active)
		fake.pm_active_changes++;
	if (on && !fake.pm_active)
		fake.pm_last = fake.now;   /* first frame ~1 s after wake */
	fake.pm_active = on;
}

bool hal_sht_poll(int16_t *t, uint8_t *rh)
{
	if (!fake.sht_ok)
		return false;
	*t = fake.temp_dC;
	*rh = fake.rh;
	return true;
}

uint16_t hal_light_read(void) { return (uint16_t)(fake.light << 8); } /* fake.light = raw >> 8 */
bool hal_door_open(void) { return fake.door; }
uint8_t hal_keys(void) { return fake.keys; }

void hal_buzzer(bool on)
{
	if (on && !fake.buzzer) {
		fake.beeps++;
		fake.buzzer_on_t = fake.now;
	} else if (!on && fake.buzzer && fake.beep_n < FAKE_LOG_MAX) {
		fake.beep_ms[fake.beep_n++] = fake.now - fake.buzzer_on_t;
	}
	fake.buzzer = on;
}

void hal_leds(uint8_t r, uint8_t g, uint8_t o)
{
	fake.led_r = r;
	fake.led_g = g;
	fake.led_o = o;
}

void hal_display_brightness(uint8_t level) { fake.brightness = level; }

void hal_display_size(uint16_t *w, uint16_t *h)
{
	*w = fake.fb_w;
	*h = fake.fb_h;
}

uint8_t *hal_display_fb(void) { return fake.fb; }
void hal_display_flush(void) { fake.flushes++; }
void hal_display_power(bool on) { fake.disp_on = on; }

int hal_eeprom_read(uint16_t addr, void *buf, size_t len)
{
	if (fake.ee_read_fail || addr + len > FAKE_EEPROM_SIZE)
		return -1;
	memcpy(buf, fake.eeprom + addr, len);
	return 0;
}

int hal_eeprom_write(uint16_t addr, const void *buf, size_t len)
{
	if (fake.ee_write_fail || addr + len > FAKE_EEPROM_SIZE)
		return -1;
	/* a 24C64 page write must not cross a 32-byte page */
	if (len > 32 || (addr / 32) != ((addr + len - 1) / 32))
		return -2;
	memcpy(fake.eeprom + addr, buf, len);
	fake.ee_writes++;
	fake.ee_bytes += (uint32_t)len;
	return 0;
}

bool hal_rfid_poll(uint8_t uid[7], uint32_t *usage)
{
	if (!fake.rfid_present)
		return false;
	memcpy(uid, fake.rfid_uid, 7);
	*usage = fake.rfid_usage;
	return true;
}

void hal_reboot(void) { fake.reboots++; }

/* optional hooks (weak defaults in src/purifier/hal_ext.c) */
bool hal_reset_was_soft(void) { return fake.soft_reset; }

int hal_rfid_write_usage(uint32_t used_s)
{
	fake.rfid_written = used_s;
	fake.rfid_writes++;
	return 0;
}

uint8_t hal_pm_header(void) { return 'B'; }
