/* wifi_mw300.c - Wi-Fi station bring-up over the WMSDK WLAN manager and
 * FreeRTOS+TCP.
 *
 * Path (same as the SDK reference / stock app):
 *   wm_wlan_init()    sdk core/util/wm_wlan.c: part_init() reads the WMPT
 *                     partition table, picks the active "wififw" (type 2)
 *                     entry and calls wlan_init(&flash_desc); the driver
 *                     reads WLFW+XZ from flash, decompresses and downloads
 *                     it to the radio over SDIO.
 *   wlan_start(cb)    starts the connection manager (wlcmgr) thread;
 *                     cb(WLAN_REASON_INITIALIZED) when ready.
 *   FreeRTOS_IPInit   IP task; net.c (SDK) runs DHCP when wlcmgr asks for
 *                     an address after association.
 *   wlan_set_country  after INITIALIZED, before any connect (stock does the
 *                     same, 0x1f00a790; see NET_WIFI_COUNTRY below).
 *   wlan_add_network + wlan_connect.
 *
 * Stock parity notes: evidence/platform-stock-parity.md.
 *
 * The wlan callback runs in the wlcmgr thread: it only records state; all
 * calls back into the WLAN API happen from net_mw300_service().
 */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"

#include <wmerrno.h>
#include <wmstdio.h>
#include <wlan.h>
#include <wm_wlan.h>
#include <wm_utils.h>
#include <partition.h>
#include <flash.h>
#include <lowlevel_drivers.h>
#include <wlan_11d.h>
#include <stdlib.h>

#include "net_mw300.h"
#include "net_mw300_internal.h"
#include "wifi_policy.h"
#include "secrets.h"

#define NET_NETWORK_NAME   "home"     /* primary profile (net_mw300_start) */
#define NET_BACKUP_NAME    "rescue"   /* optional WIFI_SSID_2 profile */

/* Optional rescue network (ROADMAP R2): secrets.h may define WIFI_SSID_2
 * and WIFI_PASSWORD_2 (password "" or absent = open network).  Absent or
 * "" SSID = no backup: then only the primary profile is ever added and the
 * reconnect rules are the pre-R2 ones (plus the R7 stuck-connect re-kick).
 * Switching rules and timings: wifi_policy.h. */
#ifdef WIFI_SSID_2
#ifndef WIFI_PASSWORD_2
#define WIFI_PASSWORD_2 ""
#endif
static const char backup_ssid[] = WIFI_SSID_2;
static const char backup_password[] = WIFI_PASSWORD_2;
#else
static const char backup_ssid[] = "";
static const char backup_password[] = "";
#endif

/* Regulatory domain.  Stock reads PSM "ot_config.zm_region": "1" ->
 * COUNTRY_US, anything else (or absent) -> COUNTRY_CN (FUN_1f00a9c8 /
 * 0x1f00aeb8); this unit's stock boot log says wlan_set_country(COUNTRY_CN),
 * i.e. channels 1-13.  Without any wlan_set_country() the driver never
 * sends a domain and the radio keeps its own region code (the SDK documents
 * US, channels 1-11, as the default), so an AP on channel 12/13 may never
 * be found.  The tested unit is used in the EU, so the default is
 * COUNTRY_EU (channels 1-13, EU limits) rather than stock's CN.  Override
 * with -DNET_WIFI_COUNTRY=COUNTRY_US etc. (make NET_WIFI_COUNTRY=...).
 * TX power limits come from board_region_pwr_tbl() (platform/board.c,
 * copied from stock). */
#ifndef NET_WIFI_COUNTRY
#define NET_WIFI_COUNTRY COUNTRY_EU
#endif

/* Optional: take the MAC and region from the stock PSM, exactly as stock
 * does.  OFF by default: the SDK libpsm's psm_module_init() always runs its
 * crash recovery, which can erase PSM sectors (read_only does not prevent
 * it in this libpsm version; stock's older libpsm skipped recovery when
 * read_only was set), and the rest of this firmware treats PSM as never
 * written (src/ota guard).  See evidence/platform-stock-parity.md, item 3. */
#ifndef NET_STOCK_PSM_IDENTITY
#define NET_STOCK_PSM_IDENTITY 0
#endif

static SemaphoreHandle_t init_sem;
static volatile int drv_ready;
static volatile int drv_failed;
static volatile int assoc_event;     /* WLAN_REASON_SUCCESS seen, mcast to redo */
static volatile int ip_up;
static volatile uint32_t wlan_events;  /* any wlcmgr event (not PS), for R7 */
static struct wifi_policy policy;
static char primary_ssid[33];          /* for log lines only */
static char hostname[33];
static country_code_t wifi_country = NET_WIFI_COUNTRY;

int net_wifi_driver_ready(void) { return drv_ready; }
int net_mw300_is_up(void) { return ip_up; }

static const char *reason_str(enum wlan_event_reason r)
{
	switch (r) {
	case WLAN_REASON_SUCCESS:             return "connected";
	case WLAN_REASON_CONNECT_FAILED:      return "connect failed";
	case WLAN_REASON_NETWORK_NOT_FOUND:   return "network not found";
	case WLAN_REASON_NETWORK_AUTH_FAILED: return "auth failed";
	case WLAN_REASON_ADDRESS_SUCCESS:     return "address ok";
	case WLAN_REASON_ADDRESS_FAILED:      return "address failed";
	case WLAN_REASON_LINK_LOST:           return "link lost";
	case WLAN_REASON_USER_DISCONNECT:     return "disconnected";
	case WLAN_REASON_INITIALIZED:         return "initialized";
	case WLAN_REASON_INITIALIZATION_FAILED: return "init failed";
	default:                              return "event";
	}
}

static int wlan_event_cb(enum wlan_event_reason reason, void *data)
{
	(void)data;
	switch (reason) {
	case WLAN_REASON_INITIALIZED:
		drv_ready = 1;
		xSemaphoreGive(init_sem);
		break;
	case WLAN_REASON_INITIALIZATION_FAILED:
		drv_failed = 1;
		xSemaphoreGive(init_sem);
		break;
	case WLAN_REASON_SUCCESS:
		assoc_event = 1;
		break;
	case WLAN_REASON_PS_ENTER:
	case WLAN_REASON_PS_EXIT:
		return 0;
	default:
		break;
	}
	wlan_events++;
	wmprintf("[net] wlan: %s (%d)\r\n", reason_str(reason), (int)reason);
	return 0;
}

/* ---- FreeRTOS+TCP application hooks ---- */

void vApplicationIPNetworkEventHook(eIPCallbackEvent_t eNetworkEvent)
{
	if (eNetworkEvent == eNetworkUp) {
		uint32_t ip, mask, gw, dns;
		char a[16], m[16], g[16];
		FreeRTOS_GetAddressConfiguration(&ip, &mask, &gw, &dns);
		FreeRTOS_inet_ntoa(ip, a);
		FreeRTOS_inet_ntoa(mask, m);
		FreeRTOS_inet_ntoa(gw, g);
		wmprintf("[net] IP up: %s mask %s gw %s\r\n", a, m, g);
		ip_up = 1;
	} else {
		if (ip_up)
			wmprintf("[net] IP down\r\n");
		ip_up = 0;
	}
}

const char *pcApplicationHostnameHook(void)
{
	return hostname;
}

/* DHCP transaction IDs, TCP ISNs, ephemeral ports. */
BaseType_t xApplicationGetRandomNumber(uint32_t *pulNumber)
{
	get_random_sequence(pulNumber, sizeof(*pulNumber));
	return pdTRUE;
}

uint32_t ulRand(void)
{
	uint32_t r;
	get_random_sequence(&r, sizeof(r));
	return r;
}

uint32_t ulApplicationGetNextSequenceNumber(uint32_t ulSourceAddress, uint16_t usSourcePort,
					    uint32_t ulDestinationAddress, uint16_t usDestinationPort)
{
	(void)ulSourceAddress; (void)usSourcePort;
	(void)ulDestinationAddress; (void)usDestinationPort;
	return ulRand();
}

/* ---- bring-up ---- */

static int add_network(const char *name, const char *ssid, const char *password)
{
	struct wlan_network nw;
	int ret;

	memset(&nw, 0, sizeof(nw));
	strlcpy(nw.name, name, sizeof(nw.name));
	strlcpy(nw.ssid, ssid, sizeof(nw.ssid));
	if (password && password[0]) {
		/* Match whatever WPA/WPA2 mode the AP advertises.  If an AP
		 * refuses this, try WLAN_SECURITY_WPA2 here. */
		nw.security.type = WLAN_SECURITY_WILDCARD;
		strlcpy(nw.security.psk, password, sizeof(nw.security.psk));
		nw.security.psk_len = strlen(nw.security.psk);
	} else {
		nw.security.type = WLAN_SECURITY_NONE;
	}
	nw.type = WLAN_BSS_TYPE_STA;
	nw.role = WLAN_BSS_ROLE_STA;
	nw.ip.ipv4.addr_type = ADDR_TYPE_DHCP;

	(void)wlan_remove_network(name);
	ret = wlan_add_network(&nw);
	memset(&nw.security, 0, sizeof(nw.security));   /* wlcmgr keeps its copy */
	if (ret != WM_SUCCESS)
		wmprintf("[net] wlan_add_network(%s) failed %d\r\n", name, ret);
	return ret;
}

static const char *net_profile(enum wp_net n)
{
	return n == WP_BACKUP ? NET_BACKUP_NAME : NET_NETWORK_NAME;
}

static const char *net_ssid(enum wp_net n)
{
	return n == WP_BACKUP ? backup_ssid : primary_ssid;
}

static uint32_t now_ms(void)
{
	return (uint32_t)xTaskGetTickCount() * (1000u / configTICK_RATE_HZ);
}

#if WIFI_FW_EMBED
/* Radio firmware embedded in our own image (tools/gen_wlfw.py).  The XIP
 * window 0x1F000000 maps the running appfw slot, so the blob's flash offset
 * is slot_start + (addr - 0x1F000000).  We don't trust "active = higher gen"
 * blindly (boot2 might have fallen back): each appfw slot is checked by
 * reading the WLFW header back from flash and comparing it with the XIP
 * copy.  The slot the FLASHC offset says is executing is tried first (and
 * exclusively, when the offset is known): the other slot may hold a
 * different build whose blob sits at the same offset with the same header
 * (e.g. an aborted OTA), and streaming that to the radio would fail. */
extern const uint8_t wlfw_embedded[];
extern const uint32_t wlfw_embedded_len;
#define XIP_BASE 0x1F000000u

static int wlan_init_embedded(void)
{
	short history = 0;
	struct partition_entry *slot[2];
	uint32_t rel = (uint32_t)wlfw_embedded - XIP_BASE;
	uint8_t buf[32];
	int ret = part_init();

	if (ret != WM_SUCCESS)
		return ret;
	slot[0] = part_get_layout_by_id(FC_COMP_FW, &history);
	slot[1] = part_get_layout_by_id(FC_COMP_FW, &history);
	int have_off = FLASHC->FCACR.BF.OFFSET_EN;
	uint32_t run_off = FLASHC->FAOFFR.BF.OFFSET_VAL;
	flash_drv_init();
	for (int i = 0; i < 2; i++) {
		struct partition_entry *p = slot[i];
		mdev_t *fl;

		if (!p || rel + wlfw_embedded_len > p->size)
			continue;
		if (have_off && p->start != run_off)
			continue;
		fl = flash_drv_open(p->device);
		if (!fl)
			continue;
		ret = flash_drv_read(fl, buf, sizeof(buf), p->start + rel);
		flash_drv_close(fl);
		if (ret != WM_SUCCESS || memcmp(buf, wlfw_embedded, sizeof(buf)) != 0)
			continue;
		flash_desc_t desc = {
			.fl_dev = p->device,
			.fl_start = p->start + rel,
			.fl_size = wlfw_embedded_len,
		};
		wmprintf("[net] radio fw embedded (W14.88.36.p135) at flash 0x%x, slot %s@0x%x\r\n",
			 (unsigned)desc.fl_start, p->name, (unsigned)p->start);
		return wlan_init(&desc);
	}
	wmprintf("[net] embedded radio fw not found in either appfw slot\r\n");
	return -WM_FAIL;
}
#endif

#if NET_STOCK_PSM_IDENTITY
#include <psm-v2.h>

/* wlan_set_mac_addr() keeps the pointer until sd_wifi_init() applies it. */
static uint8_t stock_mac[6];

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Stock FUN_1f006034 (MAC) and 0x1f00aeb8 (region), via libpsm.  Keys are
 * "<module>.<var>" (stock helper 0x1f023bdc formats "%s.%s").  Must run
 * before wlan_init(): the MAC is then programmed by sd_wifi_init() and
 * wlcmgr caches it, as in stock (which calls it before wm_wlan_init).
 * Logs only the MAC (printed later anyway) and the region number. */
static void stock_psm_identity(void)
{
	struct partition_entry *p;
	flash_desc_t fl;
	psm_hnd_t h = NULL;
	psm_cfg_t cfg;
	char buf[16];
	int n, i;

	if (part_init() != WM_SUCCESS)
		return;
	p = part_get_layout_by_id(FC_COMP_PSM, NULL);
	if (!p)
		return;
	part_to_flash_desc(p, &fl);
	memset(&cfg, 0, sizeof(cfg));
	/* read_only also gives the same usable-area limit stock computes
	 * (stock passes cfg = NULL: no swap sector reserved, 0x1f023934). */
	cfg.read_only = true;
	if (psm_module_init(&fl, &h, &cfg) != WM_SUCCESS) {
		wmprintf("[net] stock PSM not readable\r\n");
		return;
	}
	n = psm_get_variable(h, "ot_config.psm_mac", buf, sizeof(buf) - 1);
	if (n == 12) {
		for (i = 0; i < 6; i++) {
			int hi = hexval(buf[2 * i]), lo = hexval(buf[2 * i + 1]);
			if (hi < 0 || lo < 0)
				break;
			stock_mac[i] = (uint8_t)(hi << 4 | lo);
		}
		if (i == 6 && !(stock_mac[0] & 1)) {
			wlan_set_mac_addr(stock_mac);
			wmprintf("[net] MAC from stock PSM\r\n");
		}
	}
	n = psm_get_variable(h, "ot_config.zm_region", buf, sizeof(buf) - 1);
	buf[n > 0 ? n : 0] = 0;
	wifi_country = (n > 0 && strtol(buf, NULL, 10) == 1) ? COUNTRY_US : COUNTRY_CN;
	memset(buf, 0, sizeof(buf));
	psm_module_deinit(&h);
}
#endif

int net_mw300_start(const char *ssid, const char *password, const char *hostname_in)
{
	static const uint8_t zero4[4] = { 0, 0, 0, 0 };
	uint8_t mac[6];
	int ret, have_backup = 0;

	strlcpy(hostname, hostname_in ? hostname_in : "mw300", sizeof(hostname));

	net_mcast_init();
	init_sem = xSemaphoreCreateBinary();
	if (!init_sem)
		return -WM_E_NOMEM;

#if NET_STOCK_PSM_IDENTITY
	stock_psm_identity();
#endif
#if WIFI_FW_EMBED
	ret = wlan_init_embedded();
#else
	wmprintf("[net] loading Wi-Fi firmware from the wififw partition\r\n");
	ret = wm_wlan_init();
#endif
	if (ret != WM_SUCCESS) {
		wmprintf("[net] wm_wlan_init failed %d\r\n", ret);
		return ret;
	}
	ret = wlan_start(wlan_event_cb);
	if (ret != WM_SUCCESS) {
		wmprintf("[net] wlan_start failed %d\r\n", ret);
		return ret;
	}
	if (xSemaphoreTake(init_sem, pdMS_TO_TICKS(30000)) != pdTRUE || drv_failed) {
		wmprintf("[net] WLAN manager did not initialize\r\n");
		return -WM_FAIL;
	}

	memset(mac, 0, sizeof(mac));
	(void)wlan_get_mac_address(mac);
	/* Only the OUI: console output also leaves the device (R3 remote
	 * logs, HA's log viewer), and the full MAC identifies the unit. */
	wmprintf("[net] MAC %02x:%02x:%02x:xx:xx:xx\r\n", mac[0], mac[1], mac[2]);
	if ((mac[0] & 1) || !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]))
		wmprintf("[net] WARNING: radio MAC is not a valid unicast address\r\n");

	/* After INITIALIZED, before any scan/connect (wlan_11d.h, stock order). */
	ret = wlan_set_country(wifi_country);
	wmprintf("[net] country %s (%d): %d\r\n",
		 wifi_get_country_str(wifi_country), (int)wifi_country, ret);

	if (FreeRTOS_IPInit(zero4, zero4, zero4, zero4, mac) != pdPASS) {
		wmprintf("[net] FreeRTOS_IPInit failed\r\n");
		return -WM_FAIL;
	}

	net_mcast_apply_hw();           /* groups joined before Wi-Fi was up */

	strlcpy(primary_ssid, ssid, sizeof(primary_ssid));
	wmprintf("[net] connecting to \"%s\"\r\n", ssid);
	ret = add_network(NET_NETWORK_NAME, ssid, password);
	if (backup_ssid[0]) {
		/* Added while DISCONNECTED, before the first connect
		 * (wlan_add_network's rule); wlcmgr only uses it when
		 * wifi_policy switches to it by name.  Added even if the
		 * primary profile was rejected (e.g. a bad password length),
		 * so the rescue still works then. */
		if (add_network(NET_BACKUP_NAME, backup_ssid, backup_password) == WM_SUCCESS) {
			have_backup = 1;
			wmprintf("[net] backup network \"%s\" configured\r\n", backup_ssid);
		} else {
			wmprintf("[net] backup network unusable, primary only\r\n");
		}
	}
	if (ret == WM_SUCCESS)
		ret = wlan_connect(NET_NETWORK_NAME);
	if (ret != WM_SUCCESS)
		wmprintf("[net] wlan_connect failed %d (will retry)\r\n", ret);
	wifi_policy_init(&policy, have_backup, now_ms());
	return WM_SUCCESS;
}

int net_mw300_service(void)
{
	enum wlan_connection_state st;

	if (!drv_ready)
		return 0;

	if (assoc_event) {
		assoc_event = 0;
		net_mcast_apply_hw();
	}

	if (wlan_get_connection_state(&st) == WM_SUCCESS) {
		enum wp_link link;
		struct wp_action a;
		int ret;

		switch (st) {
		case WLAN_DISCONNECTED:
			link = WP_LINK_DOWN;
			break;
		case WLAN_CONNECTING:
		case WLAN_SCANNING:
		case WLAN_ASSOCIATING:
		case WLAN_ASSOCIATED:
			link = WP_LINK_BUSY;
			break;
		default:
			link = WP_LINK_UP;
			break;
		}
		a = wifi_policy_step(&policy, now_ms(), link, ip_up, wlan_events);
		if (a.act == WP_NONE)
			return ip_up;
		if (a.act == WP_KICK_DOWN)
			/* wlcmgr gave up (it retries on its own after link
			 * loss first); kick it again.  Same line as pre-R2. */
			wmprintf("[net] reconnecting\r\n");
		else if (a.act == WP_KICK_STUCK)
			wmprintf("[net] %s (state %d)\r\n", wifi_policy_act_str(a.act), (int)st);
		else if (a.act == WP_SWITCH_FAIL || a.act == WP_SWITCH_PROBE)
			/* The SSID only, never the password. */
			wmprintf("[net] %s: now %s \"%s\"\r\n", wifi_policy_act_str(a.act),
				 a.net == WP_BACKUP ? "backup" : "primary", net_ssid(a.net));
		else    /* WP_HOLD */
			wmprintf("[net] %s\r\n", wifi_policy_act_str(a.act));
		if (a.act != WP_HOLD) {
			/* wlan_connect() cancels an attempt in progress or
			 * disconnects first (wlan.h), so it is valid in every
			 * state. */
			ret = wlan_connect((char *)net_profile(a.net));
			if (ret != WM_SUCCESS) {
				wmprintf("[net] wlan_connect failed %d\r\n", ret);
				wifi_policy_kick_failed(&policy);
			}
		}
	}

	return ip_up;
}
