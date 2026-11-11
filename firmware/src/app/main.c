/* main.c - task wiring for the purifier firmware.
 *
 * main() is called by the WMSDK in the "app_start" task once the scheduler
 * runs (sdk os.c).  It brings up the platform and starts two tasks:
 *
 *   net  (prio 3): Wi-Fi join, DHCP, link events, reconnects
 *                  (src/net_mw300).  Never touches purifier_* or the
 *                  netio sockets.
 *   app  (prio 2): THE single loop for everything that is not thread-safe:
 *                  hal_init() -> purifier_init(), then every APP_LOOP_MS:
 *                  purifier_tick() (at most every APP_TICK_MS) and the
 *                  non-blocking servers: ota_server_poll(), api_server_poll(),
 *                  mdns_poll().  The servers are started here once the first
 *                  IP address arrives.  All netio.h socket calls therefore
 *                  happen in this task.
 *
 * Modules that are not linked yet resolve to the weak stubs in stubs.c.
 * Keep this file small; module logic belongs in the modules.
 */
#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "platform.h"
#include "netio.h"
#include "esphome_api.h"
#include "ota.h"
#include "hal.h"
#include "purifier.h"
#include "../net_mw300/net_mw300.h"
#include "secrets.h"
#include "diag.h"
#include "logring.h"
#ifndef API_ENCRYPTION_KEY
#define API_ENCRYPTION_KEY ""          /* older secrets.h: plaintext API */
#endif

#define APP_LOOP_MS        10          /* server poll period */
#define APP_TICK_MS        50          /* purifier_tick period (<= 50 ms) */
#define NET_SERVICE_MS     100
#define HEARTBEAT_MS       60000
/* Trial confirm (R1): POST /confirm from tools/ota_upload.py, or the
 * unattended fallback in ota_trial_poll() (src/ota, include/ota.h). */
#define TRIAL_GUARD_PERIOD_MS 5000     /* hang guard check period */
#define TRIAL_GUARD_STALL_MS  60000    /* app loop silent this long in a trial -> reset */
#ifndef OTA_PORT
#define OTA_PORT           8266        /* HTTP POST /update (src/ota), tools/ota_upload.py default */
#endif

#define APP_STACK_WORDS    2048
#define NET_STACK_WORDS    2048        /* wlan_init + XZ radio fw download run here */
#define APP_PRIO           (tskIDLE_PRIORITY + 2)
#define NET_PRIO           (tskIDLE_PRIORITY + 3)

static char mac_str[18];
static api_config api_cfg;
static int ota_trial;          /* ota_boot_check() result, from main() */
static volatile TickType_t app_alive;   /* last app-loop pass (hang guard) */
static volatile int guard_reset;        /* hal_reboot() from the hang guard */

/* ESPHome API sessions that completed the handshake and subscribed to
 * states (HA does this right after connecting).  The real definition is a
 * one-liner in src/esphome/api_server.c; this weak default means "cannot
 * tell", which turns the fallback self-confirm off: a trial image is then
 * confirmed only by POST /confirm and otherwise reverts. */
int api_server_client_count(void);
__attribute__((weak)) int api_server_client_count(void)
{
	return -1;
}

/* Trial hang guard (FreeRTOS timer task).  ota_tick() runs in the app
 * loop, so an app task that hangs during a trial would never reach the
 * timeout revert and would stay hung until a power cycle.  While the image
 * is an unconfirmed trial with a valid previous slot (the table already
 * boots it), a silent app loop resets the device -> previous image.  Inert
 * once confirmed (and the timer is stopped then).  The 60 s threshold is far
 * above the longest legitimate app-loop block (hal_init ~250 ms, EEPROM dump
 * ~3 s, the PM sensor's 10 s path, ota_mark_good's flash verify).  A false
 * trigger only reverts to the previous, confirmed image. */
static void trial_guard(TimerHandle_t t)
{
	if (!ota_trial_revert_armed()) {
		xTimerStop(t, 0);       /* confirmed: done for good */
		return;
	}
	if (xTaskGetTickCount() - app_alive < pdMS_TO_TICKS(TRIAL_GUARD_STALL_MS))
		return;
	/* No console output here: platform_logf() shares one static buffer
	 * with the (stalled) app task. */
	logring_set_reason("trial hang guard: app loop stalled 60 s");
	guard_reset = 1;        /* no purifier_flush() from this task */
	hal_reboot();
}

static void start_servers(void)
{
	uint8_t mac[6];

	netio_mac(mac);
	snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	api_cfg.name = DEVICE_NAME;
	api_cfg.friendly_name = DEVICE_NAME;
	api_cfg.password = API_PASSWORD;
	api_cfg.version = FW_VERSION;
	api_cfg.mac_str = mac_str;
	api_cfg.encryption_key = API_ENCRYPTION_KEY;

	int rc = api_server_init(&api_cfg);
	if (rc < -1)                    /* -1 = listen failed, retried in poll */
		platform_printf("[app] API DISABLED: %s\r\n",
				rc == -2 ? "API_ENCRYPTION_KEY is not base64 of 32 bytes"
					 : "no entropy for encryption");
	mdns_init(&api_cfg);
	ota_server_init(OTA_PORT, OTA_PASSWORD);
	platform_printf("[app] servers started (API 6053 %s, OTA %d)\r\n",
			API_ENCRYPTION_KEY[0] ? "encrypted" : "plaintext", OTA_PORT);
}

#ifndef EEPROM_DUMP_ON_BOOT
#define EEPROM_DUMP_ON_BOOT 0          /* UART-only backup; would flood the log ring */
#endif
#if EEPROM_DUMP_ON_BOOT
/* Hex dump of the 8 KB settings EEPROM (24C64, stock layout: settings,
 * filter usage, magic; no secrets) to the console, ~3 s at 115200.
 * Save the boot log: it is the EEPROM backup.  Restore = write it back. */
static void eeprom_dump(void)
{
	uint8_t b[32];
	platform_printf("[eeprom] dump begin\r\n");
	for (uint16_t a = 0; a < 8192; a += 32) {
		if (hal_eeprom_read(a, b, sizeof b) != 0) {
			platform_printf("[eeprom] %04x read error\r\n", a);
			continue;
		}
		int ff = 1;
		for (int i = 0; i < 32; i++)
			if (b[i] != 0xFF) ff = 0;
		if (ff)
			continue;   /* skip erased rows */
		platform_printf("[eeprom] %04x:", a);
		for (int i = 0; i < 32; i++)
			platform_printf(" %02x", b[i]);
		platform_printf("\r\n");
	}
	platform_printf("[eeprom] dump end\r\n");
}
#endif

static void app_task(void *arg)
{
	TickType_t last_tick, hb, wake;
	int servers = 0;
	(void)arg;

	hal_init();             /* blocks ~250 ms; needs the scheduler */
#if EEPROM_DUMP_ON_BOOT
	eeprom_dump();          /* backup in the console log, before any write */
#endif
	purifier_init();

	wake = last_tick = hb = xTaskGetTickCount();
	for (;;) {
		TickType_t now = xTaskGetTickCount();

		app_alive = now;
		if (now - last_tick >= pdMS_TO_TICKS(APP_TICK_MS)) {
			last_tick = now;
			purifier_tick();
		}

		if (!servers && netio_local_ip_be() != 0) {
			/* First DHCP lease.  Listening sockets survive later link
			 * loss; mDNS re-announces when the IP changes. */
			servers = 1;
			start_servers();
		}
		/* Trial images: no confirm on a timer alone any more.  POST
		 * /confirm (ota_server_poll) confirms; the fallback here needs
		 * >= 5 min up, IP, the OTA server listening (Sol review #3) and
		 * an HA API session unbroken for 60 s.  No-op when confirmed. */
		if (servers)
			ota_trial_poll(netio_local_ip_be() != 0, ota_server_ready(),
				       api_server_client_count());
		/* Boot-confirm timeout must run even if Wi-Fi never comes up
		 * (the likeliest failure of a new image); ota_server_poll()
		 * calls it too, which is harmless. */
		ota_tick();
		if (servers) {
			ota_server_poll();
			api_server_poll();
			mdns_poll();
		}

		if (now - hb >= pdMS_TO_TICKS(HEARTBEAT_MS)) {
			hb = now;
			platform_printf("[app] up %lus, heap free %lu, ip %s\r\n",
					(unsigned long)(platform_millis() / 1000),
					(unsigned long)platform_free_heap(),
					net_mw300_is_up() ? "yes" : "no");
		}
		vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_LOOP_MS));
	}
}

static void net_task(void *arg)
{
	(void)arg;
	if (net_mw300_start(WIFI_SSID, WIFI_PASSWORD, DEVICE_NAME) != 0) {
		/* Radio not usable: the purifier keeps running offline. */
		platform_printf("[net] Wi-Fi unavailable, net task stopped\r\n");
		vTaskDelete(NULL);
	}
	for (;;) {
		net_mw300_service();
		vTaskDelay(pdMS_TO_TICKS(NET_SERVICE_MS));
	}
}

/* hal_reboot() hook (weak default in src/hal_mw300): persist pending
 * settings before an OTA reboot, a confirm-timeout revert or a Wi-Fi reset.
 * Every hal_reboot() caller runs in the app task, like purifier_*(). */
void hal_reboot_hook(void)
{
	if (!guard_reset) {     /* the hang guard runs in the timer task */
		/* replaces a stale reason (e.g. a non-fatal malloc failure) */
		logring_set_reason("reboot requested (OTA, Restart or revert)");
		purifier_flush();
	}
}

int main(void)
{
	/* FIRST, before Wi-Fi, HAL or anything else that could crash or hang:
	 * if this is an unconfirmed image (OTA trial, or the first boot after
	 * a stock/UART install), re-arm the previous slot as the default so a
	 * crash, reset loop or power cycle boots the previous image.  The net
	 * task (prio 3) would otherwise run the radio bring-up before the
	 * prio-2 app task ever reached this point. */
	platform_console_init();
	diag_init();            /* read + clear the reboot tag before anything can crash */
	platform_logf("\r\n[boot] main reached, console up (" FW_VERSION ")\r\n");
	platform_logf("[boot] running ota_boot_check\r\n");
	ota_trial = ota_boot_check();
	platform_printf("[app] ota boot check: %s\r\n",
			ota_trial == 1 ? "TRIAL boot (unconfirmed image)" :
			ota_trial == 0 ? "normal" : "error");

	platform_init();

	if (ota_trial == 1 && ota_trial_revert_armed()) {
		app_alive = xTaskGetTickCount();
		TimerHandle_t g = xTimerCreate("trialg", pdMS_TO_TICKS(TRIAL_GUARD_PERIOD_MS),
					       pdTRUE, NULL, trial_guard);
		if (!g || xTimerStart(g, 0) != pdPASS)
			platform_printf("[app] trial hang guard not started\r\n");
	}

	if (xTaskCreate(app_task, "app", APP_STACK_WORDS, NULL, APP_PRIO, NULL) != pdPASS)
		platform_printf("cannot create app task\r\n");
	if (xTaskCreate(net_task, "net", NET_STACK_WORDS, NULL, NET_PRIO, NULL) != pdPASS)
		platform_printf("cannot create net task\r\n");

	platform_printf("tasks started, heap free %lu\r\n", (unsigned long)platform_free_heap());
	return 0;       /* the SDK deletes the app_start task */
}
