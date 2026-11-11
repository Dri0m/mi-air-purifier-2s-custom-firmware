/* hal_display.c - 128x96 4-bpp OLED (SSD1327-family) on SSP1.
 * (fw-hardware-map.md section 6)
 *
 * hal.h exposes a 1 bpp framebuffer: row-major, (w/8) bytes per row,
 * MSB = leftmost pixel.  hal_display_flush() expands it to 4 bpp with
 * set pixels at full grey (0xF); hal_display_brightness() writes the
 * controller contrast register (0x81), as stock OLED_SetContrast does.
 *
 * SPI: SSP1 master, Motorola SPI mode 0, 8-bit, 10 MHz, polled TX
 * (stock used DMA), software CS on GPIO43, D/C on GPIO40.
 */
#include <string.h>

#include "hal.h"
#include "hal_internal.h"
#include "hal_proto.h"

#define OLED_W      128u
#define OLED_H      96u
#define OLED_SPI_HZ 10000000u

static uint8_t g_fb[OLED_W * OLED_H / 8];
static bool g_ready;
static uint8_t g_contrast = 0x80;   /* stock init value */
static bool g_contrast_dirty;

/* ------------------------------------------------------------- SPI */

static void spi_wait_idle(void)
{
	uint32_t t0 = hal_cycles();

	while (SSP_GetStatus(SSP1_ID, SSP_STATUS_TFE) != SET ||
	       SSP_GetStatus(SSP1_ID, SSP_STATUS_BUSY) == SET) {
		while (SSP_GetStatus(SSP1_ID, SSP_STATUS_RFNE) == SET)
			(void)SSP_RecvData(SSP1_ID);
		if ((uint32_t)(hal_cycles() - t0) > 10000u * hal_cycles_per_us())
			break;
	}
	while (SSP_GetStatus(SSP1_ID, SSP_STATUS_RFNE) == SET)
		(void)SSP_RecvData(SSP1_ID);
}

static void spi_write(const uint8_t *p, size_t n)
{
	while (n--) {
		uint32_t t0 = hal_cycles();
		while (SSP_GetStatus(SSP1_ID, SSP_STATUS_TFNF) != SET) {
			if ((uint32_t)(hal_cycles() - t0) >
			    1000u * hal_cycles_per_us())
				return;
		}
		SSP_SendData(SSP1_ID, *p++);
		/* TR mode: drain RX so it never overruns. */
		while (SSP_GetStatus(SSP1_ID, SSP_STATUS_RFNE) == SET)
			(void)SSP_RecvData(SSP1_ID);
	}
}

static void spi_init(void)
{
	SSP_CFG_Type cfg;
	SSP_FIFO_Type fifo;
	SPI_Param_Type spi;
	uint32_t div;

	GPIO_PinMuxFun(PIN_OLED_CLK, GPIO42_SSP1_CLK);
	GPIO_PinMuxFun(PIN_OLED_MOSI, GPIO38_SSP1_TXD);

	/* ssp_drv_set_clk(): CLK_SYSTEM / divider (<= 10 MHz). */
	div = (CLK_GetSystemClk() + OLED_SPI_HZ - 1) / OLED_SPI_HZ;
	if (div < 1)
		div = 1;
	if (div > 31)
		div = 31;
	CLK_ModuleClkEnable(CLK_SSP1);
	CLK_SSPClkSrc(CLK_SSP_ID_1, CLK_SYSTEM);
	CLK_ModuleClkDivider(CLK_SSP1, div);

	/* ssp_drv_open(SSP1, SSP_FRAME_SPI, SSP_MASTER, ..., cs, 0) */
	cfg.mode = SSP_NORMAL;
	cfg.frameFormat = SSP_FRAME_SPI;
	cfg.masterOrSlave = SSP_MASTER;
	cfg.trMode = SSP_TR_MODE;
	cfg.dataSize = SSP_DATASIZE_8;
	cfg.sfrmPola = SSP_SAMEFRM_PSP;
	cfg.slaveClkRunning = SSP_SLAVECLK_TRANSFER;
	cfg.txd3StateType = SSP_TXD3STATE_ELSB;
	cfg.txd3StateEnable = ENABLE;
	cfg.timeOutVal = 100;

	fifo.fifoPackMode = DISABLE;
	fifo.rxFifoFullLevel = 0;
	fifo.txFifoEmptyLevel = 0;
	fifo.rxDmaService = DISABLE;
	fifo.txDmaService = DISABLE;

	spi.spiClkPhase = SPI_SCPHA_1;      /* SPH = 0 */
	spi.spiClkPolarity = SPI_SCPOL_LOW; /* SPO = 0 -> mode 0 */

	SSP_Disable(SSP1_ID);
	SSP_Init(SSP1_ID, &cfg);
	SSP_FifoConfig(SSP1_ID, &fifo);
	SPI_Config(SSP1_ID, &spi);
	SSP_Enable(SSP1_ID);
}

/* ---------------------------------------------------------- OLED io */

static void cs(bool active)
{
	spi_wait_idle();
	GPIO_WritePinOutput(PIN_OLED_CS, active ? GPIO_IO_LOW : GPIO_IO_HIGH);
}

static void dc(bool data)
{
	spi_wait_idle();
	GPIO_WritePinOutput(PIN_OLED_DC, data ? GPIO_IO_HIGH : GPIO_IO_LOW);
	hal_delay_us(1);
}

/* Command group with CS framing (stock OLED_WriteCmd: DC=0, cmd, args). */
static void oled_cmds(const uint8_t *c, size_t n)
{
	cs(true);
	dc(false);
	spi_write(c, n);
	cs(false);
}

static void oled_cmd1(uint8_t c)
{
	oled_cmds(&c, 1);
}

static void oled_cmd2(uint8_t c, uint8_t a)
{
	uint8_t b[2] = { c, a };
	oled_cmds(b, 2);
}

/* ------------------------------------------------------------- API */

void hal_display_init(void)
{
	/* Exact stock OLED_Init sequence (0x1f00ff38). */
	static const uint8_t init_seq[] = {
		0xAE,             /* display off */
		0x15, 0x00, 0x3F, /* column 0..63 (2 px per byte) */
		0x75, 0x00, 0x5F, /* row 0..95 */
		0x81, 0x80,       /* contrast 0x80 */
		0xA0, 0x51,       /* remap (overridden below) */
		0xA1, 0x00,       /* start line 0 */
		0xA0, 0x42,       /* remap = 0x42 */
		0xA2, 0x60,       /* display offset 0x60 */
		0xA4,             /* normal display */
		0xA8, 0x5F,       /* MUX ratio 96 */
		0xAB, 0x01,       /* internal VDD regulator */
		0xB1, 0xA2,       /* phase length */
		0xB3, 0x00,       /* clock divider / osc */
		0xB6, 0x04,       /* second precharge */
		0xBE, 0x05,       /* VCOMH */
		0xD5, 0x62,       /* function selection B */
	};
	static const uint8_t on_seq[] = { 0xAF, 0xCB }; /* on; 0xCB as stock */
	uint8_t zero[64];
	unsigned i;

	hal_pin_gpio_out(PIN_OLED_CS, true);    /* deasserted */
	hal_pin_gpio_out(PIN_OLED_DC, false);
	hal_pin_gpio_out(PIN_OLED_RES, false);  /* held low by early boot */

	/* RES: wait 100 ms; low 10 ms; high; 1 ms. */
	hal_sleep_ms(100);
	GPIO_WritePinOutput(PIN_OLED_RES, GPIO_IO_LOW);
	hal_sleep_ms(10);
	GPIO_WritePinOutput(PIN_OLED_RES, GPIO_IO_HIGH);
	hal_sleep_ms(1);

	spi_init();
	hal_sleep_ms(50);

	/* Stock sends the whole init with CS held low and DC=0. */
	cs(true);
	dc(false);
	spi_write(init_seq, sizeof(init_seq));
	hal_sleep_ms(20);
	/* Clear GDDRAM: 6144 zero bytes (window is already 0..63 x 0..95). */
	dc(true);
	memset(zero, 0, sizeof(zero));
	for (i = 0; i < (OLED_W / 2) * OLED_H / sizeof(zero); i++)
		spi_write(zero, sizeof(zero));
	hal_sleep_ms(10);
	dc(false);
	spi_write(on_seq, sizeof(on_seq));
	cs(false);

	memset(g_fb, 0, sizeof(g_fb));
	g_ready = true;
	if (g_contrast_dirty) {
		g_contrast_dirty = false;
		oled_cmd2(0x81, g_contrast);
	}
}

void hal_display_size(uint16_t *w, uint16_t *h)
{
	*w = OLED_W;
	*h = OLED_H;
}

uint8_t *hal_display_fb(void)
{
	return g_fb;
}

void hal_display_flush(void)
{
	static const uint8_t win[] = { 0x15, 0x00, 0x3F, 0x75, 0x00, 0x5F };
	uint8_t row[OLED_W / 2];
	unsigned y;

	if (!g_ready)
		return;
	/* Stock OLED_WriteWindow: CS low, column/row window, data, CS high. */
	cs(true);
	dc(false);
	spi_write(win, sizeof(win));
	dc(true);
	for (y = 0; y < OLED_H; y++) {
		unsigned sy = HAL_OLED_MIRROR_Y ? (OLED_H - 1 - y) : y;
		oled_row_1to4(&g_fb[sy * (OLED_W / 8)], row, OLED_W, 0x0F,
			      HAL_OLED_HIGH_NIBBLE_FIRST, HAL_OLED_MIRROR_X);
		spi_write(row, sizeof(row));
	}
	cs(false);
}

void hal_display_power(bool on)
{
	if (!g_ready)
		return;
	oled_cmd1(on ? 0xAF : 0xAE);
}

/* Brightness = SSD1327 contrast register 0x81, written as is.  Stock
 * OLED_SetContrast uses 1..97: c = min(pwm * 100 / 175, 0x60) + 1, where
 * pwm is the light-derived LED value (computed by the purifier logic). */
void hal_display_brightness(uint8_t level)
{
	if (level == g_contrast && !g_contrast_dirty)
		return;
	g_contrast = level;
	if (!g_ready) {
		g_contrast_dirty = true;
		return;
	}
	oled_cmd2(0x81, g_contrast);
}
