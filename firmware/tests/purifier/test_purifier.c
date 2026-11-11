/* test_purifier.c - host unit tests for src/purifier, driven by the fake
 * HAL with simulated time.  Plain C, no framework.
 * PURIFIER_DUMP=1 prints the rendered main screen. */
#define LRAW(v) ((uint16_t)((v) << 8))
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hal.h"
#include "hal_fake.h"
#include "purifier.h"
#include "settings.h"
#include "pur_int.h"

static int checks, fails;
static const char *cur_test;

#define CHECK(c) do { checks++; if (!(c)) { fails++; \
	printf("  FAIL [%s] %s:%d: %s\n", cur_test, __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); checks++; \
	if (_a != _b) { fails++; printf("  FAIL [%s] %s:%d: %s == %ld, expected %ld\n", \
	cur_test, __FILE__, __LINE__, #a, _a, _b); } } while (0)
#define CHECK_RANGE(a, lo, hi) do { long _a = (long)(a); checks++; \
	if (_a < (long)(lo) || _a > (long)(hi)) { fails++; printf("  FAIL [%s] %s:%d: %s == %ld, expected %ld..%ld\n", \
	cur_test, __FILE__, __LINE__, #a, _a, (long)(lo), (long)(hi)); } } while (0)

static const purifier_state *S;

static void run_step(uint32_t ms, uint32_t step)
{
	for (uint32_t t = 0; t < ms; t += step) {
		fake_advance(step);
		purifier_tick();
	}
}
static void run(uint32_t ms) { run_step(ms, 50); }

static void boot(void)
{
	hal_init();
	purifier_init();
	S = purifier_get();
	run(100);
}

static void fresh(void)
{
	fake_reset(false);
	boot();
}

static void reboot(bool soft)
{
	fake_reset(true);
	fake.soft_reset = soft;
	boot();
}

static void press(uint8_t keys, uint32_t ms)
{
	fake.keys = keys;
	run(ms);
	fake.keys = 0;
	run(200);
}

/* raw PM value that the BM correction maps to (at least) aqi */
static uint16_t raw_for(uint16_t aqi)
{
	for (uint16_t v = 0; v < 1000; v++)
		if (pur_pm_to_aqi(pur_pm_correct_bm(v)) >= aqi)
			return v;
	return 999;
}

static void set_aqi(uint16_t aqi) { fake.pm_raw = raw_for(aqi); }

static uint8_t screen(void) { return ui_current_screen(S, false); }

static const pur_speeds defsp = {
	.silent = 350, .strong = 1700, .high = 1410, .med = 1250,
	.low = 660, .favorite = 1700, .med_l = 1030 };

/* ------------------------------------------------------------------ */

static void test_logic(void)
{
	int bad = 0;
	for (unsigned v = 0; v <= 1100; v++) {
		int ref = v < 2 ? 0 : v >= 1000 ? 999 :
			  (int)(0.38 / (0.62 / v - 0.0003));
		if (pur_pm_correct_bm((uint16_t)v) != ref) {
			if (bad++ < 5)
				printf("  pm correction v=%u int=%u double=%d\n",
				       v, pur_pm_correct_bm((uint16_t)v), ref);
		}
	}
	CHECK_EQ(bad, 0);
	CHECK_EQ(pur_pm_to_aqi(0), 1);
	CHECK_EQ(pur_pm_to_aqi(999), 600);
	static const uint16_t a[] = { 35, 36, 75, 76, 115, 116, 150, 151, 249, 250, 600 };
	static const uint8_t s[] = { 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5 };
	for (unsigned i = 0; i < sizeof a / sizeof a[0]; i++)
		CHECK_EQ(pur_aqi_state(a[i], 75), s[i]);
	CHECK_EQ(pur_bright(256), 0);
	CHECK_EQ(pur_bright(10128), 100);
	CHECK_EQ(pur_bright(20000), 200);
	CHECK_EQ(pur_led_pwm(0), 15);
	CHECK_EQ(pur_led_pwm(30000), 245);
	CHECK_EQ(pur_oled_contrast(15), 9);
	CHECK_EQ(pur_oled_contrast(167), 96);
	CHECK_EQ(pur_oled_contrast(168), 97);
	CHECK_EQ(pur_filter_life(0, 3500), 100);
	CHECK_EQ(pur_filter_life(1750, 3500), 50);
	CHECK_EQ(pur_filter_life(3500, 3500), 0);
	CHECK_EQ(pur_filter_life(5000, 3500), 0);
	CHECK_EQ(pur_filter_used_s(100, 10, 10, 10), 100 + 10 + 15 + 20);
	CHECK_EQ(pur_aqi_report_due(150, 131), 0);
	CHECK_EQ(pur_aqi_report_due(150, 130), 1);
	CHECK_EQ(pur_aqi_report_due(200, 161), 0);
	CHECK_EQ(pur_aqi_report_due(200, 160), 1);
	CHECK_EQ(pur_aqi_report_due(400, 341), 0);
	CHECK_EQ(pur_aqi_report_due(400, 340), 1);
	/* 1f012580: unclamped light value drives the OLED contrast */
	CHECK_EQ(pur_light_pwm(0), 1);
	CHECK_EQ(pur_light_pwm(256), 1);
	CHECK_EQ(pur_light_pwm(300), 1);   /* 0 -> 1 */
	CHECK_EQ(pur_light_pwm(20000), 255);
	CHECK_EQ(pur_light_pwm(10128), 127);
	CHECK_EQ(pur_oled_contrast(pur_light_pwm(0)), 1);
	CHECK_EQ(pur_oled_contrast(pur_light_pwm(20000)), 97);
	CHECK_EQ(pur_led_pwm(256), 15);
	CHECK_EQ(pur_led_pwm(20000), 245);
}

static void test_auto_curve(void)
{
	const pur_speeds *p = &defsp;
	/* zone 0 with 11..20 hysteresis */
	CHECK_EQ(pur_auto_target(5, 1250, p, 75), 350);
	CHECK_EQ(pur_auto_target(10, 660, p, 75), 350);
	CHECK_EQ(pur_auto_target(11, 1250, p, 75), 1250);
	CHECK_EQ(pur_auto_target(20, 350, p, 75), 350);
	CHECK_EQ(pur_auto_target(21, 350, p, 75), 660);
	CHECK_EQ(pur_auto_target(35, 1700, p, 75), 660);
	/* zone 1: inside 43..67 */
	CHECK_EQ(pur_auto_target(43, 350, p, 75), 660);
	CHECK_EQ(pur_auto_target(67, 1700, p, 75), 660);
	CHECK_EQ(pur_auto_target(42, 1250, p, 75), 1030);  /* near: clamp */
	CHECK_EQ(pur_auto_target(42, 350, p, 75), 350);    /* near: keep */
	CHECK_EQ(pur_auto_target(70, 1030, p, 75), 1030);
	/* zone 2: inside 83..107 */
	CHECK_EQ(pur_auto_target(95, 350, p, 75), 1030);
	CHECK_EQ(pur_auto_target(78, 1410, p, 75), 1250);
	CHECK_EQ(pur_auto_target(78, 350, p, 75), 660);
	CHECK_EQ(pur_auto_target(78, 660, p, 75), 660);
	CHECK_EQ(pur_auto_target(110, 1250, p, 75), 1250);
	/* zone 3: inside 123..142 */
	CHECK_EQ(pur_auto_target(130, 350, p, 75), 1250);
	CHECK_EQ(pur_auto_target(118, 1700, p, 75), 1410);
	CHECK_EQ(pur_auto_target(118, 660, p, 75), 1030);
	CHECK_EQ(pur_auto_target(145, 1410, p, 75), 1410);
	/* zone 4: inside 158..242 */
	CHECK_EQ(pur_auto_target(200, 350, p, 75), 1410);
	CHECK_EQ(pur_auto_target(152, 1030, p, 75), 1250);
	CHECK_EQ(pur_auto_target(245, 1700, p, 75), 1700);
	/* zone 5 */
	CHECK_EQ(pur_auto_target(252, 1250, p, 75), 1410);
	CHECK_EQ(pur_auto_target(252, 1700, p, 75), 1700);
	CHECK_EQ(pur_auto_target(258, 1250, p, 75), 1700);
	/* goodh moves the zone 1/2 edge */
	CHECK_EQ(pur_auto_target(80, 350, p, 100), 660);
	/* zone 1 lower bound is Z0 (1f015d76 called with r0 = 0) */
	CHECK_EQ(pur_auto_target(42, 1700, p, 75), 1030);
	CHECK_EQ(pur_auto_target(43, 1700, p, 75), 660);
	CHECK_EQ(pur_auto_target(67, 1700, p, 75), 660);
	CHECK_EQ(pur_auto_target(68, 1700, p, 75), 1030);
	/* which evaluations "assign" (and so restart the 10 s hold) */
	uint16_t r;
	CHECK(pur_auto_eval(5, 350, p, 75, &r) && r == 350);   /* unchanged */
	CHECK(!pur_auto_eval(15, 350, p, 75, &r) && r == 350); /* band */
	CHECK(pur_auto_eval(50, 660, p, 75, &r) && r == 660);  /* inside */
	CHECK(!pur_auto_eval(40, 660, p, 75, &r) && r == 660); /* no clamp */
	CHECK(pur_auto_eval(40, 1250, p, 75, &r) && r == 1030);
	CHECK(!pur_auto_eval(78, 1030, p, 75, &r) && r == 1030);
	CHECK(!pur_auto_eval(252, 1700, p, 75, &r) && r == 1700);
	CHECK(pur_auto_eval(300, 1700, p, 75, &r) && r == 1700);
}

static void test_auto_hold(void)
{
	fresh();
	set_aqi(5);
	CHECK_EQ(purifier_set_power(true), 0);
	CHECK_EQ(S->mode, MODE_AUTO);
	run(3000);
	CHECK_EQ(S->target_rpm, 350);
	set_aqi(100);                      /* zone 2 inside -> med_l */
	run(4000);
	CHECK_EQ(S->pm25, 100);
	CHECK_EQ(S->target_rpm, 350);      /* still within 10 s hold */
	run(5000);
	CHECK_EQ(S->target_rpm, 1030);
	set_aqi(200);                      /* zone 4 inside -> high */
	run(8000);
	CHECK_EQ(S->target_rpm, 1030);     /* hold from the last change */
	run(3000);
	CHECK_EQ(S->target_rpm, 1410);
	run(5000);
	CHECK_EQ(fake.fan_cmd, 1410);      /* ramp done */
	set_aqi(152);                      /* deadband: keep high */
	run(15000);
	CHECK_EQ(S->target_rpm, 1410);
	set_aqi(300);
	run(12000);
	CHECK_EQ(S->target_rpm, 1700);
}

static void test_modes(void)
{
	fresh();
	CHECK(!S->power);
	CHECK_EQ(fake.fan_cmd, 0);
	purifier_take_changes();
	CHECK_EQ(purifier_set_power(true), 0);
	CHECK(S->power);
	CHECK(purifier_take_changes() & PF_POWER);
	CHECK_EQ(purifier_set_mode(MODE_SILENT), 0);
	CHECK(purifier_take_changes() & PF_MODE);
	run(1000);
	CHECK_EQ(S->target_rpm, 350);
	CHECK_EQ(fake.fan_cmd, 350);
	CHECK_EQ(purifier_set_mode(MODE_FAVORITE), 0);
	run(8000);
	CHECK_EQ(fake.fan_cmd, 1700);
	CHECK_EQ(purifier_set_mode((purifier_mode)3), -5001);
	CHECK_EQ(S->mode, MODE_FAVORITE);
	CHECK_EQ(purifier_set_power(false), 0);
	CHECK(!S->power);
	run(600);
	CHECK_EQ(fake.fan_cmd, 0);
	/* set_mode while off powers on */
	CHECK_EQ(purifier_set_mode(MODE_SILENT), 0);
	CHECK(S->power);
	CHECK_EQ(S->mode, MODE_SILENT);
	run(15000);
	CHECK_EQ(fake_ee_get32(0x040), 1);
	CHECK_EQ(fake_ee_get32(0x1e0), 1);
	/* LED on, main shown: 2.5 s mode splash with a beep, then main */
	run(5000);
	uint32_t b = fake.beeps;
	purifier_set_mode(MODE_FAVORITE);
	CHECK_EQ(screen(), SCR_MODE);
	run(2400);
	CHECK_EQ(screen(), SCR_MODE);
	run(200);
	CHECK_EQ(screen(), SCR_MAIN);
	CHECK_EQ(fake.beeps - b, 1);
	/* same mode again: nothing */
	b = fake.beeps;
	purifier_set_mode(MODE_FAVORITE);
	CHECK_EQ(screen(), SCR_MAIN);
	CHECK_EQ(fake.beeps - b, 0);
	/* LED off, display dark: stock wakes to main for 10 s (beep), no
	 * splash (set_mode 1f012e70) */
	purifier_set_led(false);
	run(100);
	CHECK_EQ(screen(), SCR_OFF);
	b = fake.beeps;
	purifier_set_mode(MODE_AUTO);
	CHECK_EQ(S->mode, MODE_AUTO);
	CHECK_EQ(screen(), SCR_MAIN);
	run(500);
	CHECK(fake.disp_on);
	CHECK_EQ(fake.beeps - b, 1);
	run(9300);
	CHECK_EQ(screen(), SCR_MAIN);
	run(300);
	CHECK_EQ(screen(), SCR_OFF);
}

static void test_boot_power_rules(void)
{
	fresh();
	purifier_set_power(true);
	purifier_set_child_lock(true);
	run(15000);
	CHECK_EQ(fake_ee_get32(0x1e0), 1);
	CHECK_EQ(fake_ee_get32(0x1e8), 1);
	/* soft reboot (PMU cause 8): power restored with the power-on beep,
	 * child lock kept (XMAIR_Init 1f0150d8) */
	reboot(true);
	CHECK(S->power);
	CHECK(S->child_lock);
	CHECK_EQ(fake.beep_n, 1);
	CHECK_EQ(fake.beep_ms[0], 100);
	run(15000);
	CHECK_EQ(fake_ee_get32(0x1e8), 1);
	reboot(false);                     /* mains power cycle */
	CHECK(!S->power);
	CHECK(!S->child_lock);
	CHECK_EQ(fake.fan_cmd, 0);
	CHECK_EQ(fake.beeps, 0);
	run(15000);
	CHECK_EQ(fake_ee_get32(0x1e8), 0); /* lock forced off and saved */
	CHECK_EQ(fake_ee_get32(0x1e0), 1); /* stored power untouched */
	reboot(true);
	CHECK(S->power);
	purifier_set_power(false);
	run(15000);
	reboot(true);
	CHECK(!S->power);
	/* country 91 + slave_channel 3 restores even after a power cut */
	purifier_set_power(true);
	run(15000);
	fake_ee_put32(0x278, 3);
	reboot(false);
	CHECK(S->power);
	fake_ee_put32(0x1f0, 86);
	reboot(false);
	CHECK(!S->power);
}

static void test_door(void)
{
	fresh();
	purifier_set_power(true);
	run(3000);
	purifier_take_changes();
	int n0 = fake.beep_n;
	fake.door = true;
	run(1100);
	CHECK(S->door_open);
	CHECK(!S->power);
	CHECK(purifier_take_changes() & (PF_DOOR | PF_POWER));
	CHECK_EQ(screen(), SCR_OFF);       /* standby path: blank ... */
	run(600);
	CHECK_EQ(fake.beep_n, n0 + 1);     /* ... with the long beep */
	CHECK_EQ(fake.beep_ms[fake.beep_n - 1], 500);
	CHECK_EQ(fake.fan_cmd, 0);
	CHECK_EQ(purifier_set_power(true), -6003);
	CHECK_EQ(purifier_set_mode(MODE_SILENT), -6003);
	CHECK(!S->power);
	uint32_t b = fake.beeps;
	press(HAL_KEY_POWER, 200);         /* power key: triple beep, stays off */
	CHECK_EQ(screen(), SCR_DOOR);      /* door screen for 1.5 s */
	run(1000);
	CHECK_EQ(fake.beeps - b, 3);
	CHECK(!S->power);
	CHECK_EQ(screen(), SCR_DOOR);
	run(400);
	CHECK_EQ(screen(), SCR_OFF);
	b = fake.beeps;
	press(HAL_KEY_LED, 200);           /* LED key too */
	CHECK_EQ(screen(), SCR_DOOR);
	run(800);
	CHECK_EQ(fake.beeps - b, 3);
	CHECK(S->led);
	b = fake.beeps;
	fake.door = false;
	run(1500);
	CHECK(!S->door_open);
	CHECK_EQ(fake.beeps - b, 2);       /* double beep on close */
	CHECK_EQ(purifier_set_power(true), 0);
	purifier_set_power(false);
	run(1000);
	int n = fake.beep_n;
	fake.door = true;                  /* opened while off: long beep */
	run(2000);
	CHECK_EQ(fake.beep_n, n + 1);
	CHECK_EQ(fake.beep_ms[fake.beep_n - 1], 500);
}

static void test_fav_levels(void)
{
	fresh();
	purifier_set_mode(MODE_FAVORITE);
	for (uint8_t l = 0; l < FAVORITE_LEVELS; l++) {
		CHECK_EQ(purifier_set_favorite_level(l), 0);
		run(8000);
		CHECK_EQ(S->favorite_level, l);
		CHECK_EQ(S->target_rpm, pur_fav_table[l]);
		CHECK_EQ(fake.fan_cmd, pur_fav_table[l]);
		CHECK_EQ(S->motor_rpm, pur_fav_table[l]);
	}
	CHECK_EQ(purifier_set_favorite_level(15), -5001);
	purifier_set_favorite_level(5);
	run(15000);
	CHECK_EQ(fake_ee_get32(0x218), 1080);
	/* a custom rpm (stock set_spd_favorite) reports level 15, jumps */
	fake_ee_put32(0x218, 1234);
	reboot(false);
	CHECK_EQ(S->favorite_level, 15);
	purifier_set_mode(MODE_FAVORITE);
	run(600);
	CHECK_EQ(fake.fan_cmd, 1234);
	CHECK_EQ(fake.fan_log_n, 1);
}

static void test_ramp(void)
{
	fresh();
	purifier_set_favorite_level(14);
	fake.fan_log_n = 0;
	purifier_set_mode(MODE_FAVORITE);
	run(10000);
	CHECK_EQ(fake.fan_log_n, FAVORITE_LEVELS);
	for (int i = 0; i < FAVORITE_LEVELS && i < fake.fan_log_n; i++) {
		CHECK_EQ(fake.fan_log[i], pur_fav_table[i]);
		if (i)
			CHECK_EQ(fake.fan_log_t[i] - fake.fan_log_t[i - 1], 500);
	}
	fake.fan_log_n = 0;
	purifier_set_favorite_level(10);
	run(3000);
	CHECK_EQ(fake.fan_log_n, 4);
	CHECK_EQ(fake.fan_log[0], 1660);
	CHECK_EQ(fake.fan_log[3], 1510);
	fake.fan_log_n = 0;
	purifier_set_mode(MODE_SILENT);    /* table value: steps down */
	run(6000);
	CHECK_EQ(fake.fan_log_n, 10);
	CHECK_EQ(fake.fan_cmd, 350);
	fake.fan_log_n = 0;
	purifier_set_power(false);         /* stop is immediate */
	CHECK_EQ(fake.fan_cmd, 0);
	CHECK_EQ(fake.fan_log_n, 1);
}

static void test_filter_accounting(void)
{
	fresh();
	set_aqi(5);                        /* state 0: 1x */
	purifier_set_power(true);
	run(5000);
	run_step(86400u * 1000, 1000);
	CHECK_EQ(S->filter_hours, 24);
	CHECK_EQ(S->filter_life, 99);      /* (3500-24)*100/3500 */
	set_aqi(100);                      /* state 2: 1.5x */
	run_step(86400u * 1000, 1000);
	CHECK_RANGE(S->filter_hours, 59, 60);
	set_aqi(300);                      /* state 5: 2x */
	run_step(86400u * 1000, 1000);
	CHECK_RANGE(S->filter_hours, 107, 108);
	uint32_t h = S->filter_hours;
	purifier_set_power(false);         /* standby does not count */
	run_step(86400u * 1000, 1000);
	CHECK_EQ(S->filter_hours, h);
	uint32_t ee = fake_ee_get32(0x244);
	CHECK_RANGE(ee / 3600, h - 1, h);
	CHECK_RANGE(ee, 107u * 3600, 109u * 3600);
	/* persisted every 5 min while on: lose at most ~5 min on power loss */
	purifier_set_power(true);
	run_step(3600u * 1000, 1000);
	uint32_t before = fake_ee_get32(0x244);
	run_step(310u * 1000, 1000);
	CHECK(fake_ee_get32(0x244) > before);
	reboot(false);
	CHECK_RANGE(S->filter_hours, h + 1, h + 3);
	/* rated life 3500 h -> 0 % and the warning when used up */
	fake_ee_put32(0x244, 3499u * 3600);
	reboot(false);
	CHECK_EQ(S->filter_life, 0);
	fake_ee_put32(0x244, 3200u * 3600);
	reboot(false);
	CHECK_EQ(S->filter_life, 8);
	purifier_set_power(true);
	run_step(301u * 1000, 1000);
	CHECK_EQ(screen(), SCR_MAIN);      /* life unchanged: no popup */
}

static void test_filter_reset(void)
{
	fresh();
	fake_ee_put32(0x244, 1000u * 3600);
	reboot(false);
	CHECK_EQ(S->filter_hours, 1000);
	CHECK_EQ(purifier_reset_filter(), 0);
	CHECK_EQ(S->filter_hours, 0);
	CHECK_EQ(S->filter_life, 100);
	run(15000);
	CHECK_EQ(fake_ee_get32(0x244), 0);
	/* LED button held > 5 s */
	fake_ee_put32(0x244, 500u * 3600);
	reboot(false);
	purifier_set_power(true);
	run(1000);
	uint32_t b = fake.beeps;
	fake.keys = HAL_KEY_LED;           /* acts after 6 power ticks */
	run(4900);
	CHECK_EQ(S->filter_hours, 500);
	run(1200);
	CHECK_EQ(S->filter_hours, 0);
	CHECK_EQ(screen(), SCR_RESET_OK);
	fake.keys = 0;
	run(1000);
	CHECK_EQ(fake.beeps - b, 3);       /* triple, no toggle beep */
	CHECK(S->led);                     /* release did not toggle LED */
	run(2700);
	CHECK_EQ(screen(), SCR_RESET_OK);  /* 4 s */
	run(400);
	CHECK_EQ(screen(), SCR_MAIN);
	/* in standby the hold does nothing (stock standby loop) */
	purifier_flush();
	fake_ee_put32(0x244, 500u * 3600);
	reboot(false);
	press(HAL_KEY_LED, 6500);
	CHECK_EQ(S->filter_hours, 500);
	CHECK(!S->power);
	CHECK(S->led);

	/* RFID tag present (not expected on custom HAL): reset refused */
	fake_reset(true);
	fake.rfid_present = true;
	memcpy(fake.rfid_uid, "\x04\x11\x22\x33\x44\x55\x66", 7);
	fake.rfid_usage = 1200u * 3600;
	boot();
	CHECK(S->rfid_present);
	CHECK_EQ(S->filter_hours, 1200);
	CHECK_EQ(purifier_reset_filter(), -5001);
	CHECK_EQ(S->filter_hours, 1200);
	purifier_set_power(true);
	run(1000);
	b = fake.beeps;
	press(HAL_KEY_LED, 6200);
	CHECK_EQ(screen(), SCR_RESET_REFUSED);
	run(1000);
	CHECK_EQ(fake.beeps - b, 3);
	CHECK_EQ(S->filter_hours, 1200);
	run_step(3600u * 1000, 1000);      /* same tag: usage written back */
	CHECK(fake.rfid_writes > 0);
	CHECK_RANGE(fake.rfid_written / 3600, 1200, 1201);
	/* tag gone: hours kept (stock would zero them), reset allowed */
	fake.rfid_present = false;
	run_step(301u * 1000, 1000);
	CHECK(!S->rfid_present);
	CHECK_RANGE(S->filter_hours, 1200, 1202);
	CHECK_EQ(purifier_reset_filter(), 0);
	CHECK_EQ(S->filter_hours, 0);
}

static void test_child_lock(void)
{
	fresh();
	purifier_set_power(true);
	purifier_take_changes();
	CHECK_EQ(purifier_set_child_lock(true), 0);
	CHECK(S->child_lock);
	CHECK(purifier_take_changes() & PF_LOCK);
	CHECK_EQ(screen(), SCR_LOCK_ON);
	run(4000);
	press(HAL_KEY_POWER, 200);
	CHECK_EQ(S->mode, MODE_AUTO);
	CHECK_EQ(screen(), SCR_LOCK_ON);
	press(HAL_KEY_POWER, 3000);        /* hold does not power off */
	CHECK(S->power);
	press(HAL_KEY_LED, 200);
	CHECK(S->led);
	CHECK_EQ(purifier_set_mode(MODE_SILENT), 0);  /* API not locked */
	purifier_set_child_lock(false);
	CHECK_EQ(screen(), SCR_LOCK_OFF);
	press(HAL_KEY_POWER, 200);
	CHECK_EQ(S->mode, MODE_FAVORITE);
}

static void test_eeprom_roundtrip(void)
{
	fresh();
	CHECK(memcmp(fake.eeprom + 0x20, "smartmi_air_mc1", 16) == 0);
	CHECK_EQ(fake_ee_get32(0x1e4), 0x5272);
	purifier_set_mode(MODE_SILENT);
	purifier_set_favorite_level(5);
	purifier_set_led(false);
	purifier_set_buzzer(false);
	run(15000);
	CHECK_EQ(fake_ee_get32(0x040), 1);
	CHECK_EQ(fake_ee_get32(0x218), 1080);
	CHECK_EQ(fake_ee_get32(0x050), 0);
	CHECK_EQ(fake_ee_get32(0x1fe0), 0);
	CHECK_EQ(fake_ee_get32(0x1e0), 1);
	CHECK_EQ(fake_ee_get32(0x298), 3500);
	CHECK_EQ(fake_ee_get32(0x1f0), 91);
	CHECK_EQ(fake_ee_get32(0x200), 350);
	CHECK_EQ(fake_ee_get32(0x214), 1700);
	CHECK(memcmp(fake.eeprom + 0x2a0, "0000000000000000", 16) == 0);
	reboot(true);
	CHECK(S->power);
	CHECK_EQ(S->mode, MODE_SILENT);
	CHECK_EQ(S->favorite_level, 5);
	CHECK(!S->led);
	CHECK(!S->buzzer);
	/* settings.h directly */
	settings_t s;
	CHECK_EQ(settings_load(&s), 0);
	CHECK_EQ(s.mode, 1);
	CHECK_EQ(s.favorite_rpm, 1080);
	CHECK_EQ(s.silent_rpm, 350);
	CHECK(s.power_restore);

	/* write only on change: standby = power element + the last filter
	 * fold, then nothing for an hour */
	run(15000);
	uint32_t w = fake.ee_writes;
	purifier_set_power(false);
	run_step(320u * 1000, 1000);
	CHECK_RANGE(fake.ee_writes - w, 1, 2);
	w = fake.ee_writes;
	run_step(3600u * 1000, 1000);
	CHECK_EQ(fake.ee_writes - w, 0);
	/* rate limit: 20 mode changes in 2 s -> one write of the mode */
	purifier_set_power(true);
	run(15000);
	w = fake.ee_writes;
	for (int i = 0; i < 20; i++) {
		purifier_set_mode(i & 1 ? MODE_AUTO : MODE_FAVORITE);
		run(100);
	}
	run(1000);
	CHECK_EQ(fake.ee_writes - w, 0);   /* debounced */
	run(15000);
	CHECK_EQ(fake.ee_writes - w, 1);
	CHECK_EQ(fake_ee_get32(0x040), 0);

	/* purifier_flush (hal_reboot_hook before an OTA reboot/revert): a
	 * change still inside the debounce window is written immediately */
	purifier_set_mode(MODE_SILENT);
	run(100);
	CHECK_EQ(fake_ee_get32(0x040), 0);
	purifier_flush();
	CHECK_EQ(fake_ee_get32(0x040), 1);
	w = fake.ee_writes;
	purifier_flush();                  /* nothing dirty: no write */
	CHECK_EQ(fake.ee_writes - w, 0);
}

static void test_eeprom_foreign_magic(void)
{
	/* garbled (non-blank) magic: never formatted, settings run on defaults */
	fresh();
	fake_ee_put32(0x244, 123456);          /* stock filter seconds */
	fake_ee_put32(0x020, 0x12345678);      /* corrupt the magic */
	reboot(false);
	int w = fake.ee_writes;
	purifier_set_power(true);
	purifier_set_mode(MODE_FAVORITE);
	run(400000);                           /* > several save periods */
	purifier_flush();
	CHECK_EQ(fake.ee_writes - w, 0);
	CHECK_EQ(fake_ee_get32(0x244), 123456);
	CHECK_EQ(fake_ee_get32(0x020), 0x12345678);
}

static void test_eeprom_corruption(void)
{
	/* blank EEPROM: defaults + magic written */
	fresh();
	CHECK_EQ(fake_ee_get32(0x040), 0);
	CHECK_EQ(fake_ee_get32(0x050), 1);
	CHECK_EQ(fake_ee_get32(0x1fe0), 1);
	CHECK_EQ(fake_ee_get32(0x244), 0);
	CHECK_EQ(fake_ee_get32(0x1f4), 75);
	CHECK(!S->power);
	CHECK(S->led && S->buzzer);
	CHECK_EQ(S->filter_life, 100);

	/* out-of-range fields -> defaults, repaired on the next write */
	fake_ee_put32(0x040, 9);
	fake_ee_put32(0x050, 7);
	fake_ee_put32(0x204, 50);          /* m1_low */
	fake_ee_put32(0x298, 100);         /* f1_hour */
	fake_ee_put32(0x1f4, 200);         /* goodh */
	fake_ee_put32(0x244, 0xffffffff);
	reboot(false);
	CHECK_EQ(S->mode, MODE_AUTO);
	CHECK(S->led);
	CHECK_EQ(S->filter_hours, 0);
	set_aqi(50);                       /* zone 1 -> m1_low default */
	purifier_set_power(true);
	run(15000);
	CHECK_EQ(S->target_rpm, 660);
	CHECK_EQ(fake_ee_get32(0x204), 660);
	CHECK_EQ(fake_ee_get32(0x298), 3500);
	CHECK_EQ(fake_ee_get32(0x1f4), 75);
	CHECK_EQ(fake_ee_get32(0x040), 0);

	/* layout version mismatch -> speed table defaults */
	fake_ee_put32(0x1e4, 0x1234);
	fake_ee_put32(0x200, 400);
	fake_ee_put32(0x218, 1500);
	reboot(false);
	CHECK_EQ(S->favorite_level, 14);
	purifier_set_mode(MODE_SILENT);
	run(15000);
	CHECK_EQ(S->target_rpm, 350);
	CHECK_EQ(fake_ee_get32(0x1e4), 0x5272);
	CHECK_EQ(fake_ee_get32(0x200), 350);

	/* data-reset flag (DataResetSet) -> AIR_Var_WriteAll at boot: all
	 * defaults written at once, only slave_channel and debug kept */
	fake_ee_put32(0x254, 1);
	fake_ee_put32(0x040, 6);
	fake_ee_put32(0x1f0, 86);          /* country -> 91 */
	fake_ee_put32(0x1ec, 7);           /* channel -> 0 */
	fake_ee_put32(0x240, 2);           /* hw_ver -> 0 */
	fake_ee_put32(0x244, 777);         /* filter used -> 0 */
	fake_ee_put32(0x24c, 900);         /* aqi sum -> 0 */
	fake_ee_put32(0x278, 3);           /* slave_channel kept */
	fake_ee_put32(0x054, 1);           /* debug kept */
	memcpy(fake.eeprom + 0x2a0, "abcdefghijklmnop", 16);
	reboot(false);
	CHECK_EQ(S->mode, MODE_AUTO);
	CHECK_EQ(fake_ee_get32(0x254), 0); /* written during init */
	CHECK_EQ(fake_ee_get32(0x040), 0);
	CHECK_EQ(fake_ee_get32(0x1f0), 91);
	CHECK_EQ(fake_ee_get32(0x1ec), 0);
	CHECK_EQ(fake_ee_get32(0x240), 0);
	CHECK_EQ(fake_ee_get32(0x244), 0);
	CHECK_EQ(fake_ee_get32(0x24c), 0);
	CHECK_EQ(fake_ee_get32(0x278), 3);
	CHECK_EQ(fake_ee_get32(0x054), 1);
	CHECK(memcmp(fake.eeprom + 0x2a0, "0000000000000000", 16) == 0);
	CHECK(memcmp(fake.eeprom + 0x20, "smartmi_air_mc1", 16) == 0);
	fake_ee_put32(0x278, 4);
	/* slave_channel outside 3..4 -> 4 */
	fake_ee_put32(0x278, 9);
	reboot(false);
	purifier_set_power(true);
	run(15000);
	CHECK_EQ(fake_ee_get32(0x278), 4);

	/* read failure: defaults in RAM, nothing written */
	fake_ee_put32(0x050, 0);
	fake_reset(true);
	fake.ee_read_fail = true;
	boot();
	CHECK(S->led);
	purifier_set_buzzer(false);
	run(15000);
	CHECK_EQ(fake.ee_writes, 0);
	fake.ee_read_fail = false;         /* bus recovers: retry + write */
	purifier_set_buzzer(true);
	purifier_set_buzzer(false);
	run(15000);
	CHECK_EQ(fake_ee_get32(0x1fe0), 0);
	CHECK_EQ(fake_ee_get32(0x050), 1); /* our RAM setting won */

	/* writes never cross a 32-byte page (fake rejects those) */
	fake_reset(false);
	boot();
	CHECK(memcmp(fake.eeprom + 0x20, "smartmi_air_mc1", 16) == 0);
}

static void test_pm_sleep_wake(void)
{
	/* boot: idle count starts at 150 (1f0150d8), so the sensor sleeps
	 * after the first read unless the display is lit */
	fresh();
	CHECK_EQ(S->pm25, 20);             /* stock boot value */
	set_aqi(40);
	run(2100);
	CHECK_EQ(S->pm25, 40);
	CHECK(!fake.pm_active);
	/* standby: wake every 600 s, 14 s warm-up, two reads, sleep */
	run_step(590u * 1000, 1000);
	CHECK(!fake.pm_active);
	run_step(12u * 1000, 1000);
	CHECK(fake.pm_active);
	set_aqi(90);
	run_step(10u * 1000, 1000);
	CHECK(fake.pm_active);             /* warming up */
	CHECK_EQ(S->pm25, 40);
	uint32_t f = fake.pm_frames;
	run_step(6u * 1000, 1000);
	CHECK_EQ(S->pm25, 90);
	CHECK(!fake.pm_active);
	CHECK(fake.pm_frames > f);
	/* powered on with the LED off: power-on nudges the sensor awake at
	 * the next tick; afterwards 120 s asleep / 14 s warm-up / 2 reads */
	purifier_set_led(false);
	purifier_set_power(true);
	run(2100);
	CHECK(fake.pm_active);
	set_aqi(60);
	run_step(13u * 1000, 50);
	CHECK_EQ(S->pm25, 90);
	run_step(1u * 1000, 50);
	CHECK_EQ(S->pm25, 60);             /* first read at 14 s */
	CHECK(fake.pm_active);
	set_aqi(70);
	run_step(2u * 1000, 50);
	CHECK_EQ(S->pm25, 70);             /* second read at 16 s */
	CHECK(!fake.pm_active);
	run_step(118u * 1000, 1000);
	CHECK(!fake.pm_active);
	run_step(3u * 1000, 1000);
	CHECK(fake.pm_active);             /* woke after 120 s */
	run_step(20u * 1000, 1000);
	CHECK(!fake.pm_active);
	/* LED on (bright > 0, powered): woken at once and kept awake */
	purifier_set_led(true);
	run(2500);
	CHECK(fake.pm_active);
	run_step(600u * 1000, 1000);
	CHECK(fake.pm_active);
	/* ... unless it is dark */
	fake.light = 0;
	run_step(3u * 1000, 50);
	CHECK_EQ(S->bright, 0);
	CHECK(!fake.pm_active);
	/* light returns: the 200 ms light update wakes it again */
	fake.light = 60;
	run(2500);
	CHECK(fake.pm_active);
	/* standby with the LED on: not lit, sleeps after the next read */
	purifier_set_power(false);
	run(20000);                        /* finish the warm-up, 2 reads */
	CHECK(!fake.pm_active);
	/* broken sensor: > 5 failures -> placeholder 23 */
	purifier_set_power(true);
	fake.pm_broken = true;
	run_step(30u * 1000, 50);
	CHECK_EQ(S->pm25, 23);
}

static void test_buttons(void)
{
	fresh();
	press(HAL_KEY_POWER, 200);         /* standby -> on, single beep */
	CHECK(S->power);
	run(600);
	CHECK_EQ(fake.beep_n, 1);
	CHECK_EQ(fake.beep_ms[fake.beep_n - 1], 100);
	run(3000);
	press(HAL_KEY_POWER, 200);
	CHECK_EQ(S->mode, MODE_SILENT);
	CHECK_EQ(screen(), SCR_MODE);
	run(300);
	CHECK_EQ(fake.beep_ms[fake.beep_n - 1], 100);
	press(HAL_KEY_POWER, 200);         /* splash showing: cycles again */
	CHECK_EQ(S->mode, MODE_FAVORITE);
	press(HAL_KEY_POWER, 200);
	CHECK_EQ(S->mode, MODE_AUTO);
	/* hold: standby after 2 power ticks (1-2 s), while still held,
	 * with the long beep */
	fake.keys = HAL_KEY_POWER;
	run(900);
	CHECK(S->power);
	run(1200);
	CHECK(!S->power);
	fake.keys = 0;
	run(600);
	CHECK(!S->power);                  /* release did not power on */
	CHECK_EQ(fake.beep_ms[fake.beep_n - 1], 500);
	/* LED short in standby: nothing */
	press(HAL_KEY_LED, 200);
	CHECK(S->led);
	/* a long press in standby powers on at release */
	press(HAL_KEY_POWER, 3000);
	CHECK(S->power);
	/* LED short toggles, with a beep */
	uint32_t b = fake.beeps;
	press(HAL_KEY_LED, 200);
	CHECK(!S->led);
	CHECK_EQ(screen(), SCR_OFF);
	CHECK_EQ(fake.beeps - b, 1);
	run(300);
	CHECK(!fake.disp_on);
	/* LED off: first power press only wakes (10 s), second cycles */
	b = fake.beeps;
	press(HAL_KEY_POWER, 200);
	CHECK_EQ(S->mode, MODE_AUTO);
	CHECK_EQ(screen(), SCR_MAIN);
	CHECK_EQ(fake.beeps - b, 0);
	run(100);
	CHECK(fake.disp_on);
	press(HAL_KEY_POWER, 200);
	CHECK_EQ(S->mode, MODE_SILENT);
	CHECK_EQ(screen(), SCR_MODE);
	run(2500);
	CHECK_EQ(screen(), SCR_MAIN);      /* splash, then main ... */
	run(9300);
	CHECK_EQ(screen(), SCR_MAIN);
	run(700);
	CHECK_EQ(screen(), SCR_OFF);       /* ... for 10 s */
	press(HAL_KEY_LED, 200);
	CHECK(S->led);
	/* child lock: keys refused while on (lock screen 4 s) ... */
	purifier_set_child_lock(true);
	run(4500);
	press(HAL_KEY_LED, 200);
	CHECK(S->led);
	CHECK_EQ(screen(), SCR_LOCK_ON);
	/* ... and in standby the power key does not power on (1.5 s) */
	purifier_set_power(false);
	run(1000);
	press(HAL_KEY_POWER, 200);
	CHECK(!S->power);
	CHECK_EQ(screen(), SCR_LOCK_ON);
	run(1500);
	CHECK_EQ(screen(), SCR_OFF);
	purifier_set_power(true);
	run(2000);
	/* both keys: armed after 5 ticks (triple beep), Wi-Fi reset when
	 * released (seen at the next power tick) */
	b = fake.beeps;
	fake.keys = HAL_KEY_POWER | HAL_KEY_LED;
	run(3900);
	CHECK_EQ(fake.beeps, b);
	run(1500);
	run(600);
	CHECK_EQ(fake.beeps - b, 3);
	CHECK_EQ(fake.reboots, 0);
	fake.keys = 0;
	run(1200);
	CHECK_EQ(fake.reboots, 1);         /* weak hal_wifi_reset -> reboot */
	CHECK(!S->child_lock);
	CHECK(S->power);                   /* no mode/power side effects */
	CHECK_EQ(S->mode, MODE_SILENT);
	CHECK_EQ(fake_ee_get32(0x254), 0);
	/* held 15 ticks: fires by itself (still held), sets DataResetSet;
	 * the defaults are written at the next boot */
	run(15000);
	CHECK_EQ(fake_ee_get32(0x040), 1);
	fake.keys = HAL_KEY_POWER | HAL_KEY_LED;
	run(14000);
	CHECK_EQ(fake.reboots, 1);
	run(2000);
	CHECK_EQ(fake.reboots, 2);
	fake.keys = 0;
	run(2000);
	CHECK_EQ(fake.reboots, 2);
	CHECK_EQ(fake_ee_get32(0x254), 1);
	purifier_flush();                  /* hal_reboot_hook must not undo it */
	reboot(false);
	CHECK_EQ(fake_ee_get32(0x254), 0);
	CHECK_EQ(fake_ee_get32(0x040), 0);
	CHECK_EQ(fake_ee_get32(0x1e0), 0);
	CHECK_EQ(S->mode, MODE_AUTO);
}

static void test_stall(void)
{
	/* 5 s FG windows while the speed is settled: the first only arms
	 * the grace flag, a later one with 0 rpm is the stall (1f0128d0) */
	fresh();
	purifier_set_mode(MODE_SILENT);    /* 350 rpm: one ramp step */
	fake.fan_stalled = true;
	run(9000);
	CHECK(S->power);
	run(1700);
	CHECK(!S->power);                  /* 10 s after settling */
	CHECK(S->motor_fault);             /* B4: HA "Motor fault" */
	CHECK(purifier_take_changes() & PF_MOTOR_FAULT);
	CHECK_EQ(fake.fan_cmd, 0);
	CHECK_EQ(ui_current_screen(S, true), SCR_FAULT);
	run(600);
	CHECK_EQ(fake.beep_ms[fake.beep_n - 1], 500);
	CHECK(fake.disp_on);
	run(60000);
	CHECK(fake.disp_on);               /* fault screen stays */
	fake.fan_stalled = false;
	press(HAL_KEY_POWER, 200);
	CHECK(S->power);
	CHECK(!S->motor_fault);            /* cleared by the power-on */
	CHECK(purifier_take_changes() & PF_MOTOR_FAULT);
	run(1000);
	CHECK(fake.fan_cmd > 0);
	/* stall while running: caught within one window */
	run(20000);
	fake.fan_stalled = true;
	run(5100);
	CHECK(!S->power);
	CHECK(S->motor_fault);
	purifier_set_power(false);         /* standby 4 -> 1 clears it */
	CHECK(!S->motor_fault);
	purifier_set_power(true);
	/* no stall is declared while the fan is still ramping */
	fake.fan_stalled = false;
	purifier_set_favorite_level(14);
	purifier_set_mode(MODE_FAVORITE);
	fake.fan_stalled = true;
	run(7000);                         /* 14 steps x 0.5 s */
	CHECK(S->power);
}

static void test_light_leds(void)
{
	fresh();
	fake.light = 60;                   /* raw 15360 */
	set_aqi(5);
	purifier_set_power(true);
	run(5000);
	uint8_t pwm = pur_led_pwm(LRAW(60));
	uint8_t c60 = pur_oled_contrast(pur_light_pwm(LRAW(60)));
	CHECK_EQ(S->bright, pur_bright(LRAW(60)));
	CHECK_EQ(fake.led_g, pwm);
	CHECK_EQ(fake.led_r, 0);
	CHECK_EQ(fake.led_o, 0);
	CHECK_EQ(fake.brightness, c60);
	set_aqi(100);
	run(3000);
	CHECK_EQ(fake.led_o, pwm);
	CHECK_EQ(fake.led_g, 0);
	set_aqi(300);
	run(3000);
	CHECK_EQ(fake.led_r, pwm);
	/* dim: contrast moves in steps of 16, from the unclamped value */
	fake.light = 2;
	run(250);
	CHECK_EQ(fake.brightness, c60 - 16);
	run(2000);                         /* stops within the 8 deadband */
	uint8_t c2 = pur_oled_contrast(pur_light_pwm(LRAW(2)));
	CHECK_EQ(c2, 2);
	CHECK_RANGE(fake.brightness, c2, c2 + 8);
	CHECK_EQ(fake.led_r, 15);          /* the LED PWM is clamped */
	purifier_set_led(false);
	run(200);
	CHECK_EQ(fake.led_r + fake.led_g + fake.led_o, 0);
	CHECK(!fake.disp_on);
	purifier_set_led(true);
	purifier_set_power(false);
	run(200);
	CHECK_EQ(fake.led_r + fake.led_g + fake.led_o, 0);
	/* bright: frozen in standby (light timer only while on) */
	fake.light = 70;
	run(5000);
	CHECK_EQ(S->bright, pur_bright(LRAW(2)));
	/* push: |delta| > 19 and >= 300 samples since the last push */
	fresh();
	purifier_set_power(true);
	purifier_take_changes();
	fake.light = 70;
	run(10000);
	CHECK(!(purifier_take_changes() & PF_BRIGHT));
	run(51000);
	CHECK(purifier_take_changes() & PF_BRIGHT);
	fake.light = 20;
	run(10000);
	CHECK(!(purifier_take_changes() & PF_BRIGHT));  /* counter restarted */
	run(51000);
	CHECK(purifier_take_changes() & PF_BRIGHT);
}

static int lit_pixels(void)
{
	int n = 0;
	for (int y = 0; y < fake.fb_h; y++)
		for (int x = 0; x < fake.fb_w; x++)
			n += fake_fb_px(x, y);
	return n;
}

static void test_display(void)
{
	fresh();
	fake_ee_put32(0x244, 3300u * 3600);   /* 5 % life: warning row */
	reboot(false);
	CHECK(!fake.disp_on);
	fake.temp_dC = -34;
	set_aqi(123);
	purifier_set_power(true);
	purifier_set_child_lock(true);
	run(5000);
	int bottom = 0;
	for (int y = 84; y < 96; y++)
		for (int x = 0; x < 128; x++)
			bottom += fake_fb_px(x, y);
	CHECK(bottom > 30);
	CHECK(fake.disp_on);
	CHECK_EQ(screen(), SCR_MAIN);
	CHECK(fake.flushes > 0);
	int n = lit_pixels();
	CHECK(n > 500);
	if (getenv("PURIFIER_DUMP"))
		fake_fb_print();
	uint32_t f = fake.flushes;
	run(3000);
	CHECK_EQ(fake.flushes, f);         /* unchanged picture: no flush */
	set_aqi(300);
	run(3000);
	CHECK(fake.flushes > f);
	purifier_set_child_lock(false);
	fake.door = true;
	run(1100);
	CHECK_EQ(screen(), SCR_OFF);
	fake.keys = HAL_KEY_POWER;
	run(200);
	fake.keys = 0;
	run(100);
	CHECK_EQ(screen(), SCR_DOOR);
	run(300);
	CHECK(lit_pixels() > 100 && lit_pixels() != n);
	if (getenv("PURIFIER_DUMP"))
		fake_fb_print();
	run(4000);
	CHECK(!fake.disp_on);
	/* other panel size: nothing out of bounds (ASan) */
	fake_reset(false);
	fake.fb_w = 128;
	fake.fb_h = 64;
	boot();
	purifier_set_power(true);
	run(1000);
	CHECK(lit_pixels() > 100);
}

static void test_time_wrap(void)
{
	fake_reset(false);
	fake.now = 0xffffffffu - 3000;
	boot();
	purifier_set_favorite_level(14);
	purifier_set_mode(MODE_FAVORITE);
	run(10000);
	CHECK(fake.now < 10000);
	CHECK_EQ(fake.fan_cmd, 1700);
	set_aqi(200);
	purifier_set_mode(MODE_AUTO);
	run(15000);
	CHECK_EQ(S->target_rpm, 1410);
}

static void test_flags(void)
{
	fresh();
	fake_reset(false);
	hal_init();
	purifier_init();
	S = purifier_get();
	uint32_t c = purifier_take_changes();
	CHECK_EQ(c, (PF_LAST << 1) - 1);
	CHECK_EQ(purifier_take_changes(), 0);
	purifier_set_led(false);
	purifier_set_buzzer(false);
	c = purifier_take_changes();
	CHECK_EQ(c, PF_LED | PF_BUZZER);
	purifier_set_led(false);
	CHECK_EQ(purifier_take_changes(), 0);
	fake.rh = 50;
	run(21000);
	CHECK(purifier_take_changes() & PF_HUM);
	CHECK_EQ(S->humidity, 50);
	fake.temp_dC = 219;                /* |delta| 4: no temp push */
	run(20000);
	CHECK(!(purifier_take_changes() & PF_TEMP));
	CHECK_EQ(S->temp_dC, 219);
	fake.temp_dC = 220;
	run(20000);
	CHECK(purifier_take_changes() & PF_TEMP);
}

static void test_screen_times(void)
{
	fresh();
	/* child lock RPC while off: beep only (standby loop draws nothing) */
	uint32_t b = fake.beeps;
	purifier_set_child_lock(true);
	CHECK_EQ(screen(), SCR_OFF);
	run(300);
	CHECK_EQ(fake.beeps - b, 1);
	purifier_set_child_lock(false);
	purifier_set_power(true);
	run(3000);
	/* lock on/off screens: 4 s (0xfa0) */
	purifier_set_child_lock(true);
	CHECK_EQ(screen(), SCR_LOCK_ON);
	run(3900);
	CHECK_EQ(screen(), SCR_LOCK_ON);
	run(200);
	CHECK_EQ(screen(), SCR_MAIN);
	purifier_set_child_lock(false);
	CHECK_EQ(screen(), SCR_LOCK_OFF);
	run(4100);
	CHECK_EQ(screen(), SCR_MAIN);
	/* set_led beeps; LED on while on wakes the main screen */
	b = fake.beeps;
	purifier_set_led(false);
	run(300);
	CHECK_EQ(fake.beeps - b, 1);
	CHECK(!fake.disp_on);
	purifier_set_led(true);
	run(300);
	CHECK(fake.disp_on);
	CHECK_EQ(screen(), SCR_MAIN);
	/* LED off and the lock-off screen showing: the power key wakes to
	 * main (drawn screen not main/splash) instead of cycling the mode */
	purifier_set_led(false);
	run(500);
	purifier_set_child_lock(true);
	purifier_set_child_lock(false);
	CHECK_EQ(screen(), SCR_LOCK_OFF);
	purifier_mode m = S->mode;
	press(HAL_KEY_POWER, 200);
	CHECK_EQ(S->mode, m);
	CHECK_EQ(screen(), SCR_MAIN);
	run(9500);
	CHECK_EQ(screen(), SCR_MAIN);
	run(600);
	CHECK_EQ(screen(), SCR_OFF);
}

static void test_filter_popups(void)
{
	/* 300 s of powered-on time: life changed into 5..10 with the LED on
	 * -> popup 0x16 for 10 s, red LED only */
	fresh();
	fake_ee_put32(0x244, 3115u * 3600 + 3500);
	reboot(false);
	set_aqi(5);
	CHECK_EQ(S->filter_life, 11);
	run_step(200u * 1000, 1000);       /* standby time does not count */
	purifier_set_power(true);
	run_step(295u * 1000, 1000);
	CHECK_EQ(S->filter_life, 10);
	CHECK_EQ(screen(), SCR_MAIN);
	run_step(6u * 1000, 1000);
	CHECK_EQ(screen(), SCR_FILTER_LOW);
	run(100);
	CHECK(fake.led_r > 0 && fake.led_g == 0 && fake.led_o == 0);
	run(9000);
	CHECK_EQ(screen(), SCR_MAIN);
	run(200);
	CHECK(fake.led_g > 0 && fake.led_r == 0);
	run_step(300u * 1000, 1000);       /* life unchanged: no popup */
	CHECK_EQ(screen(), SCR_MAIN);
	/* the power key on the popup goes back to main, no mode change */
	purifier_mode m = S->mode;
	fake_ee_put32(0x244, 3115u * 3600 + 3500);
	reboot(false);
	purifier_set_power(true);
	run_step(301u * 1000, 1000);
	CHECK_EQ(screen(), SCR_FILTER_LOW);
	press(HAL_KEY_POWER, 200);
	CHECK_EQ(S->mode, m);
	CHECK_EQ(screen(), SCR_MAIN);

	/* expired: "replace" for 60 s, again 180 s after the last change */
	fake_ee_put32(0x244, 3500u * 3600);
	reboot(false);
	purifier_set_power(true);
	run_step(170u * 1000, 1000);
	CHECK_EQ(screen(), SCR_MAIN);
	run_step(15u * 1000, 1000);
	CHECK_EQ(screen(), SCR_FILTER_LOW);
	run_step(55u * 1000, 1000);
	CHECK_EQ(screen(), SCR_FILTER_LOW);
	run_step(10u * 1000, 1000);
	CHECK_EQ(screen(), SCR_MAIN);
	run_step(165u * 1000, 1000);       /* back on main at +240 s */
	CHECK_EQ(screen(), SCR_MAIN);
	run_step(10u * 1000, 1000);
	CHECK_EQ(screen(), SCR_FILTER_LOW);
	purifier_set_led(false);           /* LED off: no popups */
	run_step(600u * 1000, 1000);
	CHECK_EQ(screen(), SCR_OFF);

	/* life 0..4: every 8 h of uptime since the last popup, 10 s */
	fake_ee_put32(0x244, 3395u * 3600);
	reboot(false);
	set_aqi(5);
	CHECK_EQ(S->filter_life, 3);
	purifier_set_power(true);
	purifier_set_led(true);            /* LED off was persisted above */
	run_step(28790u * 1000, 1000);
	CHECK_EQ(S->filter_life, 2);
	CHECK_EQ(screen(), SCR_MAIN);
	run_step(15u * 1000, 1000);
	CHECK_EQ(screen(), SCR_FILTER_LOW);
	run_step(10u * 1000, 1000);
	CHECK_EQ(screen(), SCR_MAIN);
}

static void test_aqi_force_push(void)
{
	fresh();
	set_aqi(40);
	run(5000);
	purifier_take_changes();
	run_step(1700u * 1000, 1000);      /* sensor mostly asleep, same value */
	CHECK(!(purifier_take_changes() & PF_PM));
	run_step(110u * 1000, 1000);
	CHECK(purifier_take_changes() & PF_PM);
}

static void test_auto_hold_mode(void)
{
	/* the runmode handler sets the auto target from aqi_state without
	 * restarting the hold; the algorithm then runs at the next read */
	fresh();
	set_aqi(5);
	purifier_set_mode(MODE_SILENT);
	run(30000);
	set_aqi(40);                       /* zone 1 near Z0: no clamp */
	run(3000);
	purifier_set_mode(MODE_AUTO);
	CHECK_EQ(S->target_rpm, 660);      /* aqi_state 1 -> idx5 */
	set_aqi(200);
	run(4100);                         /* frame of the next 2 s window */
	CHECK_EQ(S->target_rpm, 1410);     /* no 10 s wait */
}

/* B2: placeholders (boot 20, fault 23) are flagged pm25_unknown for HA;
 * the panel value stays the stock placeholder; recovery pushes at once */
static void test_pm_fault_ha(void)
{
	fresh();
	CHECK_EQ(S->pm25, 20);
	CHECK(S->pm25_unknown);            /* no reading yet */
	CHECK(!S->pm_fault);
	purifier_take_changes();
	set_aqi(40);
	run(2100);
	CHECK_EQ(S->pm25, 40);
	CHECK(!S->pm25_unknown);
	CHECK(purifier_take_changes() & PF_PM_LIVE);
	/* keep the sensor awake (lit): wake, 14 s warm-up, then every 2 s */
	purifier_set_power(true);
	purifier_set_led(true);
	run(20000);
	CHECK(fake.pm_active);
	purifier_take_changes();
	fake.pm_broken = true;
	run_step(10u * 1000, 50);          /* 5 failed reads: not yet */
	CHECK(!S->pm_fault);
	CHECK(!S->pm25_unknown);
	CHECK(!(purifier_take_changes() & (PF_PM_LIVE | PF_PM_FAULT)));
	run_step(2u * 1000, 50);           /* the 6th */
	CHECK(S->pm_fault);
	CHECK(S->pm25_unknown);
	CHECK_EQ(S->pm25, 23);             /* panel: stock placeholder 0x17 */
	uint32_t c = purifier_take_changes();
	CHECK(c & PF_PM_LIVE);
	CHECK(c & PF_PM_FAULT);
	CHECK_EQ(ui_current_screen(S, false), SCR_MAIN);
	run_step(20u * 1000, 50);          /* stays faulted, flagged once */
	CHECK(S->pm_fault);
	CHECK(!(purifier_take_changes() & (PF_PM_LIVE | PF_PM_FAULT)));
	/* recovery with the value equal to the last stock report (40): stock's
	 * limits would not push, HA must get it at the next read */
	fake.pm_broken = false;
	run_step(2u * 1000, 50);
	CHECK(!S->pm_fault);
	CHECK(!S->pm25_unknown);
	CHECK_EQ(S->pm25, 40);
	c = purifier_take_changes();
	CHECK(c & PF_PM_LIVE);
	CHECK(c & PF_PM_FAULT);
	CHECK(!(c & PF_PM));               /* stock report limit unchanged */
}

/* B2: an app-loop stall makes every() replay sensor ticks; replayed ticks
 * had no polling window and must not count as failed reads */
static void test_pm_stall_replay(void)
{
	fresh();
	purifier_set_power(true);
	purifier_set_led(true);
	set_aqi(40);
	run(20000);                        /* past the 14 s warm-up */
	CHECK(fake.pm_active);
	CHECK_EQ(S->pm25, 40);
	purifier_take_changes();
	for (int i = 0; i < 5; i++) {      /* five 30 s stalls (14 replays each) */
		run_step(30u * 1000, 30u * 1000);
		run(3000);                 /* normal loop in between */
	}
	CHECK(!S->pm_fault);
	CHECK(!S->pm25_unknown);
	CHECK_EQ(S->pm25, 40);
	CHECK(!(purifier_take_changes() & PF_PM_FAULT));
	/* back-to-back stalls with no frame in any live window still fault
	 * (the first one still uses a frame buffered before the stall) */
	fake.pm_broken = true;
	for (int i = 0; i < 6; i++)
		run_step(30u * 1000, 30u * 1000);
	CHECK(!S->pm_fault);               /* 5 failed reads */
	run_step(30u * 1000, 30u * 1000);
	CHECK(S->pm_fault);
	CHECK(S->pm25_unknown);
}

/* B3: HA's flag fires on every PM2.5 change; PF_PM keeps stock's limits */
static void test_ramp_smooth(void)
{
	/* equal / stop / standstill */
	CHECK_EQ(pur_ramp_step_smooth(792, 792), 792);
	CHECK_EQ(pur_ramp_step_smooth(1236, 0), 0);
	CHECK_EQ(pur_ramp_step_smooth(0, 1545), 350);
	CHECK_EQ(pur_ramp_step_smooth(0, 330), 330);
	/* up through the table, then exact */
	CHECK_EQ(pur_ramp_step_smooth(350, 1545), 660);
	CHECK_EQ(pur_ramp_step_smooth(1510, 1545), 1545);
	CHECK_EQ(pur_ramp_step_smooth(400, 500), 500);
	/* down */
	CHECK_EQ(pur_ramp_step_smooth(1545, 525), 1510);
	CHECK_EQ(pur_ramp_step_smooth(660, 525), 525);
	/* table to table behaves like stock's ramp */
	for (int a = 0; a < FAVORITE_LEVELS; a++)
		for (int b = 0; b < FAVORITE_LEVELS; b++)
			CHECK_EQ(pur_ramp_step_smooth(pur_fav_table[a], pur_fav_table[b]),
				 pur_ramp_step(pur_fav_table[a], pur_fav_table[b]));
	/* converges, monotonic, at most 16 steps from 0 to anything */
	int bad = 0;
	for (unsigned t = 300; t <= 2100; t += 7) {
		uint16_t c = 0, prev = 0;
		int n = 0;
		while (c != t && n < 20) {
			c = pur_ramp_step_smooth(c, (uint16_t)t);
			if (c < prev || c > t) bad++;
			prev = c; n++;
		}
		if (c != t) bad++;
	}
	CHECK_EQ(bad, 0);
}

static void test_pm_live_flag(void)
{
	fresh();
	purifier_set_power(true);
	purifier_set_led(true);
	set_aqi(40);
	run(20000);                        /* past the 14 s warm-up */
	purifier_take_changes();
	set_aqi(43);
	run(2100);
	CHECK_RANGE(S->pm25, 43, 45);
	uint32_t c = purifier_take_changes();
	CHECK(c & PF_PM_LIVE);
	CHECK(!(c & PF_PM));               /* |delta| < 20: no stock report */
	run(4100);                         /* same value: no flag */
	CHECK(!(purifier_take_changes() & PF_PM_LIVE));
	set_aqi(44);                       /* +-1 jitter: held back ... */
	run(4100);
	CHECK(!(purifier_take_changes() & PF_PM_LIVE));
	run(30000);                        /* ... then sent within 30 s */
	CHECK(purifier_take_changes() & PF_PM_LIVE);
	set_aqi(70);
	run(2100);
	c = purifier_take_changes();
	CHECK(c & PF_PM_LIVE);
	CHECK(c & PF_PM);                  /* |delta| >= 20 */
}

/* The real stock EEPROM of this unit (evidence/dumps/eeprom-20261104-
 * diag1b.txt, rows not listed are 0xFF).  Stock (AIR_Var_Init 1f016a7c,
 * location table 0x20000fac) stores led (id 0x1b, 0x050) and buzzer (id
 * 0x1d, page 255 -> 0x1fe0) as plain int32 0/1, 1 = on; both are 0 here,
 * so stock really had the display and the buzzer off. */
static const struct { uint16_t addr; const char *hex; } stock_rows[] = {
	{ 0x0000, "00 00 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x0020, "73 6d 61 72 74 6d 69 5f 61 69 72 5f 6d 63 31 00 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x0040, "06 00 00 00 ff ff ff ff 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff" },
	{ 0x0060, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x0080, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x00a0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x00c0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x00e0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x0100, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x0120, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x0140, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x0160, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x0180, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x01a0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x01c0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x01e0, "00 00 00 00 72 52 00 00 00 00 00 00 00 00 00 00 5b 00 00 00 4b 00 00 00 01 00 00 00 ff ff ff ff" },
	{ 0x0200, "5e 01 00 00 94 02 00 00 06 04 00 00 e2 04 00 00 82 05 00 00 a4 06 00 00 06 04 00 00 ff ff ff ff" },
	{ 0x0240, "00 00 00 00 61 f4 2f 01 00 00 00 00 cd 3a 6e 00 95 fc 0a 00 00 00 00 00 ff ff ff ff ff ff ff ff" },
	{ 0x0260, "00 00 00 00 00 00 00 00 68 3c 01 00 80 70 00 00 01 00 00 00 00 00 00 00 04 00 00 00 ff ff ff ff" },
	{ 0x0280, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00 01 00 00 00 ac 0d 00 00 ff ff ff ff" },
	{ 0x02a0, "30 30 30 30 30 30 30 30 30 30 30 30 30 30 30 30 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x02c0, "30 30 30 30 30 30 30 30 30 30 30 30 30 30 30 30 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x0400, "ff ff ff ff ff ff 00 00 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x0600, "00 00 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff" },
	{ 0x1fe0, "00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff" },
};

static void load_stock_dump(void)
{
	for (unsigned r = 0; r < sizeof stock_rows / sizeof stock_rows[0]; r++) {
		const char *h = stock_rows[r].hex;
		for (unsigned i = 0; i < 32; i++)
			fake.eeprom[stock_rows[r].addr + i] =
				(uint8_t)strtoul(h + 3 * i, NULL, 16);
	}
}

/* ours: ambient light for HA, updated also in standby, >= 2 % steps */
static void test_ambient_light(void)
{
	fake_reset(false);
	fake.light = 0;
	boot();
	CHECK_EQ(S->light, 0);
	CHECK_EQ(pur_light_pct(LRAW(0)), 0);
	CHECK_EQ(pur_light_pct(20000), 100);
	purifier_take_changes();
	fake.light = 40;                   /* raw 10240, standby */
	run(1500);
	CHECK(purifier_take_changes() & PF_LIGHT);
	CHECK_EQ(S->light, pur_light_pct(LRAW(40)));
	CHECK(!S->power);
	uint8_t l = S->light;
	fake.light = 40;                   /* no change: no push */
	run(3000);
	CHECK(!(purifier_take_changes() & PF_LIGHT));
	CHECK_EQ(S->light, l);
	fake.light = 78;                   /* raw 19968, near max */
	run(1500);
	CHECK(purifier_take_changes() & PF_LIGHT);
	CHECK_RANGE(S->light, 98, 100);
}

static void test_eeprom_stock_dump(void)
{
	fake_reset(false);
	load_stock_dump();
	boot();
	CHECK(!S->led);
	CHECK(!S->buzzer);
	CHECK(!S->child_lock);
	CHECK(!S->power);                  /* cold boot; stored power 0 */
	CHECK_EQ(S->mode, MODE_FAVORITE);  /* 0x040 = 6 */
	CHECK_EQ(S->favorite_level, 4);    /* 0x218 = 1030 rpm */
	CHECK_EQ(S->filter_life, 0);       /* 0x244 = 19919969 s > 3500 h */
	CHECK_EQ(S->avg_pm25, 10);         /* 7224013 / 719893 */
	CHECK_EQ(fake.beeps, 0);           /* boot beep muted */
	CHECK_EQ(S->auto_bias, 0);         /* 0x800 erased in stock: 0 */
	settings_t s;
	CHECK_EQ(settings_load(&s), 0);
	CHECK(!s.led && !s.buzzer && !s.child_lock && !s.power_restore);
	CHECK_EQ(s.favorite_rpm, 1030);
	CHECK_EQ(s.silent_rpm, 350);
	run(60000);
	CHECK_EQ(fake.ee_writes, 0);       /* valid stock data: untouched */

	/* set_buzzer on (1f013564): store 1 at 0x1fe0 and beep once */
	uint32_t b = fake.beeps;
	purifier_set_buzzer(true);
	run(15000);
	CHECK_EQ(fake.beeps - b, 1);
	CHECK_EQ(fake_ee_get32(0x1fe0), 1);
	CHECK_EQ(fake.ee_writes, 1);
	b = fake.beeps;
	purifier_set_buzzer(false);        /* off: no beep */
	run(15000);
	CHECK_EQ(fake.beeps - b, 0);
	CHECK_EQ(fake_ee_get32(0x1fe0), 0);
	purifier_set_led(true);
	run(15000);
	CHECK_EQ(fake_ee_get32(0x050), 1);
	CHECK(S->led);
	CHECK_EQ(fake_ee_get32(0x800), 0xffffffffu);   /* never written */
}


/* ---------- ours: Auto speed bias ---------- */

static void test_auto_bias_logic(void)
{
	int bad = 0;
	for (unsigned r = 0; r <= 2100; r++)       /* 0 = bit-exact stock */
		bad += pur_auto_bias_rpm((uint16_t)r, 0) != r;
	CHECK_EQ(bad, 0);
	CHECK_EQ(pur_auto_bias_rpm(0, 50), 0);     /* stop stays stop */
	CHECK_EQ(pur_auto_bias_rpm(0, -50), 0);
	CHECK_EQ(pur_auto_bias_rpm(660, 10), 726);
	CHECK_EQ(pur_auto_bias_rpm(1250, 20), 1500);
	CHECK_EQ(pur_auto_bias_rpm(1030, -30), 721);
	CHECK_EQ(pur_auto_bias_rpm(1030, 50), 1545);
	CHECK_EQ(pur_auto_bias_rpm(1410, 40), 1700);   /* 1974 capped */
	CHECK_EQ(pur_auto_bias_rpm(1250, -50), 625);
	/* low end: never below 350 (the lowest stock auto speed) */
	CHECK_EQ(pur_auto_bias_rpm(350, -50), 350);
	CHECK_EQ(pur_auto_bias_rpm(350, -10), 350);
	CHECK_EQ(pur_auto_bias_rpm(660, -50), 350);  /* 330 */
	CHECK_EQ(pur_auto_bias_rpm(700, -50), 350);
	CHECK_EQ(pur_auto_bias_rpm(710, -50), 355);
	/* ... but a custom silent below 350 is not raised */
	CHECK_EQ(pur_auto_bias_rpm(300, -50), 300);
	CHECK_EQ(pur_auto_bias_rpm(300, 10), 330);
	/* high end: never above 1700 (stock's fastest), a custom speed above
	 * it is not lowered */
	CHECK_EQ(pur_auto_bias_rpm(1700, 50), 1700);
	CHECK_EQ(pur_auto_bias_rpm(1410, 50), 1700); /* 2115 */
	CHECK_EQ(pur_auto_bias_rpm(1410, 20), 1692);
	CHECK_EQ(pur_auto_bias_rpm(1700, 20), 1700);
	CHECK_EQ(pur_auto_bias_rpm(2100, 10), 2100);
	CHECK_EQ(pur_auto_bias_rpm(2100, -50), 1050);
	/* every bias in range, every rpm: inside the clamp, monotonic */
	bad = 0;
	for (int b = AUTO_BIAS_MIN; b <= AUTO_BIAS_MAX; b += AUTO_BIAS_STEP) {
		uint16_t prev = 0;
		for (unsigned r = 300; r <= 2100; r++) {
			uint16_t o = pur_auto_bias_rpm((uint16_t)r, b);
			if (o < (r < 350 ? r : 350) || o > (r > 1700 ? r : 1700) || o < prev)
				bad++;
			prev = o;
		}
	}
	CHECK_EQ(bad, 0);
}

/* The auto state machine runs on the unscaled target: for every bias the
 * fan target is exactly bias(stock target) over an up/down sweep that
 * crosses every zone and hysteresis band, and bias 0 is the stock model. */
static void test_auto_bias_sweep(void)
{
	static const uint16_t sweep[] = {
		5, 15, 25, 15, 10, 40, 50, 42, 70, 80, 78, 95, 110, 118, 130,
		145, 152, 200, 245, 252, 258, 300, 252, 245, 200, 152, 145, 118,
		110, 95, 78, 70, 42, 35, 30, 15, 12, 5, 600, 15, 21, 5,
	};
	static const int biases[] = { 0, -50, -20, 10, 30, 50 };
	for (unsigned bi = 0; bi < sizeof biases / sizeof biases[0]; bi++) {
		int b = biases[bi];
		int bad = 0;
		fresh();
		CHECK_EQ(purifier_set_auto_bias(b), 0);
		set_aqi(5);
		CHECK_EQ(purifier_set_power(true), 0);
		run(3000);
		uint16_t model = pur_auto_rpm_for_state(0, &defsp);
		CHECK_EQ(S->target_rpm, pur_auto_bias_rpm(model, b));
		for (unsigned i = 0; i < sizeof sweep / sizeof sweep[0]; i++) {
			set_aqi(sweep[i]);
			run(12100);        /* > hold: at least one evaluation */
			model = pur_auto_target(S->pm25, model, &defsp, 75);
			uint16_t want = pur_auto_bias_rpm(model, b);
			if (S->target_rpm != want && bad++ < 3)
				printf("  bias %d aqi %u: target %u, want %u (stock %u)\n",
				       b, S->pm25, S->target_rpm, want, model);
			if (b == 0 && S->target_rpm != model)
				bad++;
		}
		CHECK_EQ(bad, 0);
	}
}

static void test_auto_bias_apply(void)
{
	fresh();
	CHECK_EQ(S->auto_bias, 0);
	set_aqi(100);                      /* zone 2 inside -> med_l 1030 */
	CHECK_EQ(purifier_set_power(true), 0);
	run(15000);
	CHECK_EQ(S->target_rpm, 1030);
	purifier_take_changes();
	uint32_t b = fake.beeps;
	/* applies at once, no 10 s hold, no beep */
	CHECK_EQ(purifier_set_auto_bias(20), 0);
	CHECK_EQ(S->auto_bias, 20);
	CHECK_EQ(S->target_rpm, 1236);
	uint32_t c = purifier_take_changes();
	CHECK(c & PF_AUTO_BIAS);
	CHECK(c & PF_RPM);
	run(50);
	CHECK_EQ(fake.beeps, b);
	run(8000);
	CHECK_EQ(fake.fan_cmd, 1236);      /* the motor follows (jump) */
	CHECK_EQ(purifier_set_auto_bias(-50), 0);
	CHECK_EQ(S->target_rpm, 515);
	CHECK_EQ(purifier_set_auto_bias(-50), 0);  /* unchanged: no flag */
	purifier_take_changes();
	CHECK_EQ(purifier_set_auto_bias(-50), 0);
	CHECK(!(purifier_take_changes() & PF_AUTO_BIAS));
	/* invalid: rejected, nothing changes */
	CHECK_EQ(purifier_set_auto_bias(15), -5001);
	CHECK_EQ(purifier_set_auto_bias(60), -5001);
	CHECK_EQ(purifier_set_auto_bias(-60), -5001);
	CHECK_EQ(purifier_set_auto_bias(-5), -5001);
	CHECK_EQ(S->auto_bias, -50);
	CHECK_EQ(S->target_rpm, 515);
	CHECK(!(purifier_take_changes() & PF_AUTO_BIAS));
	/* Silent and Favorite ignore the bias, also when it changes there */
	CHECK_EQ(purifier_set_mode(MODE_SILENT), 0);
	CHECK_EQ(S->target_rpm, 350);
	CHECK_EQ(purifier_set_auto_bias(50), 0);
	CHECK_EQ(S->target_rpm, 350);
	run(15000);
	CHECK_EQ(S->target_rpm, 350);
	CHECK_EQ(purifier_set_mode(MODE_FAVORITE), 0);
	CHECK_EQ(purifier_set_favorite_level(5), 0);
	CHECK_EQ(S->target_rpm, 1080);
	CHECK_EQ(purifier_set_auto_bias(-50), 0);
	CHECK_EQ(S->target_rpm, 1080);
	run(15000);
	CHECK_EQ(S->target_rpm, 1080);
	/* back to Auto: the biased stock target at once */
	CHECK_EQ(purifier_set_auto_bias(30), 0);
	CHECK_EQ(purifier_set_mode(MODE_AUTO), 0);
	CHECK_EQ(S->target_rpm, 1339);     /* 1030 * 1.3 */
	/* standby: stays 0; power on: biased */
	CHECK_EQ(purifier_set_power(false), 0);
	CHECK_EQ(purifier_set_auto_bias(50), 0);
	CHECK_EQ(S->target_rpm, 0);
	CHECK_EQ(purifier_set_power(true), 0);
	CHECK_EQ(S->target_rpm, 1545);
	/* top zone, +50: clamped at 1700 (stock's fastest) */
	set_aqi(300);
	run(15000);
	CHECK_EQ(S->target_rpm, 1700);
	CHECK_EQ(purifier_set_auto_bias(0), 0);  /* back to stock at once */
	CHECK_EQ(S->target_rpm, 1700);
}

static void test_auto_bias_eeprom(void)
{
	/* own location: 0x800, clear of every other element */
	CHECK_EQ(store_addr(E_AUTO_BIAS), 0x800);
	int overlap = 0;
	for (int i = 0; i < E_COUNT; i++)
		for (int j = i + 1; j < E_COUNT; j++) {
			int d = store_addr((store_elem)i) - store_addr((store_elem)j);
			overlap += d > -4 && d < 4;
		}
	CHECK_EQ(overlap, 0);

	fresh();                           /* blank chip: defaults written */
	CHECK_EQ(fake_ee_get32(0x800), 0);
	CHECK_EQ(purifier_set_auto_bias(-30), 0);
	run(15000);
	CHECK_EQ(fake_ee_get32(0x800), (uint32_t)-30);
	reboot(false);
	CHECK_EQ(S->auto_bias, -30);
	CHECK_EQ(purifier_set_auto_bias(50), 0);
	purifier_flush();
	CHECK_EQ(fake_ee_get32(0x800), 50);
	reboot(true);
	CHECK_EQ(S->auto_bias, 50);
	CHECK_EQ(S->target_rpm, 0);        /* still off (no power restore) */

	/* erased / invalid -> 0, and not written back */
	static const uint32_t badv[] = { 0xffffffffu, 25, 60, (uint32_t)-60,
					 0x7fffffff, 0x80000000u, 5 };
	for (unsigned i = 0; i < sizeof badv / sizeof badv[0]; i++) {
		fake_ee_put32(0x800, badv[i]);
		reboot(false);
		CHECK_EQ(S->auto_bias, 0);
		purifier_set_led(!S->led);  /* some other save */
		run(15000);
		CHECK_EQ(fake_ee_get32(0x800), badv[i]);
	}
	/* edges are valid */
	fake_ee_put32(0x800, (uint32_t)-50);
	reboot(false);
	CHECK_EQ(S->auto_bias, -50);
	fake_ee_put32(0x800, 0xffffffffu);
	reboot(false);
	CHECK_EQ(purifier_set_auto_bias(0), 0);   /* reads 0 already: no write */
	run(15000);
	CHECK_EQ(fake_ee_get32(0x800), 0xffffffffu);
	CHECK_EQ(purifier_set_auto_bias(10), 0);
	run(15000);
	CHECK_EQ(fake_ee_get32(0x800), 10);
}

int main(void)
{
	static const struct { const char *name; void (*fn)(void); } tests[] = {
		{ "logic", test_logic },
		{ "auto_curve", test_auto_curve },
		{ "auto_hold", test_auto_hold },
		{ "modes", test_modes },
		{ "boot_power_rules", test_boot_power_rules },
		{ "door", test_door },
		{ "fav_levels", test_fav_levels },
		{ "ramp", test_ramp },
		{ "filter_accounting", test_filter_accounting },
		{ "filter_reset", test_filter_reset },
		{ "child_lock", test_child_lock },
		{ "eeprom_roundtrip", test_eeprom_roundtrip },
		{ "eeprom_corruption", test_eeprom_corruption },
		{ "pm_sleep_wake", test_pm_sleep_wake },
		{ "buttons", test_buttons },
		{ "stall", test_stall },
		{ "light_leds", test_light_leds },
		{ "eeprom_foreign", test_eeprom_foreign_magic },
		{ "eeprom_stock_dump", test_eeprom_stock_dump },
		{ "ambient_light", test_ambient_light },
		{ "display", test_display },
		{ "time_wrap", test_time_wrap },
		{ "flags", test_flags },
		{ "screen_times", test_screen_times },
		{ "filter_popups", test_filter_popups },
		{ "aqi_force_push", test_aqi_force_push },
		{ "auto_hold_mode", test_auto_hold_mode },
		{ "pm_fault_ha", test_pm_fault_ha },
		{ "pm_stall_replay", test_pm_stall_replay },
		{ "pm_live_flag", test_pm_live_flag },
		{ "ramp_smooth", test_ramp_smooth },
		{ "auto_bias_logic", test_auto_bias_logic },
		{ "auto_bias_sweep", test_auto_bias_sweep },
		{ "auto_bias_apply", test_auto_bias_apply },
		{ "auto_bias_eeprom", test_auto_bias_eeprom },
	};
	for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
		int f = fails;
		cur_test = tests[i].name;
		tests[i].fn();
		printf("%-20s %s\n", tests[i].name, fails == f ? "ok" : "FAILED");
	}
	printf("%d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
