/* pur_int.h - private interfaces of src/purifier (not for other modules).
 *
 * Section numbers refer to evidence/fw-app-logic.md.
 */
#ifndef PUR_INT_H
#define PUR_INT_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "purifier.h"

/* ---------- logic.c: pure functions (no HAL, no state) ---------- */

extern const uint16_t pur_fav_table[FAVORITE_LEVELS];      /* 2.2 FAV */

typedef struct {                   /* speed table 2.2, RPM */
	uint16_t silent;           /* idx1 */
	uint16_t strong;           /* idx2 */
	uint16_t high;             /* idx3 */
	uint16_t med;              /* idx4 */
	uint16_t low;              /* idx5 */
	uint16_t favorite;         /* idx6 */
	uint16_t med_l;            /* idx7 */
} pur_speeds;

#define PUR_ZONE0 35
#define PUR_ZONE2 115
#define PUR_ZONE3 150
#define PUR_ZONE4 250
#define PUR_GOODH_DEFAULT 75
#define PUR_AUTO_DEADBAND 8

int pur_fav_index(uint16_t rpm);                  /* -1 if not a FAV value */
uint16_t pur_pm_correct_bm(uint16_t raw);         /* 3.3 "BM" curve */
uint16_t pur_pm_to_aqi(uint16_t corrected);       /* 3.1: 0->1, <=600 */
uint8_t pur_aqi_state(uint16_t aqi, uint16_t goodh);
uint16_t pur_auto_rpm_for_state(uint8_t aqi_state, const pur_speeds *s);
uint16_t pur_auto_target(uint16_t aqi, uint16_t target, const pur_speeds *s,
			 uint16_t goodh);         /* 2.3, one evaluation */
bool pur_auto_eval(uint16_t aqi, uint16_t target, const pur_speeds *s,
		   uint16_t goodh, uint16_t *out); /* true = assigns (hold) */
/* Ours: the Auto speed bias applied to an (unscaled) auto rpm. */
#define PUR_BIAS_RPM_MIN 350       /* lowest stock auto speed (m1_silent
				    * default = FAV[0]) */
#define PUR_BIAS_RPM_MAX 1700      /* highest speed stock ever runs (FAV[14], strong) */
uint16_t pur_auto_bias_rpm(uint16_t rpm, int bias);
uint16_t pur_ramp_step(uint16_t cur, uint16_t target);   /* 2.4 */
uint16_t pur_ramp_step_smooth(uint16_t cur, uint16_t target); /* ours: bias */
bool pur_aqi_report_due(uint16_t aqi, uint16_t last); /* 3.1 step 4 deltas */

/* 5.3: hal_light_read() returns the stock 16-bit ADC raw value. */
uint8_t pur_bright(uint16_t raw);                 /* 0..200 */
uint8_t pur_light_pwm(uint16_t raw);              /* 1..255, unclamped */
uint8_t pur_light_pct(uint16_t raw);              /* 0..100 %, ambient for HA */
uint8_t pur_led_pwm(uint16_t raw);                /* 15..245 */
uint8_t pur_oled_contrast(uint8_t light_pwm);     /* 1..97 */

uint32_t pur_filter_used_s(uint32_t stored_s, uint32_t t_low, uint32_t t_mid,
			   uint32_t t_high);      /* 4.1 */
uint8_t pur_filter_life(uint32_t used_h, uint32_t f1_hour);

/* ---------- store.c: stock EEPROM layout (6.2) ---------- */

typedef enum {
	E_MODE, E_APP_EXTRA, E_LED, E_DEBUG, E_POWER, E_LAYOUT, E_CHILD,
	E_CHAN, E_COUNTRY, E_GOODH, E_RFID,
	E_SPD_SILENT, E_SPD_LOW, E_SPD_MED_L, E_SPD_MED, E_SPD_HIGH,
	E_SPD_STRONG, E_SPD_FAV,
	E_HWVER, E_FUSED, E_AQI_SUM, E_AQI_CNT, E_DATA_RESET, E_SLAVE_CHAN,
	E_F1_HOUR, E_BUZZER,
	E_AUTO_BIAS,                   /* ours, outside the stock table */
	E_COUNT
} store_elem;

#define STORE_LAYOUT_VERSION 0x5272u
#define STORE_OK          0
#define STORE_ERR_IO     -1        /* read failed: defaults in RAM only */
#define STORE_ERR_MAGIC  -2        /* blank/foreign: defaults written */
#define STORE_FIXED       1        /* some fields were out of range */

int store_init(void);              /* read + validate the whole store */
uint32_t store_get(store_elem e);
void store_set(store_elem e, uint32_t v);
int store_flush(void);             /* writes changed elements only */
void store_defaults(bool data_reset);   /* RAM defaults (flush to write);
					 * data_reset keeps slave chan+debug */
uint16_t store_addr(store_elem e);      /* EEPROM byte address */

/* ---------- ui.c: buzzer, LEDs, OLED renderer ---------- */

enum {                             /* 5.4 screen codes (+ our own) */
	SCR_OFF = 0x00,
	SCR_MODE = 0x03,
	SCR_MAIN = 0x0d,
	SCR_LOCK_ON = 0x0e,
	SCR_LOCK_OFF = 0x0f,
	SCR_RESET_OK = 0x13,
	SCR_RESET_REFUSED = 0x14,
	SCR_FILTER_LOW = 0x16,
	SCR_NO_FILTER = 0x26,
	SCR_DOOR = 0x80,           /* not a stock code */
	SCR_FAULT = 0x81,          /* "standby 4": motor stall */
};

enum { BEEP_SINGLE, BEEP_DOUBLE, BEEP_TRIPLE, BEEP_LONG };

typedef struct {                   /* what the renderer needs */
	const purifier_state *st;
	uint8_t aqi_state;
	bool sensor_fault;
} ui_view;

void ui_init(void);
void ui_beep(int pattern, bool enabled);     /* TRIPLE ignores enabled */
void ui_show(uint8_t screen, uint32_t ms);   /* transient screen */
void ui_clear_transient(void);
void ui_wake(uint32_t ms);                   /* main screen while LED off */
uint32_t ui_screen_since(void);              /* ms stamp of last screen change */
uint8_t ui_current_screen(const purifier_state *st, bool fault);
void ui_tick(const ui_view *v, bool fault, uint8_t pwm, uint8_t contrast);
bool ui_buzzer_busy(void);
void ui_render(uint8_t screen, const ui_view *v);   /* into hal fb */

/* ---------- hal_ext.c: optional HAL hooks (weak defaults) ----------
 * Proposed additions to hal.h; a HAL may provide strong definitions. */
/* (declared in hal.h: hal_reset_was_soft, hal_rfid_write_usage,
 *  hal_wifi_reset) */

#endif
