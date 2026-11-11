/* purifier.c - power/mode state machine, fan control, sensors, filter
 * accounting, buttons and persistence policy.  Behavior per
 * evidence/fw-app-logic.md (section numbers in comments).
 *
 * Single-threaded: purifier_tick() and the purifier_set_*() commands must
 * be called from the same task.  All timing derives from hal_millis().
 */
#include <string.h>
#include "hal.h"
#include "settings.h"
#include "purifier.h"
#include "pur_int.h"

/* stock timers */
#define T_LIGHT_MS        200      /* 5.3 */
#define T_MOTOR_MS        500      /* 2.4 */
#define AMBIENT_STEP      2        /* % change that pushes st.light */
#define T_SEC_MS          1000     /* power tick: door, filter, holds */
#define T_SENSOR_MS       2000     /* 3.1 */
#define SHT_PERIOD_S      20       /* 3.4 */
#define FILTER_PERIOD_S   300      /* 4.2 */
#define AVG_PERIOD_S      300      /* 3.2 */
#define AQI_FORCE_S       1800     /* 3.1 */
#define AUTO_HOLD_MS      10000    /* 2.3 */
#define PM_IDLE_READS     150      /* 1f013adc: sleep when idle > 149 */
#define PM_SLEEP_ON       60       /* sensor ticks asleep, powered on */
#define PM_SLEEP_OFF      300      /* sensor ticks asleep, standby */
#define PM_WAKE_ADD       60       /* stock "+0x3c" wake nudge */
#define PM_WARMUP         7        /* 14 s */
#define PM_FAIL_MAX       5
#define PM_PLACEHOLDER    23
#define AQI_BOOT          20       /* 1f0150d8: aqi struct [0] = 0x14 */
/* Holds count 1 s power ticks (1f0143fc), so they act 1-2 s / 5-6 s
 * after the press, as in stock (main loop 1f0159ec). */
#define POWER_HOLD_TICKS  2        /* counter > 1 */
#define LED_HOLD_TICKS    6        /* counter > 5 */
#define COMBO_ARM_TICKS   5        /* both counters > 4 */
#define COMBO_DATA_TICKS  15       /* both > 14: data reset, auto-fire */
#define MOTOR_WINDOW      10       /* 1f0128d0: 10 x 500 ms FG window */
#define BRIGHT_SAMPLES    300      /* 1.4, counted since the last push */
/* UI durations from stock (main loop 1f0159ec, timer cb 1f013b80) */
#define SCR_MODE_MS       2500     /* screen 3: 0x9c4 */
#define SCR_MSG_MS        4000     /* 0xe/0xf/0x13/0x14: 0xfa0 */
#define SCR_STBY_MS       1500     /* standby lock/door: sleep 0x5dc */
#define WAKE_MS           10000    /* main while LED off: 0x2710 */
#define FILTER_POP_MS     10000    /* 0x20000f94 */
#define FILTER_POP0_MS    30000    /* life 0: 0x7530 */
#define FILTER_EXP_ON_S   60       /* expired: popup 60 s ... */
#define FILTER_EXP_OFF_S  180      /* ... every 180 s */
#define FILTER_LOW_EVERY_S 28800   /* life 0..4: every 8 h (0x7080) */
/* persistence rate limit */
#define SAVE_DEBOUNCE_MS  2000
#define SAVE_MAX_DELAY_MS 30000
#define SAVE_MIN_GAP_MS   10000

enum { STBY_ON = 0, STBY_OFF = 1, STBY_FAULT = 4 };
enum { PM_RUN = 0, PM_WARM = 1, PM_SLEEP = 2 };

static purifier_state st;

static struct {
	uint8_t standby;
	settings_t set;
	pur_speeds sp;
	uint16_t goodh;
	uint32_t f1_hour;
	uint32_t aqi_sum, aqi_cnt;

	uint32_t t_light, t_motor, t_sec, t_sensor;
	uint32_t uptime_s, on_s;

	/* fan */
	uint16_t target, cmd;
	uint16_t auto_raw;         /* ours: the stock auto target before the
				    * Auto speed bias; the 2.3 state machine
				    * runs on it, so the bias only scales its
				    * output (target = bias(auto_raw)) */
	uint32_t t_auto;
	uint8_t win_ticks;
	bool grace;

	/* PM sensor */
	uint8_t pm_run;
	uint16_t pm_idle, pm_cnt;
	uint8_t pm_fail;
	bool frame;
	uint16_t frame_raw;
	uint16_t aqi;
	uint8_t aqi_state;
	bool aqi_reported;
	uint16_t aqi_rep;
	uint32_t aqi_rep_s;
	uint16_t pm_live;         /* last value flagged PF_PM_LIVE */
	uint32_t pm_live_s;

	/* SHT, light */
	bool temp_reported;
	int16_t temp_rep;
	uint16_t light_n;
	uint8_t bright_rep, pwm, light_pwm;
	int16_t contrast;

	/* filter */
	uint32_t t_low, t_mid, t_high;
	uint8_t life_seen;
	bool expired;
	uint32_t popup_s;          /* last 0x16 popup with life >= 0 */
	bool tag_valid;
	uint8_t uid[7];

	/* keys (stock 0x104328 states, 0x104350 hold ticks) */
	uint8_t key_raw, key_stable;
	uint8_t kst[2];            /* 0 idle, 1 down, 3 both down */
	uint8_t hold[2];
	uint8_t armed;             /* 0x106d9c: 2 armed, 1 fire */

	/* persistence */
	bool dirty;
	uint32_t dirty_first, dirty_last, saved_t;
	bool saved_once;
} g;

/* ---------- helpers ---------- */

static void flag(uint32_t f) { st.changed |= f; }

static void set_bool(bool *f, bool v, uint32_t pf)
{
	if (*f != v) {
		*f = v;
		flag(pf);
	}
}

static void mark_dirty(void)
{
	uint32_t now = hal_millis();
	if (!g.dirty)
		g.dirty_first = now;
	g.dirty = true;
	g.dirty_last = now;
}

static void save_now(void)
{
	settings_save(&g.set);
	g.dirty = false;
	g.saved_t = hal_millis();
	g.saved_once = true;
}

static void beep(int pattern) { ui_beep(pattern, st.buzzer); }

static bool every(uint32_t *t, uint32_t period, uint32_t now)
{
	if (now - *t < period)
		return false;
	if (now - *t >= 60u * period)      /* long stall: don't replay */
		*t = now - period;
	*t += period;
	return true;
}

static void update_fav_level(void)
{
	int i = pur_fav_index(g.set.favorite_rpm);
	uint8_t lvl = i < 0 ? FAVORITE_LEVELS : (uint8_t)i;
	if (lvl != st.favorite_level) {
		st.favorite_level = lvl;
		flag(PF_FAV);
	}
}

static void set_target(uint16_t rpm)
{
	g.target = rpm;
	if (st.target_rpm != rpm) {
		st.target_rpm = rpm;
		flag(PF_RPM);
	}
}

/* Auto mode: remember the stock (unscaled) target, run the motor at the
 * biased one. */
static void set_auto_target(uint16_t raw)
{
	g.auto_raw = raw;
	set_target(pur_auto_bias_rpm(raw, st.auto_bias));
}

/* 2.1: fan target for the current mode */
static void apply_mode(void)
{
	if (g.standby != STBY_ON) {
		set_target(0);
		return;
	}
	switch (st.mode) {
	case MODE_SILENT:   set_target(g.sp.silent); break;
	case MODE_FAVORITE: set_target(g.sp.favorite); break;
	default:   /* the runmode handler leaves the auto hold alone */
		set_auto_target(pur_auto_rpm_for_state(g.aqi_state, &g.sp));
		break;
	}
}

static void motor_out(uint16_t rpm)
{
	if (rpm != g.cmd) {
		g.cmd = rpm;
		hal_fan_set_rpm(rpm);
	}
}

/* ---------- filter (4) ---------- */

static uint32_t used_s(void)
{
	return pur_filter_used_s(g.set.filter_usage, g.t_low, g.t_mid,
				 g.t_high);
}

static void filter_fold(void)
{
	uint32_t u = pur_filter_used_s(g.set.filter_usage, g.t_low,
				       g.t_mid & ~1u, g.t_high);
	g.t_low = g.t_high = 0;
	g.t_mid &= 1u;             /* keep the odd half second */
	if (u != g.set.filter_usage) {
		g.set.filter_usage = u;
		mark_dirty();
	}
}

static void filter_refresh(void)
{
	uint32_t h = used_s() / 3600;
	uint8_t life = pur_filter_life(h, g.f1_hour);
	g.expired = h >= g.f1_hour;        /* stock life -1 (1f017a18) */
	if (h != st.filter_hours || life != st.filter_life) {
		st.filter_hours = h;
		st.filter_life = life;
		flag(PF_FILTER);
	}
}

static void filter_reset(void)
{
	g.t_low = g.t_mid = g.t_high = 0;
	g.set.filter_usage = 0;
	mark_dirty();
	filter_refresh();
	g.life_seen = st.filter_life;
}

/* 4.3, reduced: the custom HAL does not authenticate tags (the poll is
 * expected to return false).  If a tag is ever reported: a new UID loads
 * its usage, the same UID gets the usage written back, and reset is
 * refused while it is present.  Unlike stock, a missing tag never zeroes
 * the local usage, and E_RFID in the EEPROM is left as stock wrote it. */
static void rfid_sync(void)
{
	uint8_t uid[7];
	uint32_t usage = 0;
	bool present = hal_rfid_poll(uid, &usage);

	if (present) {
		if (!g.tag_valid || memcmp(uid, g.uid, sizeof uid) != 0) {
			memcpy(g.uid, uid, sizeof uid);
			g.tag_valid = true;
			if (usage != 0xffffffffu) {
				g.t_low = g.t_mid = g.t_high = 0;
				g.set.filter_usage = usage;
				mark_dirty();
			}
		} else {
			filter_fold();
			hal_rfid_write_usage(g.set.filter_usage);
		}
	} else {
		g.tag_valid = false;
	}
	if (present != st.rfid_present) {
		st.rfid_present = present;
		flag(PF_FILTER);
	}
	filter_refresh();
}

/* ---------- power (6.1) ---------- */

/* Stock adds 60 to the sleep counter of a sleeping sensor (power_on
 * 1f014f8c, LED on 1f013298/1f0159ec, light update 1f012580): powered
 * on, it wakes at the next 2 s tick.  The idle-read counter is left
 * alone; only get_prop "aqi" resets it in stock. */
static void pm_wake(void)
{
	if (g.pm_run == PM_SLEEP)
		g.pm_cnt = (uint16_t)(g.pm_cnt + PM_WAKE_ADD);
}

static void light_tick(void);

/* 1f014f8c: single beep, sensor nudge, light update, persist power=1 */
static void power_on(void)
{
	beep(BEEP_SINGLE);
	rfid_sync();
	g.standby = STBY_ON;
	set_bool(&st.motor_fault, false, PF_MOTOR_FAULT);
	g.kst[0] = g.kst[1] = 0;
	g.hold[0] = 0;
	if (!st.power) {
		st.power = true;
		flag(PF_POWER);
	}
	if (!g.set.power_restore) {
		g.set.power_restore = true;
		mark_dirty();
	}
	ui_clear_transient();
	pm_wake();
	light_tick();
	apply_mode();
}

/* Standby path of 1f0159ec: persist power=0, target 0, long beep, blank
 * (or the fault screen for code 4), LEDs off. */
static void go_standby(uint8_t code)
{
	beep(BEEP_LONG);
	g.standby = code;
	/* HA "Motor fault": standby code 4 (m1_stuck) until the code changes */
	set_bool(&st.motor_fault, code == STBY_FAULT, PF_MOTOR_FAULT);
	if (st.power) {
		st.power = false;
		flag(PF_POWER);
	}
	if (g.set.power_restore) {
		g.set.power_restore = false;
		mark_dirty();
	}
	set_target(0);
	motor_out(0);
	ui_clear_transient();
}

/* ---------- PM sensor (3.1, 3.3) ---------- */

#define PM_LIVE_STEP   2
#define PM_LIVE_MIN_S  30

static void aqi_update(uint16_t raw)
{
	uint16_t a = pur_pm_to_aqi(hal_pm_header() == 'B' ? pur_pm_correct_bm(raw) : raw);
	g.pm_fail = 0;
	set_bool(&st.pm_fault, false, PF_PM_FAULT);
	g.aqi = a;
	g.aqi_state = pur_aqi_state(a, g.goodh);
	/* Not stock: HA gets the first value after boot or a fault at once,
	 * changes of >= PM_LIVE_STEP at once, and a +-1 change after at most
	 * PM_LIVE_MIN_S (sensor jitter would otherwise push every 2 s).  PF_PM
	 * below keeps the stock cloud report limits. */
	if (st.pm25_unknown ||
	    a >= g.pm_live + PM_LIVE_STEP || a + PM_LIVE_STEP <= g.pm_live ||
	    (a != g.pm_live && g.uptime_s - g.pm_live_s >= PM_LIVE_MIN_S)) {
		g.pm_live = a;
		g.pm_live_s = g.uptime_s;
		flag(PF_PM_LIVE);
	}
	st.pm25 = a;
	st.pm25_unknown = false;
	if (!g.aqi_reported || pur_aqi_report_due(a, g.aqi_rep)) {
		g.aqi_reported = true;
		g.aqi_rep = a;
		g.aqi_rep_s = g.uptime_s;
		flag(PF_PM);
	}
	/* 2.3 auto algorithm, with the 10 s hold restarted by every
	 * assignment (stock writes t_last even for an unchanged value).  It
	 * compares the unscaled target (auto_raw == target at bias 0). */
	uint16_t rpm;
	if (g.standby == STBY_ON && st.mode == MODE_AUTO &&
	    hal_millis() - g.t_auto >= AUTO_HOLD_MS &&
	    pur_auto_eval(a, g.auto_raw, &g.sp, g.goodh, &rpm)) {
		g.t_auto = hal_millis();
		set_auto_target(rpm);
	}
}

/* 1f012404.  `live` is false for a sensor tick replayed by every() after
 * an app-loop stall: no polling window passed since the previous tick, so
 * a missing frame there is not a failed read (otherwise one ~14 s stall
 * would count > 5 failures and raise the fault).  Stock polls the UART
 * inside each 2 s timer callback (1f00e828 from 1f012404), so it has no
 * separate frame window to miss. */
static void pm_read(bool live)
{
	if (g.frame) {
		aqi_update(g.frame_raw);
	} else if (live && ++g.pm_fail > PM_FAIL_MAX && !st.pm_fault) {
		st.pm_fault = true;            /* fault "no_sensor" */
		st.pm25 = PM_PLACEHOLDER;      /* 0x17, also what the panel shows */
		st.pm25_unknown = true;        /* HA: missing_state, not 23 */
		flag(PF_PM | PF_PM_LIVE | PF_PM_FAULT);
	}
}

static void pm_set(bool on)
{
	hal_pm_set_active(on);
}

/* 1f013adc state 0: read, count, stay awake while lit, else sleep once
 * more than 149 reads were taken.  The count is never reset by a wake
 * (only by get_prop "aqi" in stock), so after a warm-up the sensor is
 * read twice (14 s and 16 s) and sleeps again. */
static void sensor_tick(bool live)
{
	switch (g.pm_run) {
	case PM_RUN:
		pm_read(live);
		if (g.pm_idle < 0xffff)
			g.pm_idle++;
		if (st.led && st.bright != 0 && st.power)
			break;                 /* display lit: stay awake */
		if (g.pm_idle >= PM_IDLE_READS) {
			g.pm_run = PM_SLEEP;
			g.pm_cnt = 0;
			pm_set(false);
		}
		break;
	case PM_SLEEP:
		if (++g.pm_cnt >= (st.power ? PM_SLEEP_ON : PM_SLEEP_OFF)) {
			g.pm_run = PM_WARM;
			g.pm_cnt = 0;
			pm_set(true);
		}
		break;
	default:                               /* warm-up */
		if (++g.pm_cnt >= PM_WARMUP) {
			pm_read(live);
			g.pm_run = PM_RUN;
			g.pm_cnt = 0;
		}
		break;
	}
	g.frame = false;                       /* frames are per 2 s window */
}

/* ---------- SHT3x, light ---------- */

static void sht_read(void)
{
	int16_t t;
	uint8_t h;
	if (!hal_sht_poll(&t, &h))
		return;
	st.temp_dC = t;
	if (!g.temp_reported || t - g.temp_rep > 4 || g.temp_rep - t > 4) {
		g.temp_reported = true;
		g.temp_rep = t;
		flag(PF_TEMP);
	}
	if (h != st.humidity) {
		st.humidity = h;
		flag(PF_HUM);
	}
}

/* 200 ms light timer 1f013bb8 -> 1f012580; stock runs it only while
 * powered on (bright is frozen in standby). */
static void light_tick(void)
{
	uint16_t raw = hal_light_read();
	uint8_t b = pur_bright(raw);
	st.bright = b;
	/* push when |delta| > 19 and >= 300 samples since the last push */
	if (g.light_n < 0xffff)
		g.light_n++;
	if ((b - g.bright_rep > 19 || g.bright_rep - b > 19) &&
	    g.light_n >= BRIGHT_SAMPLES) {
		g.light_n = 0;
		g.bright_rep = b;
		flag(PF_BRIGHT);
	}
	/* lit and the sensor asleep: nudge it awake (1f012580) */
	if (b != 0 && st.power && st.led)
		pm_wake();
	g.light_pwm = pur_light_pwm(raw);
	g.pwm = pur_led_pwm(raw);
	/* OLED contrast from the unclamped value, steps of 16 when
	 * |delta| > 8 */
	int16_t c = pur_oled_contrast(g.light_pwm);
	int16_t d = (int16_t)(c - g.contrast);
	if (g.contrast < 0)
		g.contrast = c;
	else if (d > 8)
		g.contrast = (int16_t)(g.contrast + (d > 16 ? 16 : d));
	else if (d < -8)
		g.contrast = (int16_t)(g.contrast + (d < -16 ? -16 : d));
}

/* ---------- motor (2.4) ---------- */

/* 1f0128d0.  While the command equals the target, every 10 ticks (5 s)
 * is one FG window.  With target > 100 the first window only sets the
 * grace flag; a later window with 0 rpm is a stall -> standby 4 (fault
 * "m1_stuck").  Any command change (ramp step) clears the grace. */
static void motor_tick(void)
{
	uint16_t rpm = hal_fan_get_rpm();
	if (rpm != st.motor_rpm) {
		st.motor_rpm = rpm;
		flag(PF_RPM);
	}
	if (g.cmd == g.target) {
		if (++g.win_ticks >= MOTOR_WINDOW) {
			g.win_ticks = 0;
			if (g.target > 100) {
				if (!g.grace)
					g.grace = true;
				else if (rpm == 0 && g.standby == STBY_ON)
					go_standby(STBY_FAULT);
			}
		}
	} else {
		g.win_ticks = 0;
	}
	/* with an Auto speed bias the targets fall between table values,
	 * which stock's ramp would jump to; ramp through the table instead */
	uint16_t next = st.auto_bias && st.mode == MODE_AUTO ?
			pur_ramp_step_smooth(g.cmd, g.target) :
			pur_ramp_step(g.cmd, g.target);
	if (next != g.cmd)
		g.grace = false;
	motor_out(next);
}

/* ---------- 1 s power tick ---------- */

static void door_check(void)
{
	bool d = hal_door_open();
	if (d == st.door_open)
		return;
	st.door_open = d;
	flag(PF_DOOR);
	if (d) {                           /* 1f014388 */
		if (g.standby == STBY_ON)      /* event filter_door_opened */
			go_standby(STBY_OFF);  /* long beep, blank */
		else
			beep(BEEP_LONG);
	} else {
		beep(BEEP_DOUBLE);
		rfid_sync();
	}
}

/* Filter popups (1 s tick 1f0143fc, powered on, LED on). */
static void show_filter_popup(uint32_t ms)
{
	ui_show(SCR_FILTER_LOW, ms);
	if (!g.expired)
		g.popup_s = g.uptime_s;    /* 0x20000f88+4 */
}

static void filter_popups(void)
{
	if (!st.led)
		return;
	uint8_t cur = ui_current_screen(&st, false);
	uint32_t since_s = (hal_millis() - ui_screen_since()) / 1000;
	if (g.expired) {
		/* "replace": 60 s on, then again 180 s after the last draw */
		if (cur != SCR_FILTER_LOW && since_s >= FILTER_EXP_OFF_S)
			ui_show(SCR_FILTER_LOW, FILTER_EXP_ON_S * 1000u);
	} else if (st.filter_life < 5 &&
		   g.uptime_s == g.popup_s + FILTER_LOW_EVERY_S) {
		show_filter_popup(st.filter_life == 0 ? FILTER_POP0_MS :
				  FILTER_POP_MS);
	}
}

/* Not stock: an ambient-light value for HA (stock has no such property).
 * Same sensor scale as the display/LED brightness (pur_light_pwm), read
 * once a second also in standby; pushed on a change of >= 2 %. */
static void ambient_tick(void)
{
	uint8_t pct = pur_light_pct(hal_light_read());

	if (pct >= st.light + AMBIENT_STEP || pct + AMBIENT_STEP <= st.light) {
		st.light = pct;
		flag(PF_LIGHT);
	}
}

static void sec_tick(void)
{
	g.uptime_s++;
	door_check();
	if (g.standby == STBY_ON) {
		g.on_s++;
		if (g.aqi_state <= 1)
			g.t_low++;
		else if (g.aqi_state <= 3)
			g.t_mid++;
		else
			g.t_high++;
		/* every 300 s of powered-on time (counter 0x106e80) */
		if (g.on_s % AVG_PERIOD_S == 0) {      /* 3.2 */
			g.aqi_sum += g.aqi;
			g.aqi_cnt++;
			store_set(E_AQI_SUM, g.aqi_sum);
			store_set(E_AQI_CNT, g.aqi_cnt);
			mark_dirty();
			uint16_t avg = (uint16_t)(g.aqi_sum / g.aqi_cnt);
			if (avg != st.avg_pm25) {
				st.avg_pm25 = avg;
				flag(PF_PM);
			}
		}
		if (g.on_s % FILTER_PERIOD_S == 0) {   /* 4.2 */
			filter_refresh();
			uint8_t life = st.filter_life;
			if (!g.expired && life != g.life_seen &&
			    life >= 5 && life <= 10 && st.led)
				show_filter_popup(FILTER_POP_MS);
			g.life_seen = life;
			filter_fold();
			rfid_sync();
		}
		filter_popups();
	}
	filter_refresh();
	if (g.uptime_s - g.aqi_rep_s >= AQI_FORCE_S) {  /* 1f0143fc */
		g.aqi_rep = g.aqi;
		g.aqi_rep_s = g.uptime_s;
		flag(PF_PM);
	}
	if (g.uptime_s % SHT_PERIOD_S == 0)
		sht_read();
	/* hold counters: +1 per tick while a key is down (state 1 or 3) */
	for (int i = 0; i < 2; i++)
		if ((g.kst[i] == 1 || g.kst[i] == 3) && g.hold[i] < 0xff)
			g.hold[i]++;
	if (g.kst[0] == 3 && g.kst[1] == 3 && g.armed == 0 &&
	    g.hold[0] >= COMBO_ARM_TICKS && g.hold[1] >= COMBO_ARM_TICKS) {
		beep(BEEP_TRIPLE);
		g.armed = 2;
	}
	if (g.armed == 2 &&
	    ((g.kst[0] != 3 || g.kst[1] != 3) ||
	     (g.hold[0] >= COMBO_DATA_TICKS && g.hold[1] >= COMBO_DATA_TICKS)))
		g.armed = 1;
}

/* ---------- buttons (5.1, main loop 1f0159ec) ---------- */

static void cycle_mode(void)
{
	purifier_mode m = st.mode == MODE_AUTO ? MODE_SILENT :
			  st.mode == MODE_SILENT ? MODE_FAVORITE : MODE_AUTO;
	st.mode = m;
	g.set.mode = (uint8_t)m;
	flag(PF_MODE);
	mark_dirty();
	apply_mode();
}

/* Stock wakes the display to the main screen instead of acting when it
 * is dark or showing a filter screen (drawn screen 0x107458 test). */
static bool wake_instead(void)
{
	uint8_t cur = ui_current_screen(&st, false);
	return (!st.led && cur != SCR_MAIN && cur != SCR_MODE) ||
	       cur == SCR_NO_FILTER || cur == SCR_FILTER_LOW;
}

static void show_main(void)
{
	ui_clear_transient();
	if (!st.led)
		ui_wake(WAKE_MS);
}

/* Screen 3 handler: beep, 2.5 s mode splash, then main (10 s more if the
 * LED is off).  Not on main/splash: straight to main with the new icon. */
static void show_mode_change(void)
{
	uint8_t cur = ui_current_screen(&st, false);
	beep(BEEP_SINGLE);
	if (cur == SCR_MAIN || cur == SCR_MODE) {
		ui_show(SCR_MODE, SCR_MODE_MS);
		if (!st.led)
			ui_wake(SCR_MODE_MS + WAKE_MS);
	} else {
		show_main();
	}
}

/* standby loop: a key with the lock on or the door open */
static bool standby_refuse(void)
{
	if (st.child_lock) {
		ui_show(SCR_LOCK_ON, SCR_STBY_MS);
		return true;
	}
	if (st.door_open) {
		beep(BEEP_TRIPLE);
		ui_show(SCR_DOOR, SCR_STBY_MS);
		return true;
	}
	return false;
}

static void power_short(void)
{
	if (g.standby != STBY_ON) {
		if (!standby_refuse())
			power_on();
		return;
	}
	if (st.child_lock) {
		ui_show(SCR_LOCK_ON, SCR_MSG_MS);
		return;
	}
	if (wake_instead()) {
		show_main();
		return;
	}
	cycle_mode();
	show_mode_change();
}

static void led_short(void)
{
	if (g.standby != STBY_ON) {
		standby_refuse();          /* otherwise nothing in standby */
		return;
	}
	if (st.child_lock) {
		ui_show(SCR_LOCK_ON, SCR_MSG_MS);
		return;
	}
	purifier_set_led(!st.led);         /* beeps, screens */
}

static void led_hold(void)
{
	if (g.tag_valid) {
		ui_show(SCR_RESET_REFUSED, SCR_MSG_MS);
	} else {
		filter_reset();            /* event reset_filter_life */
		ui_show(SCR_RESET_OK, SCR_MSG_MS);
	}
	beep(BEEP_TRIPLE);
}

/* 1f013c4c */
static void combo_fire(void)
{
	bool data = g.hold[0] >= COMBO_DATA_TICKS &&
		    g.hold[1] >= COMBO_DATA_TICKS;
	g.armed = 0;
	g.kst[0] = g.kst[1] = 0;
	g.hold[0] = g.hold[1] = 0;
	if (data) {        /* DataResetSet: defaults written at next boot */
		store_set(E_DATA_RESET, 1);
		store_flush();
		beep(BEEP_TRIPLE);
	}
	st.child_lock = false;             /* 1f013c0c */
	g.set.child_lock = false;
	flag(PF_LOCK);
	save_now();
	ui_clear_transient();
	hal_display_power(false);
	hal_leds(0, 0, 0);
	hal_wifi_reset();
}

static void keys_tick(void)
{
	uint8_t raw = hal_keys() & (HAL_KEY_POWER | HAL_KEY_LED);
	if (raw != g.key_raw) {            /* two equal samples = stable */
		g.key_raw = raw;
		return;
	}
	uint8_t prev = g.key_stable, k = raw;
	g.key_stable = k;

	for (int i = 0; i < 2; i++) {
		uint8_t bit = i ? HAL_KEY_LED : HAL_KEY_POWER;
		int o = !i;
		if ((k & bit) && !(prev & bit)) {          /* press */
			if (g.kst[o] == 1) {
				g.kst[0] = g.kst[1] = 3;
				g.hold[o] = 0;
			} else {
				g.kst[i] = 1;
			}
			g.hold[i] = 0;
		} else if (!(k & bit) && (prev & bit)) {   /* release */
			bool shrt = g.kst[i] == 1;
			g.kst[i] = 0;
			if (shrt) {
				if (i == 0)
					power_short();
				else
					led_short();
			}
		}
	}
	/* holds act only while running (standby loop ignores them) */
	if (g.standby == STBY_ON && g.kst[0] == 1 &&
	    g.hold[0] >= POWER_HOLD_TICKS) {
		g.kst[0] = 0;
		g.hold[0] = 0;
		if (!st.child_lock)
			go_standby(STBY_OFF);
	}
	if (g.standby == STBY_ON && g.kst[1] == 1 &&
	    g.hold[1] >= LED_HOLD_TICKS) {
		g.kst[1] = 0;
		g.hold[1] = 0;
		led_hold();
	}
	if (g.armed == 1)
		combo_fire();
}

/* ---------- public API ---------- */

void purifier_init(void)
{
	memset(&st, 0, sizeof st);
	memset(&g, 0, sizeof g);
	ui_init();

	settings_load(&g.set);
	g.sp.silent = g.set.silent_rpm;
	g.sp.favorite = g.set.favorite_rpm;
	g.sp.low = (uint16_t)store_get(E_SPD_LOW);
	g.sp.med_l = (uint16_t)store_get(E_SPD_MED_L);
	g.sp.med = (uint16_t)store_get(E_SPD_MED);
	g.sp.high = (uint16_t)store_get(E_SPD_HIGH);
	g.sp.strong = (uint16_t)store_get(E_SPD_STRONG);
	g.goodh = (uint16_t)store_get(E_GOODH);
	g.f1_hour = store_get(E_F1_HOUR);
	g.aqi_sum = store_get(E_AQI_SUM);
	g.aqi_cnt = store_get(E_AQI_CNT);
	st.auto_bias = (int8_t)(int32_t)store_get(E_AUTO_BIAS); /* validated */

	if (g.set.mode != MODE_SILENT && g.set.mode != MODE_FAVORITE)
		g.set.mode = MODE_AUTO;    /* stock accepts <= 7 */
	st.mode = (purifier_mode)g.set.mode;
	st.favorite_level = 0xff;
	update_fav_level();
	st.led = g.set.led;
	st.buzzer = g.set.buzzer;
	st.avg_pm25 = g.aqi_cnt ? (uint16_t)(g.aqi_sum / g.aqi_cnt) : 0;
	st.pm25 = g.aqi = AQI_BOOT;
	st.pm25_unknown = true;            /* boot placeholder: no reading yet */

	uint32_t now = hal_millis();
	g.t_light = g.t_motor = g.t_sec = g.t_sensor = now;
	g.t_auto = now - AUTO_HOLD_MS;
	g.saved_t = now;
	g.contrast = -1;
	g.standby = STBY_OFF;
	g.pm_run = PM_RUN;
	g.pm_idle = PM_IDLE_READS;     /* 1f0150d8: +0x38 = 0x96 */
	g.bright_rep = pur_bright(hal_light_read());
	st.light = pur_light_pct(hal_light_read());

	hal_fan_set_rpm(0);
	pm_set(true);
	st.door_open = hal_door_open();
	light_tick();
	sht_read();
	filter_refresh();
	g.life_seen = st.filter_life;

	/* XMAIR_Init 1f0150d8: after a soft reset (PMU cause 8), or always
	 * for country 91 with slave_channel 3, the stored power state is
	 * restored (with the power-on beep) and child lock is kept.  A cold
	 * boot comes up off and clears child lock. */
	bool warm = hal_reset_was_soft() ||
		    (store_get(E_COUNTRY) == 91 && store_get(E_SLAVE_CHAN) == 3);
	if (warm) {
		st.child_lock = g.set.child_lock;
	} else if (g.set.child_lock) {
		g.set.child_lock = false;
		mark_dirty();
	}
	if (warm && g.set.power_restore && !st.door_open)
		power_on();
	else
		rfid_sync();
	st.changed = (PF_LAST << 1) - 1;       /* push everything once */
}

void purifier_tick(void)
{
	uint32_t now = hal_millis();
	uint16_t raw;

	if (hal_pm_poll(&raw) && !g.frame) {   /* first frame of the window */
		g.frame = true;
		g.frame_raw = raw;
	}
	keys_tick();
	while (every(&g.t_light, T_LIGHT_MS, now))
		if (st.power)              /* 1f013bb8: only while on */
			light_tick();
	while (every(&g.t_motor, T_MOTOR_MS, now))
		motor_tick();
	while (every(&g.t_sec, T_SEC_MS, now)) {
		sec_tick();
		ambient_tick();
	}
	/* only the first sensor tick of this call had a polling window; the
	 * rest (catch-up after a stall) keep the sleep/warm-up timing but
	 * cannot fail a read (pm_read) */
	for (bool live = true; every(&g.t_sensor, T_SENSOR_MS, now); live = false)
		sensor_tick(live);

	if (g.dirty && now - g.saved_t >= SAVE_MIN_GAP_MS &&
	    (now - g.dirty_last >= SAVE_DEBOUNCE_MS ||
	     now - g.dirty_first >= SAVE_MAX_DELAY_MS))
		save_now();

	ui_view v = { &st, g.aqi_state, st.pm_fault };
	ui_tick(&v, g.standby == STBY_FAULT, g.pwm,
		(uint8_t)(g.contrast < 0 ? 1 : g.contrast));
}

const purifier_state *purifier_get(void)
{
	return &st;
}

void purifier_flush(void)
{
	filter_fold();
	if (g.dirty)
		save_now();
}

uint32_t purifier_take_changes(void)
{
	uint32_t c = st.changed;
	st.changed = 0;
	return c;
}

int purifier_set_power(bool on)
{
	if (st.door_open)
		return -6003;
	if (on && g.standby != STBY_ON)
		power_on();
	else if (!on && g.standby != STBY_OFF)
		go_standby(STBY_OFF);
	return 0;
}

int purifier_set_mode(purifier_mode m)
{
	if (st.door_open)
		return -6003;
	if (m != MODE_AUTO && m != MODE_SILENT && m != MODE_FAVORITE)
		return -5001;
	bool changed = m != st.mode;
	if (changed) {
		st.mode = m;
		g.set.mode = (uint8_t)m;
		flag(PF_MODE);
		mark_dirty();
	}
	/* 1f012e70: off -> power on; on and changed -> wake to main with a
	 * beep, or the mode splash (which beeps) */
	if (g.standby != STBY_ON) {
		power_on();
	} else if (changed) {
		apply_mode();
		if (wake_instead()) {
			beep(BEEP_SINGLE);
			show_main();
		} else {
			show_mode_change();
		}
	}
	return 0;
}

int purifier_set_favorite_level(uint8_t level)
{
	if (level >= FAVORITE_LEVELS)
		return -5001;
	uint16_t rpm = pur_fav_table[level];
	if (rpm != g.set.favorite_rpm) {
		g.set.favorite_rpm = rpm;
		g.sp.favorite = rpm;
		mark_dirty();
	}
	update_fav_level();
	if (g.standby == STBY_ON && st.mode == MODE_FAVORITE)
		set_target(rpm);
	beep(BEEP_SINGLE);
	return 0;
}

int purifier_set_led(bool on)
{
	if (on != st.led) {
		st.led = on;
		g.set.led = on;
		flag(PF_LED);
		mark_dirty();
	}
	/* 1f013298: beep; while on, LED on -> main (+ sensor nudge), LED off
	 * -> screen 0 */
	beep(BEEP_SINGLE);
	if (st.power) {
		ui_clear_transient();
		if (on)
			pm_wake();
	}
	return 0;
}

int purifier_set_buzzer(bool on)
{
	if (on != st.buzzer) {
		st.buzzer = on;
		g.set.buzzer = on;
		flag(PF_BUZZER);
		mark_dirty();
	}
	/* 1f013564: "on" -> AIR_Var_BuzzerSet(1) 1f017878, then a single
	 * beep (1f00d908, gated by the new setting); "off" does not beep */
	if (on)
		beep(BEEP_SINGLE);
	return 0;
}

int purifier_set_child_lock(bool on)
{
	if (on != st.child_lock) {
		st.child_lock = on;
		g.set.child_lock = on;
		flag(PF_LOCK);
		mark_dirty();
	}
	/* 1f013490: beep + screen 0xe/0xf for 4 s; the standby loop does
	 * not service the display, so nothing is drawn while off */
	if (st.power)
		ui_show(on ? SCR_LOCK_ON : SCR_LOCK_OFF, SCR_MSG_MS);
	beep(BEEP_SINGLE);
	return 0;
}

int purifier_reset_filter(void)
{
	if (g.tag_valid)               /* 4.3: no local reset of a tag */
		return -5001;
	filter_reset();
	return 0;
}

/* Ours, not stock.  No beep (a config value; an HA slider would chirp on
 * every step).  In Auto the new bias applies at once to the current
 * stock target; the auto hold and hysteresis are untouched. */
int purifier_set_auto_bias(int bias)
{
	if (bias < AUTO_BIAS_MIN || bias > AUTO_BIAS_MAX ||
	    bias % AUTO_BIAS_STEP != 0)
		return -5001;
	if (bias != st.auto_bias) {
		st.auto_bias = (int8_t)bias;
		store_set(E_AUTO_BIAS, (uint32_t)(int32_t)bias);
		flag(PF_AUTO_BIAS);
		mark_dirty();
	}
	if (g.standby == STBY_ON && st.mode == MODE_AUTO)
		set_target(pur_auto_bias_rpm(g.auto_raw, st.auto_bias));
	return 0;
}
