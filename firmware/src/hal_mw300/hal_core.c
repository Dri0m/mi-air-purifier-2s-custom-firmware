/* hal_core.c - hal_init, time, delays, GPIO/GPT helpers, reboot. */
#include "hal.h"
#include "hal_internal.h"

#include <FreeRTOS.h>
#include <task.h>
#include <mdev_pm.h>                 /* arch_reboot() */
#include <boot_flags.h>              /* boot_reset_cause() */

/* hal_millis() needs the tick period to be a whole number of ms. */
typedef char hal_tick_rate_check[(1000 % configTICK_RATE_HZ) == 0 ? 1 : -1];

static uint32_t g_cyc_per_us = 200;  /* fixed up in hal_init */
static uint32_t g_gpt_hz = HAL_XTAL_HZ;

/* ------------------------------------------------------------- time */

uint32_t hal_millis(void)
{
	/* Wraps at 2^32 ms like the tick counter as long as the tick
	 * period is a whole number of ms (checked above). */
	return (uint32_t)xTaskGetTickCount() * (1000u / configTICK_RATE_HZ);
}

bool hal_rtos_running(void)
{
	return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

uint32_t hal_cycles(void)
{
	return DWT->CYCCNT;
}

uint32_t hal_cycles_per_us(void)
{
	return g_cyc_per_us;
}

void hal_delay_us(uint32_t us)
{
	uint32_t start = DWT->CYCCNT;
	uint32_t n = us * g_cyc_per_us;

	while ((uint32_t)(DWT->CYCCNT - start) < n)
		;
}

void hal_sleep_ms(uint32_t ms)
{
	if (hal_rtos_running()) {
		/* +1 tick: vTaskDelay(n) may return up to one tick early. */
		vTaskDelay((TickType_t)((ms + (1000u / configTICK_RATE_HZ) - 1) /
					(1000u / configTICK_RATE_HZ)) + 1);
	} else {
		while (ms--)
			hal_delay_us(1000);
	}
}

/* ------------------------------------------------------------- GPIO */

GPIO_PinMuxFunc_Type hal_gpio_fn(GPIO_NO_Type pin)
{
	/* mw300-registers.md section 3 / mw300_pinmux.c l.228 */
	if ((pin >= GPIO_6 && pin <= GPIO_10) ||
	    (pin >= GPIO_22 && pin <= GPIO_26) ||
	    (pin >= GPIO_28 && pin <= GPIO_33))
		return PINMUX_FUNCTION_1;
	return PINMUX_FUNCTION_0;
}

void hal_pin_gpio_out(GPIO_NO_Type pin, bool high)
{
	/* Latch the level first so the pin does not glitch when it becomes
	 * an output. */
	GPIO_WritePinOutput(pin, high ? GPIO_IO_HIGH : GPIO_IO_LOW);
	GPIO_PinMuxFun(pin, hal_gpio_fn(pin));
	GPIO_SetPinDir(pin, GPIO_OUTPUT);
	GPIO_WritePinOutput(pin, high ? GPIO_IO_HIGH : GPIO_IO_LOW);
}

void hal_pin_gpio_in(GPIO_NO_Type pin)
{
	/* Exactly the stock sequence (KEY_Init 1f004dd4, MOTOR_FgIrqEnable
	 * 1f00d01c, door in XMAIR_Init): FSEL = GPIO, direction = in.  The
	 * stock app never writes DI_EN or the pull bits (GPIO_PinModeConfig
	 * is not linked), so the pad keeps its reset/boot2 state: PULL_SEL 0
	 * = default 50 k pull-up.  evidence/hal-stock-parity.md. */
	GPIO_PinMuxFun(pin, hal_gpio_fn(pin));
	GPIO_SetPinDir(pin, GPIO_INPUT);
}

/* -------------------------------------------------------------- GPT */

uint32_t hal_gpt_hz(void)
{
	return g_gpt_hz;
}

void hal_gpt_init(GPT_ID_Type id)
{
	static const CLK_GptID_Type clk_id[4] = {
		CLK_GPT_ID_0, CLK_GPT_ID_1, CLK_GPT_ID_2, CLK_GPT_ID_3 };
	static const CLK_Module_Type clk_mod[4] = {
		CLK_GPT0, CLK_GPT1, CLK_GPT2, CLK_GPT3 };
	GPT_Config_Type cfg;

	/* Stock PUBLIC_Gpt_Init: XTAL ref (38.4 MHz), divider 0, prescaler
	 * 0, upper value 50 * 0x51eb851 (free running, ~2^32). */
#if HAL_GPT_USE_XTAL
	CLK_GPTInternalClkSrc(clk_id[id], CLK_XTAL_REF);
	CLK_ModuleClkDivider(clk_mod[id], 0);
	g_gpt_hz = HAL_XTAL_HZ;
#else
	/* Fallback (SDK gpt_drv_init): system clock / 4. */
	CLK_GPTInternalClkSrc(clk_id[id], CLK_SYSTEM);
	CLK_ModuleClkDivider(clk_mod[id], 4);
	g_gpt_hz = CLK_GetSystemClk() / 4;
#endif
	CLK_ModuleClkEnable(clk_mod[id]);

	cfg.clockSrc = GPT_CLOCK_0;
	cfg.cntUpdate = GPT_CNT_VAL_UPDATE_NORMAL;
	cfg.clockDivider = 0;
	cfg.clockPrescaler = 0;
	cfg.uppVal = 50u * 0x51eb851u;
	GPT_Init(id, &cfg);
	GPT_CounterReset(id);
	GPT_IntMask(id, GPT_INT_ALL, MASK);
	GPT_Start(id);
}

/* Same register sequence as the SDK's gpt_drv_pwm(), which stock uses. */
void hal_gpt_pwm(GPT_ID_Type id, GPT_ChannelNumber_Type ch,
		 uint32_t ton, uint32_t toff)
{
	GPT_ChannelOutputConfig_Type oc;

	GPT_ChannelFuncSelect(id, ch, GPT_CH_FUNC_PWM_EDGE);
	oc.polarity = GPT_CH_WAVE_POL_POS;
	oc.cntMatchVal0 = ton;
	oc.cntMatchVal1 = toff;
	GPT_ChannelOutputConfig(id, ch, &oc);
	GPT_ChannelReset(id, (uint32_t)1 << ch);
}

/* ------------------------------------------------------------- init */

static void hal_park_pins(void)
{
	/* Stock board_gpio_power_on (fw-hardware-map.md section 9): unused
	 * pins parked as outputs; GPIO34 high (purpose unknown, possibly an
	 * enable line, so keep stock behavior). */
	static const GPIO_NO_Type low[] = {
		GPIO_5, GPIO_8, GPIO_18, GPIO_22, GPIO_23, GPIO_37, GPIO_41 };
	size_t i;

	for (i = 0; i < sizeof(low) / sizeof(low[0]); i++)
		hal_pin_gpio_out(low[i], false);
	hal_pin_gpio_out(GPIO_34, true);
	/* LED0 (GPT1_CH5 in stock, Wi-Fi blinker, inverted): idle = high. */
	hal_pin_gpio_out(PIN_LED0_WIFI, true);
}

void hal_init(void)
{
	/* DWT cycle counter for busy delays and I2C/ADC timeouts. */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	g_cyc_per_us = CLK_GetSystemClk() / 1000000u;
	if (g_cyc_per_us == 0)
		g_cyc_per_us = 1;

	CLK_ModuleClkEnable(CLK_GPIO);   /* PERI_CLK_EN bit 4 (gate, 0 = on) */

	hal_park_pins();

	/* Stock order (XMAIR_Init): I2C, GPT, buzzer, ..., light ADC, motor,
	 * keys, door, SHT, PM, OLED, LEDs. */
	hal_gpt_init(GPT0_ID);           /* fan clock + buzzer */
	hal_gpt_init(GPT2_ID);           /* BL-PWM */
	hal_io_init();                   /* buzzer, LEDs, BL-PWM, keys, door, ADC */
	hal_fan_init();
	hal_rfid_init();                 /* PDOWN asserted, reader stays off */
	hal_i2c_init(I2C1_PORT);
	hal_eeprom_init();
	hal_sht_init();
	hal_pm_init();
	hal_display_init();
}

/* True after a software/watchdog/lockup reset, false after power-on or
 * brown-out.  boot_init() (SDK startup) latched and cleared the PMU
 * LAST_RST_CAUSE register: bit3 SYSRESETREQ, bit4 LOCKUP, bit5 WDT. */
bool hal_reset_was_soft(void)
{
	return (boot_reset_cause() & ((1u << 3) | (1u << 4) | (1u << 5))) != 0;
}

/* Stock "Wi-Fi reset" forgets the AP and enters provisioning.  This
 * firmware takes Wi-Fi credentials from secrets.h, so it just reboots
 * (hal.h default). */
void hal_wifi_reset(void)
{
	/* Stock lets the combo's triple beep finish (~1 s) before resetting.
	 * Wi-Fi credentials are compile-time in this firmware, so a "Wi-Fi
	 * reset" is just a reboot. */
	for (int i = 0; i < 3; i++) {
		hal_buzzer(true);
		vTaskDelay(pdMS_TO_TICKS(120));
		hal_buzzer(false);
		vTaskDelay(pdMS_TO_TICKS(120));
	}
	vTaskDelay(pdMS_TO_TICKS(300));
	hal_reboot();
}

__attribute__((weak)) void hal_reboot_hook(void)
{
}

void hal_reboot(void)
{
	hal_reboot_hook();
	hal_fan_set_rpm(0);
	hal_buzzer(false);
	/* boot2 samples GPIO43 (our OLED CS) at reset: low forces the second
	 * appfw entry.  Make sure it is high (deasserted) going into reset. */
	GPIO_WritePinOutput(PIN_OLED_CS, GPIO_IO_HIGH);
	/* SDK reboot: PM1, RC32M, WLAN power-down, NVIC_SystemReset. */
	arch_reboot();
	NVIC_SystemReset();
}
