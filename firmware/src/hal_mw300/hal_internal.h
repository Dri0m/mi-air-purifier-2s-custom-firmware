/* hal_internal.h - private to src/hal_mw300: pin map, shared helpers.
 *
 * Pin/peripheral facts: evidence/fw-hardware-map.md (stock appfwA RE).
 * Pinmux function numbers: evidence/mw300-registers.md section 3 and the
 * SDK's mw300_pinmux.h (GPIO is f1 on GPIO6-10, 22-26, 28-33; f0 elsewhere).
 */
#ifndef HAL_INTERNAL_H
#define HAL_INTERNAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lowlevel_drivers.h>

/* ------------------------------------------------------------ pin map */
/* Fan (motor 0 in the stock table at .data 0x20000402) */
#define PIN_FAN_PWM      GPIO_0   /* f1 GPT0_CH0, frequency-coded speed */
#define PIN_FAN_FG       GPIO_1   /* f0 GPIO in, rising-edge IRQ        */
#define PIN_FAN_BRK      GPIO_47  /* f0 GPIO out, held high             */
/* Buzzer */
#define PIN_BUZZER       GPIO_4   /* f1 GPT0_CH4, 3.097 kHz             */
/* PM sensor */
#define PIN_PM_AUX       GPIO_6   /* f1 GPIO out, high once at init     */
#define PIN_PM_SET       GPIO_7   /* f1 GPIO out, high = run            */
#define PIN_PM_TX        GPIO_48  /* f2 UART2_TXD (MCU -> sensor)       */
#define PIN_PM_RX        GPIO_49  /* f2 UART2_RXD (sensor -> MCU)       */
/* I2C1: SHT3x + EEPROM */
#define PIN_I2C1_SDA     GPIO_9   /* f4 I2C1_SDA                        */
#define PIN_I2C1_SCL     GPIO_10  /* f4 I2C1_SCL                        */
/* Status LEDs + brightness */
#define PIN_LED_GREEN    GPIO_11  /* f0 GPIO out, active high           */
#define PIN_LED_ORANGE   GPIO_12  /* f0 GPIO out, active high           */
#define PIN_LED_RED      GPIO_13  /* f0 GPIO out, active high           */
#define PIN_BL_PWM       GPIO_14  /* f1 GPT2_CH3, 1.2 kHz               */
#define PIN_LED0_WIFI    GPIO_24  /* f1 = GPIO24 (idle high); f3 GPT1_CH5 */
/* Keys / door */
#define PIN_KEY_POWER    GPIO_15  /* f0 GPIO in, active low             */
#define PIN_KEY_2        GPIO_35  /* f0 GPIO in, active low (2nd key)   */
#define PIN_DOOR         GPIO_26  /* f1 GPIO in, high = open            */
/* RFID (MFRC630 on I2C0) - held in power-down */
#define PIN_RFID_PDOWN   GPIO_19  /* f0 GPIO out, high = power-down     */
#define PIN_I2C0_SDA     GPIO_20  /* f2 I2C0_SDA (left as GPIO input)   */
#define PIN_I2C0_SCL     GPIO_21  /* f2 I2C0_SCL (left as GPIO input)   */
/* OLED (SSD1327-family, SSP1) */
#define PIN_OLED_MOSI    GPIO_38  /* f3 SSP1_TXD                        */
#define PIN_OLED_RES     GPIO_39  /* f0 GPIO out, low = reset           */
#define PIN_OLED_DC      GPIO_40  /* f0 GPIO out, 0 = cmd, 1 = data     */
#define PIN_OLED_CLK     GPIO_42  /* f3 SSP1_CLK                        */
#define PIN_OLED_CS      GPIO_43  /* f0 GPIO out, active low            */
/* Ambient light */
#define PIN_LIGHT        GPIO_46  /* f1 ADC0_IN4                        */

/* ----------------------------------------------------------- tunables */
#ifndef HAL_GPT_USE_XTAL
#define HAL_GPT_USE_XTAL 1        /* stock: GPTs on the 38.4 MHz XTAL ref */
#endif
#define HAL_XTAL_HZ          38400000u
#define HAL_FAN_FG_PPR       15u  /* FG edges per "rpm-unit" revolution */
#define HAL_FAN_MAX_RPM      2100u /* stock setter limit (300..2100)     */
#define HAL_FAN_FG_WINDOW_MS 2000u
#define HAL_BUZZER_HZ        3097u
#define HAL_BL_PWM_HZ        1200u
#define HAL_KEY_DEBOUNCE_MS  20u
#ifndef HAL_USE_KEY2
#define HAL_USE_KEY2 1            /* GPIO35 -> HAL_KEY_LED              */
#endif
#define HAL_LED_ON_THRESHOLD 1u   /* hal_leds(): colour GPIO on if >= this */
#define HAL_SHT_PERIOD_MS    2000u
#define HAL_LIGHT_PERIOD_MS  100u
/* OLED orientation knobs for bring-up (stock remap register = 0x42). */
#ifndef HAL_OLED_HIGH_NIBBLE_FIRST
#define HAL_OLED_HIGH_NIBBLE_FIRST 1
#endif
#ifndef HAL_OLED_MIRROR_X
#define HAL_OLED_MIRROR_X 0
#endif
#ifndef HAL_OLED_MIRROR_Y
#define HAL_OLED_MIRROR_Y 0
#endif

/* --------------------------------------------------- hal_core.c */
void hal_delay_us(uint32_t us);         /* busy wait (DWT cycle counter) */
void hal_sleep_ms(uint32_t ms);         /* RTOS sleep, busy before start */
uint32_t hal_cycles(void);              /* DWT->CYCCNT                   */
uint32_t hal_cycles_per_us(void);
bool hal_rtos_running(void);
void hal_pin_gpio_out(GPIO_NO_Type pin, bool high);
void hal_pin_gpio_in(GPIO_NO_Type pin); /* GPIO function, input, pull-up */
GPIO_PinMuxFunc_Type hal_gpio_fn(GPIO_NO_Type pin);

/* GPT timebase (GPT0 = fan + buzzer, GPT2 = BL-PWM) */
void hal_gpt_init(GPT_ID_Type id);
uint32_t hal_gpt_hz(void);
void hal_gpt_pwm(GPT_ID_Type id, GPT_ChannelNumber_Type ch,
		 uint32_t ton, uint32_t toff);

/* --------------------------------------------------- hal_i2c.c */
#define HAL_I2C_OK       0
#define HAL_I2C_NACK    -2
#define HAL_I2C_TIMEOUT -3
void hal_i2c_init(I2C_ID_Type port);
void hal_i2c_lock(I2C_ID_Type port);
void hal_i2c_unlock(I2C_ID_Type port);
/* One bus transaction: START addr+W w[0..wn) [RESTART addr+R r[0..rn)] STOP.
 * wn or rn may be 0.  Caller holds the lock. */
int hal_i2c_xfer(I2C_ID_Type port, uint8_t addr, const uint8_t *w, size_t wn,
		 uint8_t *r, size_t rn);

/* --------------------------------------------------- per-module init */
void hal_fan_init(void);
void hal_pm_init(void);
void hal_sht_init(void);
void hal_eeprom_init(void);
void hal_rfid_init(void);
void hal_io_init(void);
void hal_display_init(void);

/* --------------------------------------------------- extras (proposed
 * for hal.h; not used by the portable logic yet) */
uint8_t hal_mw300_pm_header(void);     /* 'B'/'C'/'D' of last frame, 0 = none */
void hal_mw300_pm_send_mode(uint8_t arg); /* stock cmd 0x59, arg 0/1     */
#endif
