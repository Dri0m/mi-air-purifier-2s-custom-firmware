/* hal_pm.c - laser PM2.5 sensor on UART2 9600 8N1 (fw-hardware-map.md 4).
 *
 * RX is interrupt driven into a 512-byte ring (stock: uart_drv_rxbuf_size
 * 512); hal_pm_poll() parses 32-byte 'BM'/'CM'/'DM' frames and returns the
 * raw PM2.5 (bytes 6-7).  Calibration ('BM' only) is done by the logic.
 */
#include "hal.h"
#include "hal_internal.h"
#include "hal_proto.h"

#define PM_RING 512u                /* power of two */

static volatile uint8_t g_ring[PM_RING];
static volatile uint16_t g_head;    /* written by ISR */
static volatile uint16_t g_tail;    /* written by task */
static volatile uint32_t g_overflow;
static pm_parser g_parser;
static uint8_t g_header;            /* last valid frame header, 0 = none */

static void pm_uart_isr(void)
{
	while (UART_GetLineStatus(UART2_ID, UART_LINESTATUS_DR) == SET) {
		uint8_t b = (uint8_t)UART_ReceiveData(UART2_ID);
		uint16_t next = (uint16_t)((g_head + 1) & (PM_RING - 1));
		if (next == g_tail) {
			g_overflow++;
			continue;           /* drop newest */
		}
		g_ring[g_head] = b;
		g_head = next;
	}
}

void hal_pm_init(void)
{
	UART_CFG_Type ucfg;
	UART_FifoCfg_Type fcfg;

	/* Stock POWDER_Sensor_Init(6): SET high, 100 ms, AUX high, UART. */
	hal_pin_gpio_out(PIN_PM_SET, true);
	hal_sleep_ms(100);
	hal_pin_gpio_out(PIN_PM_AUX, true);

	GPIO_PinMuxFun(PIN_PM_TX, GPIO48_UART2_TXD);
	GPIO_PinMuxFun(PIN_PM_RX, GPIO49_UART2_RXD);
	/* No pull/DI_EN writes: stock only muxes the pins (1f00e884). */

	/* Same bring-up as SDK uart_drv_init/uart_drv_open for UART2, which
	 * stock uses in its non-DMA (RX IRQ) mode: slow UART clock, 9600 8N1,
	 * HSE on, FIFO on, RX level 1 byte (uart_drv_open 1f03ee58). */
	CLK_SetUARTClkSrc(CLK_UART_ID_2, CLK_UART_SLOW);
	CLK_ModuleClkEnable(CLK_UART2);

	ucfg.baudRate = 9600;
	ucfg.dataBits = UART_DATABITS_8;
	ucfg.stickyParity = DISABLE;
	ucfg.parity = UART_PARITY_NONE;
	ucfg.highSpeedUart = ENABLE;
	ucfg.nrzCodeing = DISABLE;

	fcfg.fifoEnable = ENABLE;
	fcfg.autoFlowControl = DISABLE;
	fcfg.rxFifoReset = ENABLE;
	fcfg.txFifoReset = ENABLE;
	fcfg.peripheralBusType = UART_PERIPHERAL_BITS_8;
	fcfg.fifoDmaEnable = DISABLE;
	fcfg.rxFifoLevel = UART_RXFIFO_BYTE_1;
	fcfg.txFifoLevel = UART_TXFIFO_HALF_EMPTY;

	UART_Disable(UART2_ID);
	UART_Enable(UART2_ID);
	UART_SetStopBits(UART2_ID, UART_STOPBITS_1);
	UART_Init(UART2_ID, &ucfg);
	UART_FifoConfig(UART2_ID, &fcfg);

	g_head = g_tail = 0;
	pm_parser_reset(&g_parser);

	install_int_callback(INT_UART2, UART_INT_RDA, pm_uart_isr);
	install_int_callback(INT_UART2, UART_INT_RTO, pm_uart_isr);
	UART_IntMask(UART2_ID, UART_INT_RDA, UNMASK);
	/* ISR does not call FreeRTOS APIs, so the lowest priority is fine. */
	NVIC_SetPriority(UART2_IRQn, 0xF);
	NVIC_EnableIRQ(UART2_IRQn);
}

bool hal_pm_poll(uint16_t *pm25_raw)
{
	bool got = false;
	pm_frame f;

	while (g_tail != g_head) {
		uint8_t b = g_ring[g_tail];
		g_tail = (uint16_t)((g_tail + 1) & (PM_RING - 1));
		if (pm_parser_feed(&g_parser, b, &f)) {
			/* Keep the newest frame of this batch. */
			*pm25_raw = f.pm25_raw;
			g_header = f.header;
			got = true;
		}
	}
	return got;
}

void hal_pm_set_active(bool on)
{
	GPIO_WritePinOutput(PIN_PM_SET, on ? GPIO_IO_HIGH : GPIO_IO_LOW);
	if (on)
		pm_parser_reset(&g_parser);
}

uint8_t hal_mw300_pm_header(void)
{
	return g_header;
}

void hal_mw300_pm_send_mode(uint8_t arg)
{
	uint8_t cmd[7];
	size_t i, n;

	/* Stock only sends this to 'BM'/'CM' sensors once the type is known. */
	if (g_header != 0x42 && g_header != 0x43)
		return;
	n = pm_build_cmd(g_header, arg, cmd);
	for (i = 0; i < n; i++) {
		uint32_t t0 = hal_cycles();
		while (UART_GetLineStatus(UART2_ID, UART_LINESTATUS_TDRQ) != SET)
			if ((uint32_t)(hal_cycles() - t0) >
			    5000u * hal_cycles_per_us())
				return;
		UART_SendData(UART2_ID, cmd[i]);
	}
}

/* hal.h */
uint8_t hal_pm_header(void)
{
	return hal_mw300_pm_header();
}
