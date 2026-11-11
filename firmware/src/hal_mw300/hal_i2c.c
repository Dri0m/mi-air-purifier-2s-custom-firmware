/* hal_i2c.c - polled I2C master on the MW300 DesignWare I2C blocks.
 *
 * Bus setup follows the SDK's mdev_i2c.c (module clock = sysclk / 3,
 * standard mode, SCL high 4.0 us / low 4.7 us), which stock uses via
 * i2c_drv_open().  Transfers are polled like i2c_drv_pio_read/write.
 *
 * DesignWare quirk: the controller issues STOP as soon as its TX FIFO
 * runs empty.  A preempted task could therefore split a transaction, so
 * the FIFO is fed with the scheduler suspended (ISRs still run; the
 * 16-entry FIFO covers ~1.4 ms of ISR latency at 100 kHz).
 */
#include "hal_internal.h"

#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>

#define I2C_MOD_CLK_DIV   3
#define SCL_HIGH_NS       4000u
#define SCL_LOW_NS        4700u
#define MAX_OUTSTANDING   8u     /* read commands in flight */

static SemaphoreHandle_t g_mutex[2];

void hal_i2c_init(I2C_ID_Type port)
{
	I2C_CFG_Type cfg;
	I2C_FIFO_Type fifo = { 0, 0 };
	I2C_SpeedClkCnt_Type cnt;
	uint32_t mhz;

	if (port == I2C0_PORT) {
		GPIO_PinMuxFun(PIN_I2C0_SDA, GPIO20_I2C0_SDA);
		GPIO_PinMuxFun(PIN_I2C0_SCL, GPIO21_I2C0_SCL);
		CLK_ModuleClkEnable(CLK_I2C0);
		CLK_ModuleClkDivider(CLK_I2C0, I2C_MOD_CLK_DIV);
	} else {
		GPIO_PinMuxFun(PIN_I2C1_SDA, GPIO9_I2C1_SDA);
		GPIO_PinMuxFun(PIN_I2C1_SCL, GPIO10_I2C1_SCL);
		CLK_ModuleClkEnable(CLK_I2C1);
		/* NB: one I2C_CLK_DIV field is shared by I2C0 and I2C1. */
		CLK_ModuleClkDivider(CLK_I2C1, I2C_MOD_CLK_DIV);
	}

	/* I2C module clock: CHIP_SFLL_FREQ()/3 in mdev_i2c.c (~66 MHz). */
	mhz = CLK_GetSystemClk() / 1000000u / I2C_MOD_CLK_DIV;
	cnt.stdSpeedClkHighCnt = mhz * SCL_HIGH_NS / 1000u;
	cnt.stdSpeedClkLowCnt = mhz * SCL_LOW_NS / 1000u;
	if (cnt.stdSpeedClkHighCnt < 6)
		cnt.stdSpeedClkHighCnt = 6;
	if (cnt.stdSpeedClkLowCnt < 8)
		cnt.stdSpeedClkLowCnt = 8;
	cnt.fastSpeedClkHighCnt = 6 > mhz * 600u / 1000u ? 6 : mhz * 600u / 1000u;
	cnt.fastSpeedClkLowCnt = 8 > mhz * 1300u / 1000u ? 8 : mhz * 1300u / 1000u;
	cnt.highSpeedClkHighCnt = 6;
	cnt.highSpeedClkLowCnt = 8;

	cfg.masterSlaveMode = I2C_MASTER;
	cfg.speedMode = I2C_SPEED_STANDARD;
	cfg.masterAddrBitMode = I2C_ADDR_BITS_7;
	cfg.slaveAddrBitMode = I2C_ADDR_BITS_7;
	cfg.ownSlaveAddr = 0;
	cfg.restartEnable = ENABLE;

	I2C_Disable(port);
	I2C_SetClkCount(port, &cnt);
	I2C_Init(port, &cfg);
	I2C_FIFOConfig(port, &fifo);
	I2C_IntMask(port, I2C_INT_ALL, MASK); /* polled; NVIC IRQ stays off */
	I2C_IntClr(port, I2C_INT_ALL);

	if (g_mutex[port] == NULL)
		g_mutex[port] = xSemaphoreCreateMutex();
}

void hal_i2c_lock(I2C_ID_Type port)
{
	if (g_mutex[port] && hal_rtos_running())
		xSemaphoreTake(g_mutex[port], portMAX_DELAY);
}

void hal_i2c_unlock(I2C_ID_Type port)
{
	if (g_mutex[port] && hal_rtos_running())
		xSemaphoreGive(g_mutex[port]);
}

static bool timed_out(uint32_t start, uint32_t limit_cyc)
{
	return (uint32_t)(hal_cycles() - start) > limit_cyc;
}

static bool aborted(I2C_ID_Type port)
{
	return I2C_GetRawIntStatus(port, I2C_INT_TX_ABORT) == SET;
}

int hal_i2c_xfer(I2C_ID_Type port, uint8_t addr, const uint8_t *w, size_t wn,
		 uint8_t *r, size_t rn)
{
	/* ~100 us per byte at 100 kHz, plus 2 ms margin. */
	uint32_t limit = (uint32_t)(2000u + 200u * (wn + rn + 2)) *
			 hal_cycles_per_us();
	uint32_t t0;
	size_t wi = 0, ri = 0, cmds = 0;
	int err = HAL_I2C_OK;
	bool suspended = false;

	if (wn == 0 && rn == 0)
		return HAL_I2C_OK;

	/* TAR can only change while the block is disabled. */
	I2C_Disable(port);
	hal_delay_us(10);
	I2C_SetSlaveAddr(port, addr);
	I2C_IntClr(port, I2C_INT_ALL);
	I2C_Enable(port);

	if (hal_rtos_running()) {
		vTaskSuspendAll();
		suspended = true;
	}

	t0 = hal_cycles();
	while (wi < wn || ri < rn) {
		if (aborted(port)) {
			err = HAL_I2C_NACK;
			break;
		}
		if (wi < wn) {
			if (I2C_GetStatus(port, I2C_STATUS_TFNF) == SET) {
				I2C_SendByte(port, w[wi++]);
				t0 = hal_cycles();
			}
		} else if (cmds < rn && (cmds - ri) < MAX_OUTSTANDING &&
			   I2C_GetStatus(port, I2C_STATUS_TFNF) == SET) {
			/* Direction change after writes -> RESTART. */
			I2C_MasterReadCmd(port);
			cmds++;
		}
		while (ri < rn && I2C_GetStatus(port, I2C_STATUS_RFNE) == SET) {
			r[ri++] = I2C_ReceiveByte(port);
			t0 = hal_cycles();
		}
		if (timed_out(t0, limit)) {
			err = HAL_I2C_TIMEOUT;
			break;
		}
	}

	if (suspended)
		xTaskResumeAll();

	/* Wait for the STOP: TX FIFO empty and master idle. */
	t0 = hal_cycles();
	while (err == HAL_I2C_OK) {
		if (aborted(port)) {
			err = HAL_I2C_NACK;
			break;
		}
		if (I2C_GetStatus(port, I2C_STATUS_TFE) == SET &&
		    I2C_GetStatus(port, I2C_STATUS_MST_ACTIVITY) == RESET)
			break;
		if (timed_out(t0, limit)) {
			err = HAL_I2C_TIMEOUT;
			break;
		}
	}
	if (aborted(port) && err == HAL_I2C_OK)
		err = HAL_I2C_NACK;

	/* On abort the controller flushes its TX FIFO and sends STOP; give it
	 * time to finish before disabling. */
	if (err != HAL_I2C_OK) {
		t0 = hal_cycles();
		while (I2C_GetStatus(port, I2C_STATUS_MST_ACTIVITY) == SET &&
		       !timed_out(t0, 2000u * hal_cycles_per_us()))
			;
	}
	while (I2C_GetStatus(port, I2C_STATUS_RFNE) == SET)
		(void)I2C_ReceiveByte(port);
	I2C_IntClr(port, I2C_INT_ALL);
	I2C_Disable(port);
	return err;
}
