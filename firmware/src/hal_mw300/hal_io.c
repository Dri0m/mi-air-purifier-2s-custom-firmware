/* hal_io.c - buzzer, status LEDs, brightness PWM, keys, door, light ADC.
 * (fw-hardware-map.md sections 6.3 and 7)
 */
#include "hal.h"
#include "hal_internal.h"
#include "hal_proto.h"

static uint8_t g_key_stable, g_key_cand;
static uint32_t g_key_t;
static uint16_t g_light;
static uint32_t g_light_ms;
static bool g_light_valid;
static int16_t g_bl = -1;         /* current BL-PWM value, -1 = unknown */
static bool g_buzz;

/* ------------------------------------------------------------- ADC */

static void light_adc_init(void)
{
	/* Stock PUBLIC_Light_Init: 16-bit, Vref 1.8 V internal, gain 0.5,
	 * clock divider 32; channel 4 = GPIO46 f1.  Sequence as in the SDK's
	 * adc_drv_open(). */
	ADC_CFG_Type cfg;

	GPIO_PinMuxFun(PIN_LIGHT, GPIO46_ADC0_IN4);
	cfg.adcResolution = ADC_RESOLUTION_16BIT;
	cfg.adcVrefSource = ADC_VREF_18;
	cfg.adcGainSel = ADC_GAIN_0P5;
	cfg.adcClockDivider = ADC_CLOCK_DIVIDER_32;
	ADC_Init(ADC0_ID, &cfg);
	ADC_Enable(ADC0_ID);
	ADC_ModeSelect(ADC0_ID, ADC_MODE_ADC);
	ADC_InputModeSelect(ADC0_ID, ADC_INPUT_SINGLE_ENDED);
	ADC_SetScanLength(ADC0_ID, ADC_SCAN_LENGTH_1);
	ADC_SetBufferMode(ADC0_ID, ENABLE);
	ADC_ConversionModeSelect(ADC0_ID, ADC_CONVERSION_CONTINUOUS);
	ADC_ResultWidthConfig(ADC0_ID, ADC_RESULT_WIDTH_16);
	ADC_SetScanChannel(ADC0_ID, ADC_SCAN_CHANNEL_0, ADC_CH4);
}

/* Returns the stock "raw" value (signed 16-bit, negative -> 0), or -1. */
static int32_t light_adc_raw(void)
{
	uint32_t t0;
	int16_t v;

	ADC_IntClr(ADC0_ID, ADC_RDY);
	ADC_ConversionStart(ADC0_ID);
	t0 = hal_cycles();
	while (ADC_GetStatus(ADC0_ID, ADC_STATUS_RDY) == RESET) {
		if ((uint32_t)(hal_cycles() - t0) > 5000u * hal_cycles_per_us()) {
			ADC_ConversionStop(ADC0_ID);
			return -1;
		}
	}
	ADC_ConversionStop(ADC0_ID);
	v = (int16_t)(ADC_GetConversionResult(ADC0_ID) & 0xFFFF);
	return v < 0 ? 0 : v;
}

/* Stock scale: the signed 16-bit ADC result, negative clamped to 0
 * (stock PUBLIC_Light_Read); ~20000 = brightest in the stock maths. */
uint16_t hal_light_read(void)
{
	uint32_t now = hal_millis();

	if (!g_light_valid || (uint32_t)(now - g_light_ms) >= HAL_LIGHT_PERIOD_MS) {
		int32_t raw = light_adc_raw();
		g_light_ms = now;
		if (raw >= 0) {
			g_light = (uint16_t)raw;
			g_light_valid = true;
		}
	}
	return g_light;
}

/* Entropy for the API's Noise RNG: fresh 16-bit conversions of the light
 * sensor channel; the low bits carry analog/thermal noise.  App task only
 * (same task that owns the light reading).  Doesn't disturb hal_light_read's
 * cached value. */
uint32_t hal_adc_noise(void)
{
	uint32_t acc = 0;

	for (int i = 0; i < 8; i++) {
		int32_t raw = light_adc_raw();
		acc = (acc << 4) ^ (acc >> 28) ^ (uint32_t)(raw & 0xFFFF);
	}
	return acc;
}

/* ------------------------------------------------------------ keys */

static uint8_t keys_raw(void)
{
	uint8_t k = 0;

	if (GPIO_ReadPinLevel(PIN_KEY_POWER) == GPIO_IO_LOW)
		k |= HAL_KEY_POWER;
#if HAL_USE_KEY2
	if (GPIO_ReadPinLevel(PIN_KEY_2) == GPIO_IO_LOW)
		k |= HAL_KEY_LED;
#endif
	return k;
}

uint8_t hal_keys(void)
{
	/* Polled debounce: a new level must be stable for 20 ms (stock: edge
	 * IRQ + 20 ms debounce timer).  Call at least every ~50 ms. */
	uint32_t now = hal_millis();
	uint8_t raw = keys_raw();

	if (raw != g_key_cand) {
		g_key_cand = raw;
		g_key_t = now;
	} else if (raw != g_key_stable &&
		   (uint32_t)(now - g_key_t) >= HAL_KEY_DEBOUNCE_MS) {
		g_key_stable = raw;
	}
	return g_key_stable;
}

bool hal_door_open(void)
{
	return GPIO_ReadPinLevel(PIN_DOOR) == GPIO_IO_HIGH;
}

/* ---------------------------------------------------------- buzzer */

void hal_buzzer(bool on)
{
	uint32_t half;

	if (on == g_buzz)
		return;
	g_buzz = on;
	if (!on) {
		hal_pin_gpio_out(PIN_BUZZER, false); /* stock: GPIO4 out low */
		return;
	}
	/* 38.4 MHz / (2 * 6200) = 3.097 kHz, 50 % (stock ton = toff = 6200). */
	half = (hal_gpt_hz() + HAL_BUZZER_HZ) / (2u * HAL_BUZZER_HZ);
	hal_gpt_pwm(GPT0_ID, GPT_CH_4, half, half);
	GPIO_PinMuxFun(PIN_BUZZER, GPIO4_GPT0_CH4);
}

/* ------------------------------------------------------------ LEDs */

/* BL-PWM on GPIO14 / GPT2_CH3, stock LED_Pwm_Set(1, v, 0x7d): period
 * 32000 counts at 38.4 MHz (1.2 kHz), ton = 125 * v, i.e. v/256 duty. */
static void bl_pwm_set(uint8_t v)
{
	uint32_t period, ton;

	if ((int16_t)v == g_bl)
		return;
	g_bl = v;
	if (v == 0) {
		hal_pin_gpio_out(PIN_BL_PWM, false);
	g_bl = 0;
		return;
	}
	period = hal_gpt_hz() / HAL_BL_PWM_HZ;
	ton = period * v / 256u;
	hal_gpt_pwm(GPT2_ID, GPT_CH_3, ton, period - ton);
	GPIO_PinMuxFun(PIN_BL_PWM, GPIO14_GPT2_CH3);
}

void hal_leds(uint8_t red, uint8_t green, uint8_t orange)
{
	/* The colour lines are plain GPIOs in stock (exactly one on); the
	 * dimming is the shared BL-PWM line, set to the largest value (stock
	 * drives it with the light-derived 15..245). */
	uint8_t v = red > green ? red : green;

	if (orange > v)
		v = orange;
	GPIO_WritePinOutput(PIN_LED_RED, red >= HAL_LED_ON_THRESHOLD ?
			    GPIO_IO_HIGH : GPIO_IO_LOW);
	GPIO_WritePinOutput(PIN_LED_GREEN, green >= HAL_LED_ON_THRESHOLD ?
			    GPIO_IO_HIGH : GPIO_IO_LOW);
	GPIO_WritePinOutput(PIN_LED_ORANGE, orange >= HAL_LED_ON_THRESHOLD ?
			    GPIO_IO_HIGH : GPIO_IO_LOW);
	bl_pwm_set(v);
}

/* ------------------------------------------------------------ init */

void hal_io_init(void)
{
	/* Buzzer and BL-PWM idle low, color LEDs off. */
	hal_pin_gpio_out(PIN_BUZZER, false);
	g_buzz = false;
	hal_pin_gpio_out(PIN_LED_GREEN, false);
	hal_pin_gpio_out(PIN_LED_ORANGE, false);
	hal_pin_gpio_out(PIN_LED_RED, false);
	hal_pin_gpio_out(PIN_BL_PWM, false);

	/* Stock PUBLIC_Gpio15PreDrive: drive the key line high once before
	 * using it as an input (likely pre-charges an RC on the key). */
	hal_pin_gpio_out(PIN_KEY_POWER, true);
	hal_delay_us(100);
	hal_pin_gpio_in(PIN_KEY_POWER);
#if HAL_USE_KEY2
	hal_pin_gpio_in(PIN_KEY_2);
#endif
	hal_pin_gpio_in(PIN_DOOR);
	hal_delay_us(1000);
	g_key_stable = g_key_cand = keys_raw();
	g_key_t = hal_millis();

	light_adc_init();
}
