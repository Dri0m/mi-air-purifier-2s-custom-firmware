/* hal_fan.c - BLDC fan: frequency-coded speed command + FG tach.
 * (fw-hardware-map.md section 2)
 *
 * Command: GPIO0 = GPT0_CH0, 50 % square wave at (rpm >> 2) Hz, i.e. the
 * FG rate the stock firmware expects at that rpm (15 edges / rev).
 * Stop: GPIO0 back to GPIO output low.  BRK (GPIO47) is held high always.
 * Tach: rising edges on GPIO1, counted in a GPIO IRQ.
 *
 * Deviations from stock (evidence/hal-stock-parity.md), both deliberate:
 *  - stock turns FG into an output (low) while the motor is stopped; here
 *    FG stays an input with the default pull-up so we never drive against
 *    the motor driver's FG output;
 *  - stock leaves GPT0_CH0 toggling at the last frequency for up to 1 s
 *    after MOTOR_Stop (a one-shot timer then muxes GPIO0 back to GPIO
 *    low); here the pin goes low at once.
 */
#include "hal.h"
#include "hal_internal.h"
#include "hal_proto.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <mdev_gpio.h>
#pragma GCC diagnostic pop

static volatile uint32_t g_fg_edges;
static uint16_t g_cmd_rpm;
static uint16_t g_meas_rpm;
static uint32_t g_win_ms, g_win_edges;

static void fan_fg_isr(int pin, void *data)
{
	(void)pin;
	(void)data;
	g_fg_edges++;
}

void hal_fan_init(void)
{
	/* Stock MOTOR_Clk_Init: BRK high, PWM pin low. */
	hal_pin_gpio_out(PIN_FAN_BRK, true);
	hal_pin_gpio_out(PIN_FAN_PWM, false);
	hal_pin_gpio_in(PIN_FAN_FG);
	g_cmd_rpm = 0;

	/* The SDK's mdev_gpio.c owns GPIO_IRQHandler (it overrides the weak
	 * one in mw300_gpio.c), so register through its API.  The callback
	 * only bumps a counter, so priority 0xF (set by the SDK) is fine. */
	gpio_drv_init();
	gpio_drv_set_cb(gpio_drv_open("MDEV_GPIO"), PIN_FAN_FG,
			GPIO_INT_RISING_EDGE, NULL, fan_fg_isr);

	g_win_ms = hal_millis();
	g_win_edges = g_fg_edges;
}

void hal_fan_set_rpm(uint16_t rpm)
{
	uint32_t half;

	if (rpm > HAL_FAN_MAX_RPM)
		rpm = HAL_FAN_MAX_RPM;
	half = fan_half_period(hal_gpt_hz(), rpm);

	/* BRK is never low in stock (MOTOR_Clk_Init writes 1; a one-shot
	 * timer writes 1 again 30 ms after each start).  Re-assert it. */
	GPIO_WritePinOutput(PIN_FAN_BRK, GPIO_IO_HIGH);

	if (half == 0) {
		/* Stop: PWM pin to GPIO low (stock MOTOR_Stop). */
		hal_pin_gpio_out(PIN_FAN_PWM, false);
		g_cmd_rpm = 0;
		return;
	}
	if ((rpm >> 2) == (g_cmd_rpm >> 2))
		return;                     /* same frequency, no glitch */

	/* Program the channel first, then hand the pin to the GPT. */
	hal_gpt_pwm(GPT0_ID, GPT_CH_0, half, half);
	if (g_cmd_rpm == 0)
		GPIO_PinMuxFun(PIN_FAN_PWM, GPIO0_GPT0_CH0);
	g_cmd_rpm = rpm;
}

uint16_t hal_fan_get_rpm(void)
{
	uint32_t now = hal_millis();
	uint32_t dt = now - g_win_ms;

	if (dt >= HAL_FAN_FG_WINDOW_MS) {
		uint32_t e = g_fg_edges;
		g_meas_rpm = fan_rpm_from_count(e - g_win_edges, dt,
						HAL_FAN_FG_PPR);
		g_win_edges = e;
		g_win_ms = now;
	}
	return g_meas_rpm;
}
