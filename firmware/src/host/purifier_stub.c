/* purifier_stub.c - fake purifier.h for host tests of the ESPHome API.
 * Host-only; never link this into the device image or together with
 * src/purifier/.  Commands are logged to stderr as "STUB <cmd> <arg> -> rc". */
#include <stdio.h>
#include "purifier.h"
#include "purifier_stub.h"

static purifier_state st;

static void set_u8(uint8_t *f, uint8_t v, uint32_t flag)
{
	if (*f != v) {
		*f = v;
		st.changed |= flag;
	}
}

static void set_b(bool *f, bool v, uint32_t flag)
{
	if (*f != v) {
		*f = v;
		st.changed |= flag;
	}
}

static int logrc(const char *what, int arg, int rc)
{
	fprintf(stderr, "STUB %s %d -> %d\n", what, arg, rc);
	return rc;
}

void purifier_init(void)
{
	st.power = true;
	st.mode = MODE_AUTO;
	st.favorite_level = 7;
	st.led = true;
	st.buzzer = true;
	st.child_lock = false;
	st.door_open = false;
	st.pm25 = 12;
	st.avg_pm25 = 12;
	st.temp_dC = 215;
	st.humidity = 45;
	st.bright = 100;
	st.filter_life = 87;
	st.filter_hours = 450;
	st.changed = 0xffffffffu;
}

void purifier_tick(void)
{
	uint16_t target = 0;
	if (st.power) {
		switch (st.mode) {
		case MODE_AUTO: target = (uint16_t)(800 * (100 + st.auto_bias) / 100); break;
		case MODE_SILENT: target = 400; break;
		case MODE_FAVORITE: target = (uint16_t)(300 + 100 * st.favorite_level); break;
		}
	}
	st.target_rpm = target;
	if (st.motor_rpm != target) {
		int d = (int)target - (int)st.motor_rpm;
		if (d > 50)
			d = 50;
		if (d < -50)
			d = -50;
		st.motor_rpm = (uint16_t)(st.motor_rpm + d);
		st.changed |= PF_RPM;
	}
}

const purifier_state *purifier_get(void) { return &st; }

uint32_t purifier_take_changes(void)
{
	uint32_t c = st.changed;
	st.changed = 0;
	return c;
}

int purifier_set_power(bool on)
{
	if (st.door_open)
		return logrc("set_power", on, -6003);
	set_b(&st.power, on, PF_POWER);
	return logrc("set_power", on, 0);
}

int purifier_set_mode(purifier_mode m)
{
	if (m != MODE_AUTO && m != MODE_SILENT && m != MODE_FAVORITE)
		return logrc("set_mode", m, -5001);
	if (st.door_open)
		return logrc("set_mode", m, -6003);
	if (st.mode != m) {
		st.mode = m;
		st.changed |= PF_MODE;
	}
	/* like purifier.c (stock 1f012e70): a mode command powers on */
	set_b(&st.power, true, PF_POWER);
	return logrc("set_mode", m, 0);
}

int purifier_set_favorite_level(uint8_t level)
{
	if (level >= FAVORITE_LEVELS)
		return logrc("set_favorite_level", level, -5001);
	set_u8(&st.favorite_level, level, PF_FAV);
	return logrc("set_favorite_level", level, 0);
}

int purifier_set_led(bool on)
{
	set_b(&st.led, on, PF_LED);
	return logrc("set_led", on, 0);
}

int purifier_set_buzzer(bool on)
{
	set_b(&st.buzzer, on, PF_BUZZER);
	return logrc("set_buzzer", on, 0);
}

int purifier_set_child_lock(bool on)
{
	set_b(&st.child_lock, on, PF_LOCK);
	return logrc("set_child_lock", on, 0);
}

int purifier_set_auto_bias(int bias)
{
	if (bias < AUTO_BIAS_MIN || bias > AUTO_BIAS_MAX || bias % AUTO_BIAS_STEP)
		return logrc("set_auto_bias", bias, -5001);
	if (st.auto_bias != bias) {
		st.auto_bias = (int8_t)bias;
		st.changed |= PF_AUTO_BIAS;
	}
	return logrc("set_auto_bias", bias, 0);
}

int purifier_reset_filter(void)
{
	st.filter_life = 100;
	st.filter_hours = 0;
	st.changed |= PF_FILTER;
	return logrc("reset_filter", 0, 0);
}

void purifier_stub_set_door(bool open) { set_b(&st.door_open, open, PF_DOOR); }

void purifier_stub_set_pm25(uint16_t pm)
{
	if (st.pm25 != pm) {
		st.pm25 = pm;
		st.changed |= PF_PM;
	}
}

void purifier_stub_set_temp(int16_t t)
{
	if (st.temp_dC != t) {
		st.temp_dC = t;
		st.changed |= PF_TEMP;
	}
}

void purifier_stub_set_humidity(uint8_t rh) { set_u8(&st.humidity, rh, PF_HUM); }

void purifier_stub_format(char *buf, unsigned size)
{
	snprintf(buf, size,
		 "STATE power=%d mode=%d fav=%u led=%d buzzer=%d lock=%d door=%d "
		 "pm25=%u temp=%d hum=%u rpm=%u filter_life=%u filter_hours=%u bias=%d",
		 st.power, (int)st.mode, st.favorite_level, st.led, st.buzzer,
		 st.child_lock, st.door_open, st.pm25, st.temp_dC, st.humidity,
		 st.motor_rpm, st.filter_life, (unsigned)st.filter_hours,
		 st.auto_bias);
}
