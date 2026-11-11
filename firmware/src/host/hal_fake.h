/* hal_fake.h - control/inspection of the host fake HAL (hal_fake.c).
 * Simulated time, fan, sensors, keys, buzzer, LEDs, OLED, EEPROM in RAM. */
#ifndef HAL_FAKE_H
#define HAL_FAKE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define FAKE_EEPROM_SIZE 8192
#define FAKE_FB_MAX (256 * 128 / 8)
#define FAKE_LOG_MAX 64

struct hal_fake {
	uint32_t now;                       /* hal_millis() */

	/* fan */
	uint16_t fan_cmd;                   /* last hal_fan_set_rpm() */
	uint32_t fan_cmds;                  /* number of set calls */
	uint16_t fan_log[FAKE_LOG_MAX];     /* distinct commands, in order */
	uint32_t fan_log_t[FAKE_LOG_MAX];
	int fan_log_n;
	bool fan_stalled;                   /* measured rpm = 0 */

	/* PM sensor: emits a frame every pm_period_ms while active */
	bool pm_active;
	uint32_t pm_active_changes;
	bool pm_broken;                     /* no frames at all */
	uint16_t pm_raw;
	uint32_t pm_period_ms, pm_last;
	uint32_t pm_frames;                 /* frames delivered */

	/* SHT */
	bool sht_ok;
	int16_t temp_dC;
	uint8_t rh;

	uint8_t light;
	bool door;
	uint8_t keys;

	/* buzzer */
	bool buzzer;
	uint32_t buzzer_on_t;
	uint32_t beeps;                     /* rising edges */
	uint32_t beep_ms[FAKE_LOG_MAX];     /* duration of each tone */
	int beep_n;

	/* LEDs / display */
	uint8_t led_r, led_g, led_o;
	uint8_t brightness;
	bool disp_on;
	uint16_t fb_w, fb_h;
	uint8_t fb[FAKE_FB_MAX];
	uint32_t flushes;

	/* EEPROM */
	uint8_t eeprom[FAKE_EEPROM_SIZE];
	uint32_t ee_writes, ee_bytes;
	bool ee_read_fail, ee_write_fail;

	/* RFID */
	bool rfid_present;
	uint8_t rfid_uid[7];
	uint32_t rfid_usage;
	uint32_t rfid_written;
	int rfid_writes;

	uint32_t reboots;
	bool soft_reset;
};

extern struct hal_fake fake;

/* Reset everything except (optionally) the EEPROM contents. */
void fake_reset(bool keep_eeprom);
void fake_advance(uint32_t ms);
void fake_ee_put32(uint16_t addr, uint32_t v);
uint32_t fake_ee_get32(uint16_t addr);
bool fake_fb_px(int x, int y);
void fake_fb_print(void);                 /* ASCII dump to stdout */
#endif
