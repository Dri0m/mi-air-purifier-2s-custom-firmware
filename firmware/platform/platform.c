/* platform.c - console, core services and boot banner.
 *
 * Remote logs (R3): every console path also goes into the RAM log ring
 * (src/log/logring.c), which api_server.c streams to ESPHome log
 * subscribers.  The paths and where they are captured:
 *   wmprintf / printf / fprintf / platform_printf -> wmstdio vprintf ->
 *       c_stdio_funcs->sf_printf: replaced by console_tee_printf() below
 *       (thread context, under the wmstdio mutex);
 *   wm_printf (prebuilt WLAN manager), platform_logf (boot checkpoints,
 *       OTA_LOG), platform_fatalf (fatal hooks): here, any context;
 *   ll_printf (SDK ISR / hard-fault / assert prints): __wrap_ll_printf
 *       below, active only when the link has -Wl,--wrap=ll_printf.
 * UART output is unchanged.  The ring's last 512 bytes and the fatal
 * reason are mirrored into AON RAM (.nvram_uninit, not cleared by the
 * startup code) and replayed as "[prev] ..." lines on the next boot. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include <wmerrno.h>
#include <wmstdio.h>
#include <wmtime.h>
#include <pwrmgr.h>
#include <partition.h>
#include <boot_flags.h>
#include <lowlevel_drivers.h>
#include <board.h>

#include "platform.h"
#include "logring.h"
#include "diag.h"

#define CONSOLE_UART   UART0_ID
#define CONSOLE_BAUD   115200

static int console_ready;

/* Retained across soft resets (AON RAM; see logring.h).  Word access only.
 * In .nvram_uninit.* (linked after the SDK's own .nvram_uninit variables,
 * so pm4_sig / the hwrtc words keep their addresses), outside the .nvram
 * range that mdev_startup.c zero-fills on every non-PM4 boot. */
__attribute__((section(".nvram_uninit.logring"), aligned(4)))
static logring_retain log_retained;
static int log_attached, log_prev_done;

static int (*console_sf_printf)(char *);
static int (*console_sf_putchar)(char *);
static stdio_funcs_t console_tee_funcs;

static int console_tee_printf(char *s)
{
	logring_puts(s);
	return console_sf_printf(s);
}

static int console_tee_putchar(char *c)
{
	logring_write(c, 1);
	return console_sf_putchar(c);
}

static void log_attach(void)
{
	if (!log_attached) {
		log_attached = 1;
		logring_retain_attach(&log_retained);
	}
}

int platform_boot_partition(void)
{
	return (int)(g_boot_flags & BOOT_PARTITION_MASK);
}

int platform_boot_fallback(void)
{
	return (g_boot_flags & BOOT_MAIN_FIRMWARE_BAD_CRC) ? 1 : 0;
}

uint32_t platform_millis(void)
{
	return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

uint32_t platform_free_heap(void)
{
	return (uint32_t)xPortGetFreeHeapSize();
}

void platform_reboot(void)
{
	logring_set_reason("platform_reboot()");
	if (console_ready)
		wmstdio_flush();
	NVIC_SystemReset();
	for (;;)
		;
}

int platform_printf(const char *fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = vprintf(fmt, ap);           /* wmstdio's vprintf (mutex + UART0) */
	va_end(ap);
	return r;
}

/* Used by the prebuilt Wi-Fi libs and the SDK's ISR/OS debug prints: no
 * mutex, polled UART writes, so it is safe from any context. */
void wm_printf(const char *format, ...)
{
	static char buf[128];
	va_list ap;
	if (!console_ready)
		return;
	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	logring_puts(buf);
	UART_WriteLine(CONSOLE_UART, (uint8_t *)buf);
}

/* SDK ll_printf (ISR / OS debug / hard-fault dump / wmassert), only with
 * -Wl,--wrap=ll_printf at link time; unused otherwise.  Unlike the SDK's
 * version it is safe before the console is up (ring only, then). */
void __wrap_ll_printf(const char *format, ...)
{
	static char buf[128];
	va_list ap;
	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	logring_puts(buf);
	if (console_ready)
		UART_WriteLine(CONSOLE_UART, (uint8_t *)buf);
}

/* Fatal hooks (platform/hooks.c): any context, interrupts usually off.
 * Ring + retained reason + polled UART (if the console is up). */
void platform_fatalf(const char *fmt, ...)
{
	static char buf[LOGRING_REASON_MAX + 32];
	const char *r;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	logring_puts(buf);
	for (r = buf; *r == '\r' || *r == '\n' || *r == '*' || *r == ' '; r++)
		;
	logring_set_reason(r);
	if (console_ready)
		UART_WriteLine(CONSOLE_UART, (uint8_t *)buf);
}

/* Synchronous, polled log line (no mutex, no buffering): each call is on
 * the wire before it returns, so the last line printed before a hang is
 * the last step that completed.  Used for boot checkpoints and OTA logs. */
int platform_logf(const char *fmt, ...)
{
	static char buf[160];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	logring_puts(buf);
	if (console_ready)
		UART_WriteLine(CONSOLE_UART, (uint8_t *)buf);
	return n;
}

int platform_console_is_ready(void)
{
	return console_ready;
}

/* The previous boot's retained log tail + reset reason, as "[prev]" lines
 * (into the ring and to the UART).  Once per boot. */
static void log_replay_prev(void)
{
	char line[LOGRING_LINE_MAX - 16];   /* + "[prev] " + "\r\n" fits platform_logf */
	unsigned pos = 0;
	const char *r;

	if (!logring_prev_valid()) {
		platform_logf("[boot] no previous-boot log (power-on, or AON RAM not retained)\r\n");
		return;
	}
	platform_logf("[prev] ---- previous boot: last log lines ----\r\n");
	while (logring_prev_line(&pos, line, sizeof(line)) > 0)
		platform_logf("[prev] %s\r\n", line);
	r = logring_prev_reason();
	if (r[0])       /* "[W]": shown as a warning by ESPHome log viewers */
		platform_logf("[W][prev] reset reason: %s\r\n", r);
	else
		platform_logf("[prev] reset reason: none recorded (OTA/clean reboot, watchdog or lockup)\r\n");
}

void platform_console_init(void)
{
	/* The log ring works before the console (and without it). */
	log_attach();
	/* UART0 on GPIO2/3 (board_uart_pin_config) via the mdev UART driver. */
	if (!console_ready && wmstdio_init(CONSOLE_UART, CONSOLE_BAUD) == WM_SUCCESS) {
		/* Tee wmstdio's output into the log ring.  wmstdio_init() just
		 * set c_stdio_funcs to the serial console's table. */
		if (c_stdio_funcs && c_stdio_funcs->sf_printf && c_stdio_funcs->sf_putchar) {
			console_tee_funcs = *c_stdio_funcs;
			console_sf_printf = console_tee_funcs.sf_printf;
			console_sf_putchar = console_tee_funcs.sf_putchar;
			console_tee_funcs.sf_printf = console_tee_printf;
			console_tee_funcs.sf_putchar = console_tee_putchar;
			c_stdio_funcs = &console_tee_funcs;
		}
		console_ready = 1;
	}
	if (!log_prev_done) {
		log_prev_done = 1;
		log_replay_prev();
	}
}

int platform_init(void)
{
	int ret, err = 0;

	platform_console_init();

	wmprintf("\r\n\r\n=== %s %s (built %s) ===\r\n",
		 FW_NAME, FW_VERSION, fw_build_time);
	wmprintf("MW300 @ %d MHz, FreeRTOS " tskKERNEL_VERSION_NUMBER "\r\n",
		 board_cpu_freq() / 1000000);
	wmprintf("boot2: appfw partition %d%s, boot flags 0x%08x, reset cause 0x%x\r\n",
		 platform_boot_partition(),
		 platform_boot_fallback() ? " (FALLBACK: other image bad CRC)" : "",
		 (unsigned)g_boot_flags, (unsigned)g_rst_cause);

	/* RTC/wmtime: hwrtc on RC32K; calibration briefly outputs ~32 kHz on
	 * GPIO24 (same as stock, see board_rc32k_calib()). */
	ret = wmtime_init();
	if (ret != WM_SUCCESS) {
		wmprintf("wmtime_init failed %d\r\n", ret);
		err = ret;
	}
	/* Power manager: the WLAN driver registers PM callbacks. */
	ret = pm_init();
	if (ret != WM_SUCCESS) {
		wmprintf("pm_init failed %d\r\n", ret);
		err = ret;
	}
	/* Partition table (WMPT @0x4000/0x5000, higher gen wins). */
	ret = part_init();
	if (ret != WM_SUCCESS) {
		wmprintf("part_init failed %d\r\n", ret);
		err = ret;
	}
	wmprintf("heap free: %u bytes\r\n", (unsigned)platform_free_heap());
	return err;
}
