/* platform.h - board/OS bring-up for the purifier firmware (88MW300).
 *
 * Boot sequence (all before main() runs is SDK code):
 *   boot2 -> Reset_IRQHandler (sdk mdev_startup.c, linked from libsdk)
 *         -> board_gpio_power_on() (platform/board.c: actuators safe)
 *         -> CLK_Init (200 MHz from 38.4 MHz XTAL via SFLL), boot_init()
 *         -> os_init(): FreeRTOS scheduler, task "app_start"
 *         -> main() (src/app/main.c) in that task.
 */
#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdint.h>

#define FW_NAME     "purifier-2s"
#ifndef FW_VERSION
#define FW_VERSION  "0.1.0-dev"
#endif

/* UART0 console (GPIO2 TX / GPIO3 RX, 115200 8N1), wmtime (RTC), power
 * manager, partition table; prints the banner + boot info.  Call first in
 * main().  Returns 0, or <0 if a core service failed (banner still out). */
int platform_init(void);

/* UART0 console only (idempotent; platform_init() calls it too).  Lets
 * main() log the OTA boot check before anything riskier runs. */
void platform_console_init(void);
int platform_console_is_ready(void);
int platform_logf(const char *fmt, ...);  /* synchronous polled line, for checkpoints */   /* fatal hooks print only if 1 */

/* Fatal-path log line (any context, interrupts may be off): log ring,
 * retained reset reason (replayed as "[prev]" on the next boot) and the
 * polled UART once the console is up. */
void platform_fatalf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Which appfw partition boot2 started (0/1, from boot2's boot flags), and
 * whether boot2 fell back because the other image had a bad CRC. */
int platform_boot_partition(void);
int platform_boot_fallback(void);

/* Console printf (thread context only; uses the wmstdio mutex). */
int platform_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Milliseconds since the scheduler started (wraps after ~49 days). */
uint32_t platform_millis(void);

/* Free heap bytes (heap_marvell, both SRAM banks). */
uint32_t platform_free_heap(void);

/* Software reset via NVIC. */
void platform_reboot(void) __attribute__((noreturn));
#endif
