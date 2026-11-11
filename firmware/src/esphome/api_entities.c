/* api_entities.c - the entities exposed to Home Assistant, built from
 * purifier.h and diag.h.  Encodes ListEntities*Response / *StateResponse
 * messages and dispatches Fan / Switch / Button / Number /
 * SelectCommandRequest to purifier_set_*().  Also the diagnostics (U2):
 * periodic uptime / heap pushes and the deferred Restart button, both run
 * from api_entity_poll().  Field numbers: api.proto @ esphome 2026.10.2. */
#include <math.h>
#include <string.h>
#include "api_internal.h"
#include "diag.h"
#include "hal.h"
#include "netio.h"
#include "ota.h"
#include "purifier.h"

enum { T_FAN, T_SENSOR, T_SWITCH, T_BUTTON, T_BINARY, T_NUMBER, T_SELECT, T_TEXT };

/* Diagnostics push policy (all pushes go through the 1/s sensor throttle
 * in api_server.c as well).  Uptime: every DIAG_PERIOD_MS.  Free heap /
 * min free heap: checked every DIAG_PERIOD_MS, pushed only if they moved
 * by >= HEAP_DELTA since the last push.  IP: checked every DIAG_FAST_MS,
 * pushed on change.  OTA slot: re-read (flash) every DIAG_PERIOD_MS only
 * while the image is an unconfirmed trial, pushed when that ends. */
#define DIAG_PERIOD_MS   60000u
#define DIAG_FAST_MS     1000u
#define HEAP_DELTA       1024u
#define RESTART_DELAY_MS 500u      /* lets the log line / TCP ACKs go out */

typedef struct {
	uint8_t type;
	uint8_t category;
	uint8_t state_class;
	uint8_t accuracy;
	const char *object_id;
	const char *name;
	const char *icon;
	const char *device_class;
	const char *unit;
	uint32_t pf;               /* PF_* flags that change this entity */
} entity_def;

/* Order must match the E_* enum in api_internal.h. */
static const entity_def ents[E_COUNT] = {
	/* name "" = the device's main entity: HA names it after the device */
	[E_FAN] = { T_FAN, ENTITY_CATEGORY_NONE, 0, 0, "purifier", "",
		    "mdi:air-purifier", NULL, NULL, PF_POWER | PF_MODE | PF_FAV },
	/* The fan's speed slider and presets, as first-class controls on the
	 * HA device page (HA shows only the fan's toggle there).  Fan level =
	 * the Favorite level 1..15, shown in every mode. */
	[E_FAN_LEVEL] = { T_NUMBER, ENTITY_CATEGORY_NONE, 0, 0, "fan_level", "Fan level",
			  "mdi:fan", NULL, NULL, PF_FAV },
	[E_MODE] = { T_SELECT, ENTITY_CATEGORY_NONE, 0, 0, "mode", "Mode",
		     "mdi:fan-auto", NULL, NULL, PF_MODE },
	/* PF_PM_LIVE: every new value; PF_PM (stock report limits, 30 min
	 * force) only re-sends the same live value */
	[E_PM25] = { T_SENSOR, ENTITY_CATEGORY_NONE, STATE_CLASS_MEASUREMENT, 0,
		     "pm25", "PM2.5", NULL, "pm25", "\xc2\xb5g/m\xc2\xb3",
		     PF_PM | PF_PM_LIVE },
	[E_TEMP] = { T_SENSOR, ENTITY_CATEGORY_NONE, STATE_CLASS_MEASUREMENT, 1,
		     "temperature", "Temperature", NULL, "temperature",
		     "\xc2\xb0" "C", PF_TEMP },
	[E_HUM] = { T_SENSOR, ENTITY_CATEGORY_NONE, STATE_CLASS_MEASUREMENT, 0,
		    "humidity", "Humidity", NULL, "humidity", "%", PF_HUM },
	[E_RPM] = { T_SENSOR, ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_MEASUREMENT, 0,
		    "fan_speed", "Fan speed", "mdi:fan", NULL, "rpm", PF_RPM },
	[E_FILTER_LIFE] = { T_SENSOR, ENTITY_CATEGORY_NONE, STATE_CLASS_MEASUREMENT, 0,
			    "filter_life", "Filter life", "mdi:air-filter", NULL, "%",
			    PF_FILTER },
	[E_FILTER_HOURS] = { T_SENSOR, ENTITY_CATEGORY_DIAGNOSTIC,
			     STATE_CLASS_TOTAL_INCREASING, 0, "filter_hours",
			     "Filter used", "mdi:timer-sand", "duration", "h", PF_FILTER },
	[E_RSSI] = { T_SENSOR, ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_MEASUREMENT, 0,
		     "wifi_signal", "WiFi signal", NULL, "signal_strength", "dBm", 0 },
	/* not stock: the light sensor behind the auto display brightness */
	[E_LIGHT] = { T_SENSOR, ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_MEASUREMENT, 0,
		      "ambient_light", "Ambient light", "mdi:brightness-5", NULL, "%",
		      PF_LIGHT },
	[E_LED] = { T_SWITCH, ENTITY_CATEGORY_CONFIG, 0, 0, "display_led", "Display LED",
		    "mdi:led-on", NULL, NULL, PF_LED },
	[E_BUZZER] = { T_SWITCH, ENTITY_CATEGORY_CONFIG, 0, 0, "buzzer", "Buzzer",
		       "mdi:volume-high", NULL, NULL, PF_BUZZER },
	[E_LOCK] = { T_SWITCH, ENTITY_CATEGORY_CONFIG, 0, 0, "child_lock", "Child lock",
		     "mdi:lock", NULL, NULL, PF_LOCK },
	[E_DOOR] = { T_BINARY, ENTITY_CATEGORY_NONE, 0, 0, "filter_door", "Filter door",
		     NULL, "door", NULL, PF_DOOR },
	/* stock faults "m1_stuck" (standby 4) and "no_sensor" */
	[E_MOTOR_FAULT] = { T_BINARY, ENTITY_CATEGORY_DIAGNOSTIC, 0, 0, "motor_fault",
			    "Motor fault", NULL, "problem", NULL, PF_MOTOR_FAULT },
	[E_PM_FAULT] = { T_BINARY, ENTITY_CATEGORY_DIAGNOSTIC, 0, 0, "pm_sensor_fault",
			 "PM sensor fault", NULL, "problem", NULL, PF_PM_FAULT },
	[E_RESET_FILTER] = { T_BUTTON, ENTITY_CATEGORY_CONFIG, 0, 0, "reset_filter",
			     "Reset filter", "mdi:air-filter", NULL, NULL, 0 },
	/* not stock: scales the stock Auto rpm by (100 + bias) %, 0 = stock */
	[E_AUTO_BIAS] = { T_NUMBER, ENTITY_CATEGORY_CONFIG, 0, 0, "auto_bias",
			  "Auto speed bias", "mdi:tune-variant", NULL, "%",
			  PF_AUTO_BIAS },
	/* ---- diagnostics (U1/U2); never the MAC, DID, token or keys ---- */
	[E_FIRMWARE] = { T_TEXT, ENTITY_CATEGORY_DIAGNOSTIC, 0, 0, "firmware",
			 "Firmware", "mdi:chip", NULL, NULL, 0 },
	[E_RESET_REASON] = { T_TEXT, ENTITY_CATEGORY_DIAGNOSTIC, 0, 0, "reset_reason",
			     "Reset reason", "mdi:restart-alert", NULL, NULL, 0 },
	[E_OTA_SLOT] = { T_TEXT, ENTITY_CATEGORY_DIAGNOSTIC, 0, 0, "ota_slot",
			 "OTA slot", "mdi:swap-horizontal", NULL, NULL, 0 },
	[E_IP] = { T_TEXT, ENTITY_CATEGORY_DIAGNOSTIC, 0, 0, "ip_address",
		   "IP address", "mdi:ip-network", NULL, NULL, 0 },
	[E_UPTIME] = { T_SENSOR, ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_TOTAL_INCREASING,
		       0, "uptime", "Uptime", "mdi:timer-outline", "duration", "s", 0 },
	[E_HEAP] = { T_SENSOR, ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_MEASUREMENT, 0,
		     "free_heap", "Free heap", "mdi:memory", "data_size", "B", 0 },
	/* lowest free heap seen at the API poll rate (10 ms) since the API
	 * server started (heap_marvell keeps no min-ever counter in this build) */
	[E_HEAP_MIN] = { T_SENSOR, ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_MEASUREMENT, 0,
			 "min_free_heap", "Min free heap", "mdi:memory", "data_size", "B", 0 },
	[E_RESTART] = { T_BUTTON, ENTITY_CATEGORY_DIAGNOSTIC, 0, 0, "restart", "Restart",
			NULL, "restart", NULL, 0 },
};

/* T_NUMBER ranges: min, max, step */
static void number_range(unsigned idx, float r[3])
{
	if (idx == E_AUTO_BIAS) {
		r[0] = AUTO_BIAS_MIN;
		r[1] = AUTO_BIAS_MAX;
		r[2] = AUTO_BIAS_STEP;
	} else {                   /* E_FAN_LEVEL */
		r[0] = 1.0f;
		r[1] = (float)FAVORITE_LEVELS;
		r[2] = 1.0f;
	}
}

static const char *const presets[] = { "Auto", "Silent", "Favorite" };
static const purifier_mode preset_modes[] = { MODE_AUTO, MODE_SILENT, MODE_FAVORITE };
#define N_PRESETS 3

static uint32_t keys[E_COUNT];

/* diagnostics state (app task only) */
static struct {
	uint8_t restart;           /* Restart pressed, reboot pending */
	uint8_t slot_trial;        /* slot text shows a trial: keep re-reading */
	uint32_t slow_ms, fast_ms, restart_ms;
	uint32_t up_sent, heap_sent, min_sent, heap_min, ip;
	char slot[12];
} dg;

static void diag_read_slot(void)
{
	ota_info_t in;
	if (ota_get_info(&in) != OTA_OK || (in.running != 'A' && in.running != 'B'))
		return;            /* keep the last good value (or "") */
	dg.slot[0] = in.running;
	dg.slot[1] = 0;
	if (in.trial)
		strcpy(dg.slot + 1, " (trial)");
	dg.slot_trial = in.trial;
}

static void diag_reset_state(uint32_t now)
{
	memset(&dg, 0, sizeof(dg));
	diag_init();
	dg.slow_ms = dg.fast_ms = now;
	dg.heap_min = dg.heap_sent = dg.min_sent = diag_free_heap();
	dg.up_sent = diag_uptime_s();
	dg.ip = netio_local_ip_be();
	diag_read_slot();
}

/* Why Restart is not allowed now, NULL = allowed. */
static const char *restart_refusal(void)
{
	ota_info_t in;
	ota_get_info(&in);         /* in_progress / update_pending are set even on error */
	if (in.in_progress)
		return "restart refused: OTA upload in progress";
	if (in.update_pending)
		return "restart refused: OTA update committed, the OTA server reboots into it";
	/* Any reset of an unconfirmed trial boots the previous image; the
	 * trial confirms or reverts by itself within 10 min, so a Restart
	 * would only lose the new image by accident. */
	if (ota_trial_revert_armed())
		return "restart refused: unconfirmed OTA trial (a reset reverts it); "
		       "confirm it or wait for the timeout";
	return NULL;
}

uint32_t api_entity_poll(uint32_t now)
{
	uint32_t bits = 0, heap = diag_free_heap();

	if (heap < dg.heap_min)
		dg.heap_min = heap;
	if (dg.restart && now - dg.restart_ms >= RESTART_DELAY_MS) {
		const char *why = restart_refusal();   /* re-check: an upload may have begun */
		dg.restart = 0;
		if (why) {
			api_log(API_LOG_WARN, why);
		} else {
			diag_set_reboot_tag(DIAG_REBOOT_RESTART);
			hal_reboot();      /* hal_reboot_hook(): purifier_flush(); no return on device */
		}
	}
	if (now - dg.fast_ms >= DIAG_FAST_MS) {
		uint32_t ip = netio_local_ip_be();
		dg.fast_ms = now;
		if (ip != dg.ip) {
			dg.ip = ip;
			bits |= 1u << E_IP;
		}
	}
	if (now - dg.slow_ms >= DIAG_PERIOD_MS) {
		uint32_t up = diag_uptime_s();
		dg.slow_ms = now;
		if (up != dg.up_sent)
			bits |= 1u << E_UPTIME;
		if ((heap > dg.heap_sent ? heap - dg.heap_sent : dg.heap_sent - heap) >= HEAP_DELTA)
			bits |= 1u << E_HEAP;
		if (dg.min_sent - dg.heap_min >= HEAP_DELTA)
			bits |= 1u << E_HEAP_MIN;
		if (dg.slot_trial) {
			diag_read_slot();
			if (!dg.slot_trial)
				bits |= 1u << E_OTA_SLOT;
		}
	}
	return bits;
}

/* ESPHome: key = fnv1_hash(object_id) (esphome/core/helpers.cpp) */
static uint32_t fnv1(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s) {
		h *= 16777619u;
		h ^= (uint8_t)*s++;
	}
	return h;
}

void api_entities_init(void)
{
	for (unsigned i = 0; i < E_COUNT; i++)
		keys[i] = fnv1(ents[i].object_id);
	diag_reset_state(hal_millis());
}

uint32_t api_entity_key(unsigned idx) { return idx < E_COUNT ? keys[idx] : 0; }
const char *api_entity_object_id(unsigned idx)
{
	return idx < E_COUNT ? ents[idx].object_id : "";
}

static int find_key(uint32_t key, uint8_t type)
{
	for (unsigned i = 0; i < E_COUNT; i++)
		if (keys[i] == key && ents[i].type == type)
			return (int)i;
	return -1;
}

uint32_t api_entity_throttled_mask(void)
{
	uint32_t m = 0;
	for (unsigned i = 0; i < E_COUNT; i++)
		if (ents[i].type == T_SENSOR)
			m |= 1u << i;
	return m;
}

uint32_t api_entity_stateful_mask(void)
{
	return E_ALL_MASK & ~((1u << E_RESET_FILTER) | (1u << E_RESTART));
}

uint32_t api_entity_dirty_from_pf(uint32_t pf)
{
	uint32_t m = 0;
	for (unsigned i = 0; i < E_COUNT; i++)
		if (ents[i].pf & pf)
			m |= 1u << i;
	/* api_server.c calls this once per api_server_poll() (track_changes),
	 * which makes it the app-loop hook for the diagnostics too. */
	return m | api_entity_poll(hal_millis());
}

uint16_t api_entity_encode_list(unsigned idx, pb_writer *w)
{
	const entity_def *e = &ents[idx];
	/* common prefix: object_id = 1, key = 2, name = 3 */
	pb_w_string(w, 1, e->object_id);
	pb_w_fixed32_force(w, 2, keys[idx]);
	pb_w_string(w, 3, e->name);
	switch (e->type) {
	case T_FAN:
		pb_w_bool(w, 6, true);                  /* supports_speed */
		pb_w_int32(w, 8, FAVORITE_LEVELS);      /* supported_speed_count */
		pb_w_string(w, 10, e->icon);
		pb_w_uint32(w, 11, e->category);
		for (unsigned i = 0; i < N_PRESETS; i++)
			pb_w_string(w, 12, presets[i]); /* supported_preset_modes */
		return MSG_LIST_FAN_RESP;
	case T_SENSOR:
		pb_w_string(w, 5, e->icon);
		pb_w_string(w, 6, e->unit);
		pb_w_int32(w, 7, e->accuracy);
		pb_w_string(w, 9, e->device_class);
		pb_w_uint32(w, 10, e->state_class);
		pb_w_uint32(w, 13, e->category);
		return MSG_LIST_SENSOR_RESP;
	case T_SWITCH:
		pb_w_string(w, 5, e->icon);
		pb_w_uint32(w, 8, e->category);
		pb_w_string(w, 9, e->device_class);
		return MSG_LIST_SWITCH_RESP;
	case T_BUTTON:
		pb_w_string(w, 5, e->icon);
		pb_w_uint32(w, 7, e->category);
		pb_w_string(w, 8, e->device_class);
		return MSG_LIST_BUTTON_RESP;
	case T_BINARY:
		pb_w_string(w, 5, e->device_class);
		pb_w_string(w, 8, e->icon);
		pb_w_uint32(w, 9, e->category);
		return MSG_LIST_BINARY_SENSOR_RESP;
	case T_NUMBER: {
		float rg[3];
		number_range(idx, rg);
		pb_w_string(w, 5, e->icon);
		pb_w_float(w, 6, rg[0]);                /* min_value */
		pb_w_float(w, 7, rg[1]);                /* max_value */
		pb_w_float(w, 8, rg[2]);                /* step */
		pb_w_uint32(w, 10, e->category);
		pb_w_string(w, 11, e->unit);
		pb_w_uint32(w, 12, NUMBER_MODE_SLIDER);
		pb_w_string(w, 13, e->device_class);
		return MSG_LIST_NUMBER_RESP;
	}
	case T_SELECT:
		pb_w_string(w, 5, e->icon);
		for (unsigned i = 0; i < N_PRESETS; i++)
			pb_w_string(w, 6, presets[i]);  /* options */
		pb_w_uint32(w, 8, e->category);
		return MSG_LIST_SELECT_RESP;
	case T_TEXT:
		pb_w_string(w, 5, e->icon);
		pb_w_uint32(w, 7, e->category);
		pb_w_string(w, 8, e->device_class);
		return MSG_LIST_TEXT_SENSOR_RESP;
	}
	return 0;
}

/* dotted quad of a network-order IPv4 address */
static void ip_str(uint32_t ip_be, char out[16])
{
	uint8_t b[4];
	char *d = out, *end = out + 15;
	memcpy(b, &ip_be, 4);
	for (int i = 0; i < 4; i++) {
		if (i)
			*d++ = '.';
		d = api_itoa_end(d, end, b[i]);
	}
	*d = 0;
}

static const char *text_state(unsigned idx, char buf[16])
{
	switch (idx) {
	case E_FIRMWARE: return diag_fw_version();
	case E_RESET_REASON: return diag_reset_reason();
	case E_OTA_SLOT: return dg.slot;
	case E_IP:
		if (!dg.ip)
			return "";
		ip_str(dg.ip, buf);
		return buf;
	}
	return "";
}

static const char *preset_name(purifier_mode m)
{
	for (unsigned i = 0; i < N_PRESETS; i++)
		if (preset_modes[i] == m)
			return presets[i];
	return "";
}

/* Favorite level as HA sees it, 1..FAVORITE_LEVELS (15 = custom rpm is
 * clamped, as for the fan's speed_level). */
static int32_t fav_level_1(const purifier_state *s)
{
	return (s->favorite_level < FAVORITE_LEVELS ? s->favorite_level :
		FAVORITE_LEVELS - 1) + 1;
}

uint16_t api_entity_encode_state(unsigned idx, pb_writer *w)
{
	const purifier_state *s = purifier_get();
	const entity_def *e = &ents[idx];
	float v = 0;
	bool missing = false;

	pb_w_fixed32_force(w, 1, keys[idx]);
	switch (e->type) {
	case T_FAN:
		pb_w_bool(w, 2, s->power);
		/* HA convention: percentage only while a fixed speed is in use
		 * (Favorite); Auto/Silent are presets with no percentage. */
		if (s->mode == MODE_FAVORITE)
			pb_w_int32(w, 6, fav_level_1(s));
		pb_w_string(w, 7, preset_name(s->mode));
		return MSG_FAN_STATE;
	case T_NUMBER:
		/* Fan level: always the Favorite level, also in Auto/Silent.
		 * Auto bias 0 is omitted (proto3 default 0.0), as ESPHome does. */
		pb_w_float(w, 2, idx == E_AUTO_BIAS ? (float)s->auto_bias :
				 (float)fav_level_1(s));
		return MSG_NUMBER_STATE;
	case T_SELECT: {
		const char *p = preset_name(s->mode);
		pb_w_string(w, 2, p);
		pb_w_bool(w, 3, !*p);                   /* missing_state */
		return MSG_SELECT_STATE;
	}
	case T_SWITCH:
		pb_w_bool(w, 2, idx == E_LED ? s->led : idx == E_BUZZER ? s->buzzer :
				s->child_lock);
		return MSG_SWITCH_STATE;
	case T_TEXT: {
		char buf[16];
		const char *t = text_state(idx, buf);
		pb_w_string(w, 2, t);
		pb_w_bool(w, 3, !*t);                   /* missing_state */
		return MSG_TEXT_SENSOR_STATE;
	}
	case T_BINARY:
		pb_w_bool(w, 2, idx == E_DOOR ? s->door_open :
				idx == E_MOTOR_FAULT ? s->motor_fault : s->pm_fault);
		return MSG_BINARY_SENSOR_STATE;
	case T_SENSOR:
		switch (idx) {
		case E_PM25:
			/* placeholder (boot 20, fault 23): no value, as ESPHome
			 * sends a sensor without a state (NaN + missing_state) */
			v = s->pm25_unknown ? NAN : s->pm25;
			missing = s->pm25_unknown;
			break;
		case E_TEMP: v = s->temp_dC / 10.0f; break;
		case E_HUM: v = s->humidity; break;
		case E_RPM: v = s->motor_rpm; break;
		case E_FILTER_LIFE: v = s->filter_life; break;
		case E_FILTER_HOURS: v = (float)s->filter_hours; break;
		case E_LIGHT: v = s->light; break;
		case E_RSSI: {
			int r = netio_rssi();
			v = (float)r;
			missing = r == 0;
			break;
		}
		/* the value sent becomes the reference for the next delta */
		case E_UPTIME: dg.up_sent = diag_uptime_s(); v = (float)dg.up_sent; break;
		case E_HEAP: {
			uint32_t h = diag_free_heap();
			if (h < dg.heap_min)
				dg.heap_min = h;
			dg.heap_sent = h;
			v = (float)h;
			break;
		}
		case E_HEAP_MIN: dg.min_sent = dg.heap_min; v = (float)dg.heap_min; break;
		}
		pb_w_float(w, 2, v);
		pb_w_bool(w, 3, missing);
		return MSG_SENSOR_STATE;
	}
	return 0;
}

static void log_err(const char *what, int rc)
{
	if (rc)
		api_log_int(API_LOG_WARN, what, rc);
}

/* Entities a power/level/mode command can change: all re-sent, so HA
 * gets the real state back promptly, also after a rejected command. */
#define FAN_CONTROLS ((1u << E_FAN) | (1u << E_FAN_LEVEL) | (1u << E_MODE))

static uint32_t fan_command(const uint8_t *data, size_t len)
{
	pb_reader r;
	pb_field f;
	uint32_t key = 0;
	bool has_state = false, state = false, has_level = false, has_preset = false;
	int32_t level = 0;
	pb_field preset = { 0 };
	int rc;

	pb_reader_init(&r, data, len);
	while ((rc = pb_next(&r, &f)) > 0) {
		switch (f.field) {
		case 1: key = (uint32_t)f.u64; break;
		case 2: has_state = f.u64 != 0; break;
		case 3: state = f.u64 != 0; break;
		case 10: has_level = f.u64 != 0; break;
		case 11: level = (int32_t)f.u64; break;
		case 12: has_preset = f.u64 != 0; break;
		case 13: preset = f; break;
		}
	}
	if (rc < 0 || find_key(key, T_FAN) < 0)
		return 0;

	if (has_state && !state) {
		log_err("fan off rejected", purifier_set_power(false));
		return FAN_CONTROLS;
	}
	if (has_level && level <= 0) {         /* HA: percentage 0 = off */
		log_err("fan off rejected", purifier_set_power(false));
		return FAN_CONTROLS;
	}
	if (has_state && state && !purifier_get()->power)
		log_err("fan on rejected", purifier_set_power(true));
	if (has_preset) {
		for (unsigned i = 0; i < N_PRESETS; i++)
			if (pb_field_eq(&preset, presets[i]))
				log_err("preset rejected", purifier_set_mode(preset_modes[i]));
	}
	if (has_level) {
		if (level > FAVORITE_LEVELS)
			level = FAVORITE_LEVELS;
		/* a speed means "fixed speed": the Favorite mode at that level */
		log_err("level rejected", purifier_set_favorite_level((uint8_t)(level - 1)));
		if (purifier_get()->mode != MODE_FAVORITE)
			log_err("mode rejected", purifier_set_mode(MODE_FAVORITE));
	}
	return FAN_CONTROLS;
}

/* The fan's "speed" semantics: a level means the Favorite mode at that
 * level.  purifier_set_mode() powers the unit on when it is off (stock
 * 1f012e70), like HA's set_percentage (which also sends state=true). */
static void set_fixed_level(int32_t level)
{
	log_err("level rejected", purifier_set_favorite_level((uint8_t)(level - 1)));
	log_err("mode rejected", purifier_set_mode(MODE_FAVORITE));
}

static uint32_t number_command(const uint8_t *data, size_t len)
{
	pb_reader r;
	pb_field f;
	uint32_t key = 0;
	float v = 0.0f;                 /* proto3: an absent float is 0.0 */
	int rc;

	pb_reader_init(&r, data, len);
	while ((rc = pb_next(&r, &f)) > 0) {
		if (f.field == 1)
			key = (uint32_t)f.u64;
		else if (f.field == 2 && f.wt == PB_WT_I32) {
			uint32_t u = (uint32_t)f.u64;
			memcpy(&v, &u, 4);
		}
	}
	int idx = rc < 0 ? -1 : find_key(key, T_NUMBER);
	if (idx == E_AUTO_BIAS) {
		if (v == v) {           /* NaN: nothing to do, re-send */
			if (v < (float)AUTO_BIAS_MIN)
				v = (float)AUTO_BIAS_MIN;
			if (v > (float)AUTO_BIAS_MAX)
				v = (float)AUTO_BIAS_MAX;
			/* nearest step, halves away from zero */
			float q = v / AUTO_BIAS_STEP;
			int32_t n = (int32_t)(q < 0 ? q - 0.5f : q + 0.5f);
			log_err("auto bias rejected",
				purifier_set_auto_bias(n * AUTO_BIAS_STEP));
		}
		return 1u << E_AUTO_BIAS;
	}
	if (idx != E_FAN_LEVEL)
		return 0;
	if (v != v)
		return FAN_CONTROLS;    /* NaN: nothing to do, re-send */
	/* clamp before converting (float -> int of a huge value is UB) */
	if (v < 1.0f)
		v = 1.0f;
	if (v > (float)FAVORITE_LEVELS)
		v = (float)FAVORITE_LEVELS;
	set_fixed_level((int32_t)(v + 0.5f));
	return FAN_CONTROLS;
}

static uint32_t select_command(const uint8_t *data, size_t len)
{
	pb_reader r;
	pb_field f;
	uint32_t key = 0;
	pb_field opt = { 0 };
	int rc;

	pb_reader_init(&r, data, len);
	while ((rc = pb_next(&r, &f)) > 0) {
		if (f.field == 1)
			key = (uint32_t)f.u64;
		else if (f.field == 2 && f.wt == PB_WT_LEN)
			opt = f;
	}
	if (rc < 0 || find_key(key, T_SELECT) != E_MODE)
		return 0;
	for (unsigned i = 0; i < N_PRESETS; i++)
		if (pb_field_eq(&opt, presets[i]))
			log_err("mode rejected", purifier_set_mode(preset_modes[i]));
	return FAN_CONTROLS;
}

uint32_t api_entity_command(uint16_t type, const uint8_t *data, size_t len)
{
	pb_reader r;
	pb_field f;
	uint32_t key = 0;
	bool state = false;
	int idx;

	if (type == MSG_FAN_CMD)
		return fan_command(data, len);
	if (type == MSG_NUMBER_CMD)
		return number_command(data, len);
	if (type == MSG_SELECT_CMD)
		return select_command(data, len);

	pb_reader_init(&r, data, len);
	while (pb_next(&r, &f) > 0) {
		if (f.field == 1)
			key = (uint32_t)f.u64;
		else if (f.field == 2)
			state = f.u64 != 0;
	}
	if (type == MSG_SWITCH_CMD) {
		idx = find_key(key, T_SWITCH);
		if (idx == E_LED)
			log_err("led rejected", purifier_set_led(state));
		else if (idx == E_BUZZER)
			log_err("buzzer rejected", purifier_set_buzzer(state));
		else if (idx == E_LOCK)
			log_err("child lock rejected", purifier_set_child_lock(state));
		else
			return 0;
		return 1u << idx;
	}
	if (type == MSG_BUTTON_CMD) {
		idx = find_key(key, T_BUTTON);
		if (idx == E_RESTART) {
			/* Not from here: the reboot runs from api_entity_poll()
			 * RESTART_DELAY_MS later, in the app loop. */
			const char *why = restart_refusal();
			if (why) {
				api_log(API_LOG_WARN, why);
			} else if (!dg.restart) {
				dg.restart = 1;
				dg.restart_ms = hal_millis();
				api_log(API_LOG_INFO, "restart requested, rebooting in 0.5 s");
			}
			return 0;
		}
		if (idx != E_RESET_FILTER)
			return 0;
		log_err("filter reset rejected", purifier_reset_filter());
		api_log(API_LOG_INFO, "filter reset");
		return (1u << E_FILTER_LIFE) | (1u << E_FILTER_HOURS);
	}
	return 0;
}
