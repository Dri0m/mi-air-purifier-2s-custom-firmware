/* logic.c - pure purifier computations (fw-app-logic.md 2.x, 3.x, 4.1,
 * 5.3).  No state, no HAL: unit-testable in isolation. */
#include "pur_int.h"

const uint16_t pur_fav_table[FAVORITE_LEVELS] = {
	350, 660, 850, 960, 1030, 1080, 1250, 1360,
	1410, 1460, 1510, 1560, 1610, 1660, 1700,
};

int pur_fav_index(uint16_t rpm)
{
	for (int i = 0; i < FAVORITE_LEVELS; i++)
		if (pur_fav_table[i] == rpm)
			return i;
	return -1;
}

/* 3.3: (int)(0.38 / (0.62 / v - 0.0003)) == 0.38v / (0.62 - 0.0003v)
 * == 3800v / (6200 - 3v), evaluated exactly in integers.  Stock uses soft
 * double; the only input where its rounding differs is v = 496 (exact
 * quotient 400, double gives 399.99.. -> 399).  The host test compares
 * against the double formula for every input. */
uint16_t pur_pm_correct_bm(uint16_t v)
{
	if (v < 2)
		return 0;
	if (v >= 1000)
		return 999;
	if (v == 496)
		return 399;
	return (uint16_t)((3800u * v) / (6200u - 3u * v));
}

uint16_t pur_pm_to_aqi(uint16_t pm)
{
	if (pm == 0)
		return 1;
	return pm > 600 ? 600 : pm;
}

uint8_t pur_aqi_state(uint16_t a, uint16_t goodh)
{
	if (a <= PUR_ZONE0) return 0;
	if (a <= goodh) return 1;
	if (a <= PUR_ZONE2) return 2;
	if (a <= PUR_ZONE3) return 3;
	if (a < PUR_ZONE4) return 4;
	return 5;
}

uint16_t pur_auto_rpm_for_state(uint8_t s, const pur_speeds *sp)
{
	switch (s) {               /* 2.1: 0->1, 1->5, 2->7, 3->4, 4->3, 5->2 */
	case 0: return sp->silent;
	case 1: return sp->low;
	case 2: return sp->med_l;
	case 3: return sp->med;
	case 4: return sp->high;
	default: return sp->strong;
	}
}

static bool inside(uint16_t a, uint16_t lo, uint16_t hi)
{
	return (int)a - lo >= PUR_AUTO_DEADBAND &&
	       (int)hi - a >= PUR_AUTO_DEADBAND;
}

/* 2.3, main loop 1f0159ec (auto block 1f015c42..1f015f6e).  Comparisons
 * are on RPM values.  Returns true when stock "assigns" a target (it then
 * also restarts the 10 s hold, even if the value is unchanged); false when
 * it keeps the current one (hysteresis band, or a clamp that is a no-op). */
bool pur_auto_eval(uint16_t a, uint16_t t, const pur_speeds *s,
		   uint16_t goodh, uint16_t *out)
{
	uint16_t r = t;
	bool set = true;

	if (a <= PUR_ZONE0) {
		if (a < 11)
			r = s->silent;
		else if (a > 20)
			r = pur_fav_table[1];      /* 660, a constant */
		else
			set = false;               /* 11..20 hysteresis */
	} else if (a <= goodh) {
		/* lower bound Z0: 1f015d76 is called with r0 = mode = 0 */
		if (inside(a, PUR_ZONE0, goodh))
			r = s->low;
		else if (t > s->med_l)
			r = s->med_l;
		else
			set = false;
	} else if (a <= PUR_ZONE2) {
		if (inside(a, goodh, PUR_ZONE2))
			r = s->med_l;
		else if (t > s->med)
			r = s->med;
		else if (t < s->low)
			r = s->low;
		else
			set = false;
	} else if (a <= PUR_ZONE3) {
		if (inside(a, PUR_ZONE2, PUR_ZONE3))
			r = s->med;
		else if (t > s->high)
			r = s->high;
		else if (t < s->med_l)
			r = s->med_l;
		else
			set = false;
	} else if (a < PUR_ZONE4) {
		if (inside(a, PUR_ZONE3, PUR_ZONE4))
			r = s->high;
		else if (t < s->med)
			r = s->med;
		else
			set = false;
	} else if (a - PUR_ZONE4 >= PUR_AUTO_DEADBAND) {
		r = s->strong;
	} else if (t < s->high) {
		r = s->high;
	} else {
		set = false;
	}
	*out = r;
	return set;
}

uint16_t pur_auto_target(uint16_t a, uint16_t t, const pur_speeds *s,
			 uint16_t goodh)
{
	uint16_t r;
	pur_auto_eval(a, t, s, goodh, &r);
	return r;
}

/* Ours, not stock: Auto speed bias.  rpm * (100 + bias) / 100, rounded
 * to nearest, then clamped to PUR_BIAS_RPM_MIN..PUR_BIAS_RPM_MAX.  The
 * lower bound never raises a value that was already below it (a custom
 * m1_silent of 300..349 stays reachable), the upper bound (the fastest
 * speed stock itself uses; the motor was never run harder) never lowers a
 * custom speed above it, and bias 0 or rpm 0 (stop) are
 * returned unchanged, so bias 0 is bit-exact stock. */
uint16_t pur_auto_bias_rpm(uint16_t rpm, int bias)
{
	if (bias == 0 || rpm == 0)
		return rpm;
	if (bias < AUTO_BIAS_MIN)
		bias = AUTO_BIAS_MIN;
	if (bias > AUTO_BIAS_MAX)
		bias = AUTO_BIAS_MAX;
	uint32_t r = ((uint32_t)rpm * (uint32_t)(100 + bias) + 50) / 100;
	uint32_t lo = rpm < PUR_BIAS_RPM_MIN ? rpm : PUR_BIAS_RPM_MIN;
	if (r < lo)
		r = lo;
	uint32_t hi = rpm > PUR_BIAS_RPM_MAX ? rpm : PUR_BIAS_RPM_MAX;
	if (r > hi)
		r = hi;
	return (uint16_t)r;
}

/* 2.4: one FAV step per call (500 ms) when both ends are table values;
 * from standstill start at FAV[0]; otherwise jump. */
uint16_t pur_ramp_step(uint16_t cur, uint16_t target)
{
	if (cur == target || target == 0)
		return target;
	int ti = pur_fav_index(target);
	if (ti < 0)
		return target;
	if (cur == 0)
		return pur_fav_table[0];
	int ci = pur_fav_index(cur);
	if (ci < 0)
		return target;
	return pur_fav_table[ci + (ti > ci ? 1 : -1)];
}

/* Ours: like pur_ramp_step, but a target between table values (Auto
 * speed bias) is approached through the FAV values that lie between cur
 * and target, one per call, then reached exactly; from standstill start
 * at FAV[0] when the target is above it.  Used only while a bias is set,
 * so bias 0 keeps stock's ramp bit-exact. */
uint16_t pur_ramp_step_smooth(uint16_t cur, uint16_t target)
{
	if (cur == target || target == 0)
		return target;
	if (cur == 0)
		return target > pur_fav_table[0] ? pur_fav_table[0] : target;
	if (target > cur) {
		for (int i = 0; i < FAVORITE_LEVELS; i++)
			if (pur_fav_table[i] > cur)
				return pur_fav_table[i] < target ? pur_fav_table[i] : target;
	} else {
		for (int i = FAVORITE_LEVELS - 1; i >= 0; i--)
			if (pur_fav_table[i] < cur)
				return pur_fav_table[i] > target ? pur_fav_table[i] : target;
	}
	return target;
}

bool pur_aqi_report_due(uint16_t a, uint16_t last)
{
	unsigned d = a > last ? a - last : last - a;
	if (a <= 150)
		return d >= 20;
	if (a <= 250)
		return d >= 40;
	return d >= 60;
}

uint8_t pur_bright(uint16_t raw)
{
	if (raw <= 256)
		return 0;
	if (raw < 20000)
		return (uint8_t)((uint32_t)(raw - 256) * 200 / 19744);
	return 200;
}

/* 1f012580: raw <= 256 -> 1, raw >= 20000 -> 255, else
 * (raw-256)*255/19744 with 0 -> 1.  The OLED contrast uses this value. */
uint8_t pur_light_pwm(uint16_t raw)
{
	if (raw >= 20000)
		return 255;
	int32_t p = raw <= 256 ? 1 : (int32_t)(raw - 256) * 255 / 19744;
	return (uint8_t)(p == 0 ? 1 : p);
}

/* Ambient light for HA, 0..100 %: the same 0..255 scale, 1 -> 0 %. */
uint8_t pur_light_pct(uint16_t raw)
{
	uint8_t p = pur_light_pwm(raw);
	return (uint8_t)(p <= 1 ? 0 : ((unsigned)p * 100 + 127) / 255);
}

/* The LED PWM is the same value clamped to 15..245. */
uint8_t pur_led_pwm(uint16_t raw)
{
	uint8_t p = pur_light_pwm(raw);
	return p < 15 ? 15 : p > 245 ? 245 : p;
}

uint8_t pur_oled_contrast(uint8_t pwm)
{
	unsigned c = pwm >= 168 ? 96 : (unsigned)pwm * 100 / 175;
	return (uint8_t)(c + 1);
}

uint32_t pur_filter_used_s(uint32_t stored, uint32_t lo, uint32_t mid,
			   uint32_t hi)
{
	uint64_t u = (uint64_t)stored + lo + (uint64_t)mid * 3 / 2 +
		     (uint64_t)hi * 2;
	return u > 0xffffffffu ? 0xffffffffu : (uint32_t)u;
}

uint8_t pur_filter_life(uint32_t used_h, uint32_t f1_hour)
{
	if (f1_hour == 0 || used_h >= f1_hour)
		return 0;
	return (uint8_t)((f1_hour - used_h) * 100 / f1_hour);
}
