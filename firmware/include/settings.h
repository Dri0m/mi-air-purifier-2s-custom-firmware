/* settings.h - persisted settings, stored in the settings EEPROM using the
 * stock layout (fw-app-logic.md 6.2) so stock and custom firmware agree. */
#ifndef SETTINGS_H
#define SETTINGS_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint8_t mode;
	uint16_t favorite_rpm;
	uint16_t silent_rpm;
	bool led, buzzer, child_lock;
	uint32_t filter_usage;             /* stock units */
	bool power_restore;                /* stock: power state after soft reboot */
} settings_t;

int settings_load(settings_t *s);          /* 0 ok, defaults filled on error */
int settings_save(const settings_t *s);
#endif
