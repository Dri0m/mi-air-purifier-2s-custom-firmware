/* purifier.h - portable purifier state machine (behavior per
 * evidence/fw-app-logic.md).  Uses only hal.h and settings.h. */
#ifndef PURIFIER_H
#define PURIFIER_H
#include <stdbool.h>
#include <stdint.h>

typedef enum { MODE_AUTO = 0, MODE_SILENT = 1, MODE_FAVORITE = 6 } purifier_mode;

#define FAVORITE_LEVELS 15                 /* levels 0..14, stock FAV table */

/* Ours (not stock): Auto-mode fan speed bias, % of the stock auto rpm.
 * 0 = exact stock behaviour. */
#define AUTO_BIAS_MIN  (-50)
#define AUTO_BIAS_MAX  50
#define AUTO_BIAS_STEP 10

typedef struct {
	bool power;
	purifier_mode mode;
	uint8_t favorite_level;            /* 0..14, 15 = custom rpm */
	bool led;                          /* display/LEDs enabled */
	bool buzzer;
	bool child_lock;
	bool door_open;
	uint16_t pm25;                     /* "aqi" in stock = corrected PM2.5;
	                                       the panel value (placeholder 20 at
	                                       boot, 23 on a sensor fault) */
	bool pm25_unknown;                 /* ours, for HA: pm25 is a placeholder
	                                       (no reading since boot, or the
	                                       sensor is faulted) */
	bool pm_fault;                     /* stock fault "no_sensor" */
	bool motor_fault;                  /* stock fault "m1_stuck": standby 4 */
	uint16_t avg_pm25;
	int16_t temp_dC;
	uint8_t humidity;
	uint8_t bright;                    /* 0..200 like stock */
	uint8_t light;                     /* ambient light 0..100 % (ours, for HA;
	                                       read also in standby) */
	uint16_t target_rpm, motor_rpm;
	int8_t auto_bias;                  /* ours: AUTO_BIAS_MIN..MAX, step 10 */
	uint8_t filter_life;               /* % */
	uint32_t filter_hours;
	bool rfid_present;
	uint32_t changed;                  /* bitmask of PF_* since last read */
} purifier_state;

/* change flags */
#define PF_POWER   (1u << 0)
#define PF_MODE    (1u << 1)
#define PF_FAV     (1u << 2)
#define PF_LED     (1u << 3)
#define PF_BUZZER  (1u << 4)
#define PF_LOCK    (1u << 5)
#define PF_DOOR    (1u << 6)
#define PF_PM      (1u << 7)
#define PF_TEMP    (1u << 8)
#define PF_HUM     (1u << 9)
#define PF_RPM     (1u << 10)
#define PF_FILTER  (1u << 11)
#define PF_BRIGHT  (1u << 12)
#define PF_LIGHT   (1u << 13)
#define PF_PM_LIVE (1u << 14)               /* ours, for HA: pm25 or
                                               pm25_unknown changed (PF_PM
                                               keeps the stock report limits) */
#define PF_PM_FAULT (1u << 15)
#define PF_MOTOR_FAULT (1u << 16)
#define PF_AUTO_BIAS (1u << 17)             /* ours: auto_bias changed */
#define PF_LAST    PF_AUTO_BIAS

void purifier_init(void);                  /* loads settings, power-on rules */
void purifier_tick(void);                  /* call every ~50 ms */
const purifier_state *purifier_get(void);
uint32_t purifier_take_changes(void);      /* returns and clears changed */
/* Write pending settings (and the filter time) to the EEPROM now, e.g.
 * before a reboot.  Same task as purifier_tick(). */
void purifier_flush(void);

/* Commands (from HA or UI).  Return 0 or a stock-like error:
 * -6003 door open, -5001 invalid argument. */
int purifier_set_power(bool on);
int purifier_set_mode(purifier_mode m);
int purifier_set_favorite_level(uint8_t level);
int purifier_set_led(bool on);
int purifier_set_buzzer(bool on);
int purifier_set_child_lock(bool on);
int purifier_reset_filter(void);
/* Ours: Auto speed bias in %, AUTO_BIAS_MIN..AUTO_BIAS_MAX and a multiple
 * of AUTO_BIAS_STEP (else -5001).  Persisted; applies at once in Auto. */
int purifier_set_auto_bias(int bias);
#endif
