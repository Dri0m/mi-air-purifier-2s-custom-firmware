/* hooks.c - FreeRTOS kernel hooks and fatal-error handling. */
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include <wmstdio.h>
#include <lowlevel_drivers.h>

/* Fatal output goes through platform_fatalf() (platform.c): the log ring,
 * the retained reset reason (AON RAM, replayed as "[prev]" after the
 * reset) and the polled UART -- but the UART ONLY once the console is
 * open: before that, the SDK's ll_printf() dereferenced the NULL mdev UART
 * handle and faulted again inside the fault path (that, plus the
 * scheduler's priority-grouping assert, silently hung the first boot).
 * platform_fatalf() works with interrupts disabled and from any context. */
#include <critical_error.h>
#include "platform.h"
#define FATAL_PRINT(...) platform_fatalf(__VA_ARGS__)

static uint32_t uptime_s(void)
{
	/* the tick count is a plain 32-bit read: fine with interrupts off */
	return (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
}

static void fatal_reset(void) __attribute__((noreturn));
static void fatal_reset(void)
{
	/* boot2 samples GPIO43 (OLED CS, low while a display transfer is in
	 * flight) at reset: low forces the second appfw entry.  Deassert it
	 * like hal_reboot() does, so a crash mid-transfer doesn't pick the
	 * wrong slot (if the pin state survives the reset at all). */
	GPIO_WritePinOutput(GPIO_43, GPIO_IO_HIGH);
	/* Busy-wait ~1 s at 200 MHz so the message drains, then reset.  A
	 * crashed device should come back on its own rather than hang. */
	for (volatile uint32_t i = 0; i < 40000000u; i++)
		;
	NVIC_SystemReset();
	for (;;)
		;
}

void vAssertCalled(const char *pcFile, uint32_t ulLine)
{
	taskDISABLE_INTERRUPTS();
	FATAL_PRINT("\r\n*** ASSERT %s:%u at %lus\r\n", pcFile, (unsigned)ulLine,
		    (unsigned long)uptime_s());
	fatal_reset();
}

/* SDK assert (core/wmstdio/wmassert.c, used by the prebuilt wlcmgr):
 * stock prints and spins forever in a critical section; with no watchdog
 * that is a hang until a power cycle.  Linked via -Wl,--wrap=_wm_assert. */
void __wrap__wm_assert(const char *filename, int lineno, const char *fail_cond);
void __wrap__wm_assert(const char *filename, int lineno, const char *fail_cond)
{
	taskDISABLE_INTERRUPTS();
	FATAL_PRINT("\r\n*** SDK assert %s:%d (%s) at %lus\r\n",
		    filename ? filename : "?", lineno, fail_cond ? fail_cond : "?",
		    (unsigned long)uptime_s());
	fatal_reset();
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	taskDISABLE_INTERRUPTS();
	FATAL_PRINT("\r\n*** stack overflow in task %s at %lus\r\n", pcTaskName,
		    (unsigned long)uptime_s());
	fatal_reset();
}

void vApplicationMallocFailedHook(void)
{
	/* Not fatal: callers see NULL.  Report it (rate-limited by nature).
	 * It is kept as the reason too; any later reset path (fatal hook,
	 * hang guard, hal_reboot via hal_reboot_hook) overwrites it. */
	FATAL_PRINT("\r\n*** malloc failed (free %u) at %lus\r\n",
		    (unsigned)xPortGetFreeHeapSize(), (unsigned long)uptime_s());
}

/* Overrides the SDK's weak critical_error() (core/util/critical_error.c),
 * which halts forever.  Reached on HardFault after the register dump
 * (drivers/common/hardfault) and on fatal WLAN/app-framework errors.
 * Resetting brings the unit back without opening it. */
void critical_error(int crit_errno, void *data)
{
	(void)data;
	taskDISABLE_INTERRUPTS();
	if (crit_errno == -CRIT_ERR_HARD_FAULT) {
		/* The fault status registers are still latched here.  The task
		 * frame is the one on PSP: right for a fault in a task (thread
		 * mode); for a fault in an ISR it is the interrupted task's. */
		uint32_t psp = __get_PSP(), pc = 0, lr = 0;
		if ((psp & 3u) == 0 &&
		    ((psp >= 0x00100000u && psp + 32u <= 0x00160000u) ||
		     (psp >= 0x20000000u && psp + 32u <= 0x20020000u))) {
			lr = ((const uint32_t *)psp)[5];
			pc = ((const uint32_t *)psp)[6];
		}
		FATAL_PRINT("\r\n*** hardfault pc %08lx lr %08lx cfsr %08lx hfsr %08lx "
			    "bfar %08lx at %lus\r\n", (unsigned long)pc, (unsigned long)lr,
			    (unsigned long)SCB->CFSR, (unsigned long)SCB->HFSR,
			    (unsigned long)SCB->BFAR, (unsigned long)uptime_s());
	} else {
		FATAL_PRINT("\r\n*** critical error %d at %lus, resetting\r\n", crit_errno,
			    (unsigned long)uptime_s());
	}
	fatal_reset();
}

/* All allocation goes through heap_marvell (malloc/free/calloc/realloc are
 * --wrap'ed to pvPortMalloc & co.).  newlib's internal allocator must never
 * grow a heap of its own: `end` in the linker script is the top of the SRAM0
 * heap, so a libnosys _sbrk would hand out the keystore and beyond. */
#include <errno.h>
void *_sbrk(ptrdiff_t incr)
{
	(void)incr;
	errno = ENOMEM;
	return (void *)-1;
}
