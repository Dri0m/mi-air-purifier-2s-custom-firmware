/* hal.h - hardware abstraction used by the portable purifier logic.
 *
 * Device implementation: src/hal_mw300/ (pins/protocols from
 * evidence/fw-hardware-map.md).  Host implementation: src/host/ (fakes).
 * All functions are non-blocking unless stated; called from the app task.
 */
#ifndef HAL_H
#define HAL_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

void hal_init(void);
uint32_t hal_millis(void);                 /* monotonic ms */

/* Fan (BLDC with driver): target speed in RPM, 0 = stop (brake). */
void hal_fan_set_rpm(uint16_t rpm);
uint16_t hal_fan_get_rpm(void);            /* measured from FG */

/* Laser PM sensor. Returns true and fills *pm25 (ug/m3, stock-corrected
 * value is computed in purifier logic) when a new valid frame arrived. */
bool hal_pm_poll(uint16_t *pm25_raw);
void hal_pm_set_active(bool on);           /* SET pin: sensor sleep/wake */
/* Header letter of the last valid PM frame: 'B' (BM), 'C', 'D', 0 = none.
 * Stock applies its PM2.5 correction to 'BM' sensors only. */
uint8_t hal_pm_header(void);

/* SHT3x temperature (0.1 C) and humidity (%RH); true if fresh. */
bool hal_sht_poll(int16_t *temp_dC, uint8_t *rh);

/* Ambient light: raw 16-bit ADC value on the stock scale (ADC0 ch4,
 * GPIO46); the logic maps it to "bright" 0..200 and LED/contrast. */
uint16_t hal_light_read(void);

/* Filter door switch: true = open. */
bool hal_door_open(void);

/* Keys.  Returns a bitmask of currently pressed keys. */
#define HAL_KEY_POWER 0x01
#define HAL_KEY_LED   0x02
uint8_t hal_keys(void);                    /* debounced in HAL: call every <=50 ms */

/* Buzzer: tone on/off (patterns are timed in logic). */
void hal_buzzer(bool on);

/* Status LEDs on the display board (0..255 PWM each).
 * hal_display_brightness: stock OLED contrast value 1..97, sent as
 * SSD1327 command 0x81 <v>. */
void hal_leds(uint8_t red, uint8_t green, uint8_t orange);
void hal_display_brightness(uint8_t level);

/* OLED framebuffer: 1 bpp, width x height from hal_display_size()
 * (128x96), row-major, stride (w+7)/8 bytes, MSB = leftmost pixel,
 * 1 = lit.  hal_display_flush() converts to the panel's 4 bpp. */
void hal_display_size(uint16_t *w, uint16_t *h);
uint8_t *hal_display_fb(void);
void hal_display_flush(void);
void hal_display_power(bool on);

/* Settings EEPROM (24C64-class at I2C 0x50).  Returns 0 on success. */
int hal_eeprom_read(uint16_t addr, void *buf, size_t len);
int hal_eeprom_write(uint16_t addr, const void *buf, size_t len); /* blocks ~6 ms per 32-byte page */

/* RFID filter tag: true if a tag is present; fills uid (7 bytes) and the
 * tag's usage counter (hours*?) as read by stock (see fw-app-logic.md 4.3). */
bool hal_rfid_poll(uint8_t uid[7], uint32_t *usage);

/* Write the filter usage back to the tag (stock page 8); <0 = n/a. */
int hal_rfid_write_usage(uint32_t used_s);

/* True if the last reset was a watchdog/software reboot (PMU reset cause),
 * false after a power cycle.  Stock restores power state only then. */
bool hal_reset_was_soft(void);

/* Two-button Wi-Fi reset (stock 5.1).  Default: reboot. */
void hal_wifi_reset(void);

/* Reboot: calls hal_reboot_hook() first (weak, empty by default; the app
 * overrides it to flush settings), then stops the fan and buzzer. */
void hal_reboot(void);
void hal_reboot_hook(void);
#endif
