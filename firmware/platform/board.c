/* board.c - WMSDK board hooks for the Mi Air Purifier 2S main board.
 *
 * Replaces sdk/src/boards/mw300_rd.c + boards/modules/gti-mw300.c.  The SDK
 * drivers call these (board_cpu_freq from CLK_Init and FreeRTOSConfig,
 * board_uart_pin_config from wmstdio/uart_drv, board_i2c/ssp/adc_pin_config
 * from the mdev drivers, board_antenna_* from libwifidriver, ...).
 *
 * Values come from the stock app (evidence/fw-hardware-map.md, Appendix A
 * and section 9): board_main_xtal 38.4 MHz, cpu 200 MHz, 32k_xtal 0,
 * 32k_osc 0, rc32k_calib 1, antenna_select 1, antenna_switch_ctrl 1.
 */
#include <stddef.h>
#include <wmtypes.h>
#include <wmerrno.h>
#include <board.h>
#include <lowlevel_drivers.h>

int board_main_xtal(void)      { return 38400000; }
int board_main_osc(void)       { return -WM_FAIL; }
int board_cpu_freq(void)       { return 200000000; }
int board_32k_xtal(void)       { return false; }
int board_32k_osc(void)        { return false; }
/* Stock returns 1: rtc_drv_init calibrates RC32K, which briefly outputs
 * ~32.76 kHz on GPIO24 (GPT1_CH5) at boot. */
int board_rc32k_calib(void)    { return true; }

/* RF control pins GPIO44/45: stock drives 44=1, 45=0 (rfctrl_set_config(1)). */
unsigned int board_antenna_select(void) { return 1; }
int board_antenna_switch_ctrl(void)     { return true; }
/* Wi-Fi TX power tables, copied from the stock app (board_region_pwr_tbl
 * 0x1f019944, tables in .data 0x20010330/0x200103a0, 0x20010434/0x200103a8,
 * 0x200103ba/0x200103b0; evidence/platform-stock-parity.md).
 *
 * Row = { channel, 7 power values in dBm, one per modulation group }.  The
 * prebuilt driver reads `table` as flat 8-byte rows (wifi_prepare_
 * txpwrlimit_data_cmd, wifi_country_trpc_cfg_data), not as the uint8_t **
 * the header declares, so the rows are passed through a cast.
 *
 * Users, both in the stock app and in our prebuilt libs:
 *  - wlan_init() calls board_region_pwr_tbl(BOARD_COUNTRY_US) and hands the
 *    result to wifi_set_txpwrlimit_data(): the US table becomes the TX power
 *    limit sent to the radio at init (stock 0x1f02d222).  Without it the
 *    radio runs at its firmware defaults instead of Xiaomi's limits.
 *  - wlan_set_country(c) sends a TRPC table if one exists for c (stock
 *    0x1f00a790).  Stock has tables for EU and JP only; CN and the rest
 *    return NULL; 0, US and out-of-range codes get the US table. */
static const uint8_t pwr_rows_us[11][8] = {
	{ 1, 15, 12, 12, 12, 10, 10, 10 }, { 2, 15, 12, 12, 12, 10, 10, 10 },
	{ 3, 15, 12, 12, 12, 10, 10, 10 }, { 4, 17, 14, 14, 14, 13, 13, 13 },
	{ 5, 17, 14, 14, 14, 13, 13, 13 }, { 6, 17, 14, 14, 14, 13, 13, 13 },
	{ 7, 17, 14, 14, 14, 13, 13, 13 }, { 8, 17, 14, 14, 14, 13, 13, 13 },
	{ 9, 15, 12, 12, 12, 11, 11, 11 }, { 10, 15, 12, 12, 12, 11, 11, 11 },
	{ 11, 15, 12, 12, 12, 11, 11, 11 },
};
static const uint8_t pwr_rows_eu[13][8] = {
	{ 1, 13, 13, 13, 13, 13, 13, 13 }, { 2, 13, 13, 13, 13, 13, 13, 13 },
	{ 3, 13, 13, 13, 13, 13, 13, 13 }, { 4, 13, 13, 13, 13, 13, 13, 13 },
	{ 5, 13, 13, 13, 13, 13, 13, 13 }, { 6, 13, 13, 13, 13, 13, 13, 13 },
	{ 7, 13, 13, 13, 13, 13, 13, 13 }, { 8, 13, 13, 13, 13, 13, 13, 13 },
	{ 9, 13, 13, 13, 13, 13, 13, 13 }, { 10, 13, 13, 13, 13, 13, 13, 13 },
	{ 11, 13, 13, 13, 13, 13, 13, 13 }, { 12, 13, 13, 13, 13, 13, 13, 13 },
	{ 13, 13, 13, 13, 13, 13, 13, 13 },
};
static const uint8_t pwr_rows_jp[13][8] = {
	{ 1, 16, 14, 14, 14, 13, 13, 13 }, { 2, 16, 14, 14, 14, 13, 13, 13 },
	{ 3, 16, 14, 14, 14, 13, 13, 13 }, { 4, 16, 14, 14, 14, 13, 13, 13 },
	{ 5, 16, 14, 14, 14, 13, 13, 13 }, { 6, 16, 14, 14, 14, 13, 13, 13 },
	{ 7, 16, 14, 14, 14, 13, 13, 13 }, { 8, 16, 14, 14, 14, 13, 13, 13 },
	{ 9, 16, 14, 14, 14, 13, 13, 13 }, { 10, 16, 14, 14, 14, 13, 13, 13 },
	{ 11, 16, 14, 14, 14, 13, 13, 13 }, { 12, 16, 14, 14, 14, 13, 13, 13 },
	{ 13, 16, 14, 14, 14, 13, 13, 13 },
};
/* Layout must match the prebuilt libs: country u8 @0, count u8 @1,
 * table ptr @4 (short enums; checked against the lib disassembly). */
_Static_assert(offsetof(struct pwr_table, num_of_channels) == 1, "pwr_table layout");
_Static_assert(offsetof(struct pwr_table, table) == 4, "pwr_table layout");
static struct pwr_table pwr_tbl_us = {
	BOARD_COUNTRY_US, 11, (const uint8_t **)pwr_rows_us };
static struct pwr_table pwr_tbl_eu = {
	BOARD_COUNTRY_EU, 13, (const uint8_t **)pwr_rows_eu };
static struct pwr_table pwr_tbl_jp = {
	BOARD_COUNTRY_JP, 13, (const uint8_t **)pwr_rows_jp };

struct pwr_table *board_region_pwr_tbl(board_country_code_t country)
{
	switch ((int)country) {
	case BOARD_COUNTRY_EU: return &pwr_tbl_eu;
	case BOARD_COUNTRY_JP: return &pwr_tbl_jp;
	case BOARD_COUNTRY_CA: case BOARD_COUNTRY_SG: case BOARD_COUNTRY_AU:
	case BOARD_COUNTRY_KR: case BOARD_COUNTRY_FR: case BOARD_COUNTRY_CN:
		return NULL;
	default:     return &pwr_tbl_us;   /* 0, US, unknown (as stock) */
	}
}

static void gpio_out(GPIO_NO_Type pin, GPIO_PinMuxFunc_Type fn, GPIO_IO_Type lvl)
{
	GPIO_PinMuxFun(pin, fn);
	GPIO_SetPinDir(pin, GPIO_OUTPUT);
	GPIO_WritePinOutput(pin, lvl);
}

/* Called from Reset_IRQHandler_C before clocks are set up.  Puts every
 * actuator into the same safe state the stock firmware uses
 * (fw-hardware-map.md section 9, FUN_1f01650c + FUN_1f019830):
 * fan clock low (motor idle), PM sensor asleep, LEDs off, RFID powered
 * down, OLED held in reset.
 *
 * NOTE: this repurposes GPIO7/GPIO8 (JTAG TCK-ish/TMS), as stock does, so
 * SWD/JTAG is unavailable once this runs. */
void board_gpio_power_on(void)
{
	gpio_out(GPIO_0,  GPIO0_GPIO0,   GPIO_IO_LOW);   /* fan clock idle */
	gpio_out(GPIO_5,  GPIO5_GPIO5,   GPIO_IO_LOW);
	gpio_out(GPIO_7,  GPIO7_GPIO7,   GPIO_IO_LOW);   /* PM sensor SET: sleep */
	gpio_out(GPIO_8,  GPIO8_GPIO8,   GPIO_IO_LOW);
	gpio_out(GPIO_11, GPIO11_GPIO11, GPIO_IO_LOW);   /* LED green */
	gpio_out(GPIO_12, GPIO12_GPIO12, GPIO_IO_LOW);   /* LED orange */
	gpio_out(GPIO_13, GPIO13_GPIO13, GPIO_IO_LOW);   /* LED red */
	gpio_out(GPIO_18, GPIO18_GPIO18, GPIO_IO_LOW);
	gpio_out(GPIO_19, GPIO19_GPIO19, GPIO_IO_HIGH);  /* MFRC630 PDOWN: off */
	gpio_out(GPIO_21, GPIO21_GPIO21, GPIO_IO_LOW);
	gpio_out(GPIO_22, GPIO22_GPIO22, GPIO_IO_LOW);
	gpio_out(GPIO_23, GPIO23_GPIO23, GPIO_IO_LOW);
	gpio_out(GPIO_34, GPIO34_GPIO34, GPIO_IO_HIGH);  /* unknown, stock: high */
	gpio_out(GPIO_37, GPIO37_GPIO37, GPIO_IO_LOW);
	gpio_out(GPIO_39, GPIO39_GPIO39, GPIO_IO_LOW);   /* OLED RESET held */
	gpio_out(GPIO_41, GPIO41_GPIO41, GPIO_IO_LOW);

	/* Stock board_gpio_power_on (FUN_1f019830): RF ctrl pins to function 7
	 * until the Wi-Fi driver calls rfctrl_set_config(), then the
	 * wake-up pin config the SDK reference board also does. */
	GPIO_PinMuxFun(GPIO_44, PINMUX_FUNCTION_7);
	GPIO_PinMuxFun(GPIO_45, PINMUX_FUNCTION_7);
	PMU_ConfigWakeupPin(PMU_GPIO22_INT, PMU_WAKEUP_LEVEL_LOW);
	PMU_ConfigWakeupPin(PMU_GPIO23_INT, PMU_WAKEUP_LEVEL_LOW);
}

void board_uart_pin_config(int id)
{
	switch (id) {
	case UART0_ID:          /* debug console, 115200 8N1 */
		GPIO_PinMuxFun(GPIO_2, GPIO2_UART0_TXD);
		GPIO_PinMuxFun(GPIO_3, GPIO3_UART0_RXD);
		break;
	case UART2_ID:          /* PM2.5 sensor, 9600 8N1 */
		GPIO_PinMuxFun(GPIO_48, GPIO48_UART2_TXD);
		GPIO_PinMuxFun(GPIO_49, GPIO49_UART2_RXD);
		break;
	default:                /* UART1 (GPIO13/14) collides with LEDs on the 2S */
		break;
	}
}

void board_i2c_pin_config(int id)
{
	switch (id) {
	case I2C0_PORT:         /* MFRC630 RFID @0x28 */
		GPIO_PinMuxFun(GPIO_20, GPIO20_I2C0_SDA);
		GPIO_PinMuxFun(GPIO_21, GPIO21_I2C0_SCL);
		break;
	case I2C1_PORT:         /* SHT3x @0x44, 24C64 EEPROM @0x50 */
		GPIO_PinMuxFun(GPIO_9, GPIO9_I2C1_SDA);
		GPIO_PinMuxFun(GPIO_10, GPIO10_I2C1_SCL);
		break;
	}
}

void board_ssp_pin_config(int id, int cs)
{
	switch (id) {
	case SSP1_ID:           /* OLED: CLK GPIO42, MOSI GPIO38 */
		/* CS is GPIO43 driven in software: stock passes it to
		 * ssp_drv_open(..., cs=43, ...), the driver muxes it. */
		(void)cs;
		GPIO_PinMuxFun(GPIO_42, GPIO42_SSP1_CLK);
		GPIO_PinMuxFun(GPIO_38, GPIO38_SSP1_TXD);
		break;
	default:
		break;
	}
}

int board_adc_pin_config(int adc_id, int channel)
{
	(void)adc_id;
	/* Only ADC0_IN4 on GPIO46 (ambient light) is wired. */
	if (channel != ADC_CH4)
		return -WM_FAIL;
	GPIO_PinMuxFun(GPIO_46, GPIO46_ADC0_IN4);
	return WM_SUCCESS;
}

void board_dac_pin_config(int channel) { (void)channel; }
void board_usb_pin_config(void) { }
I2C_ID_Type board_acodec_i2c_id(void) { return I2C0_PORT; }
SSP_ID_Type board_acodec_ssp_id(void) { return SSP2_ID; }

/* No board LEDs/buttons in the SDK sense (the purifier HAL drives its own). */
output_gpio_cfg_t board_led_1(void) { output_gpio_cfg_t g = { .gpio = -1 }; return g; }
output_gpio_cfg_t board_led_2(void) { output_gpio_cfg_t g = { .gpio = -1 }; return g; }
output_gpio_cfg_t board_led_3(void) { output_gpio_cfg_t g = { .gpio = -1 }; return g; }
output_gpio_cfg_t board_led_4(void) { output_gpio_cfg_t g = { .gpio = -1 }; return g; }
int board_button_1(void) { return -WM_FAIL; }
int board_button_2(void) { return -WM_FAIL; }
int board_button_3(void) { return -WM_FAIL; }
int board_button_pressed(int pin) { (void)pin; return false; }
int board_wakeup0_functional(void) { return false; }
int board_wakeup1_functional(void) { return false; }
