/* netio_mw300.c - include/netio.h over FreeRTOS+TCP 2.3.2.
 *
 * Handles are small integers indexing a table of FreeRTOS sockets.  All
 * sockets get zero send/receive timeouts, so every call is non-blocking.
 *
 * Threading: all netio_* socket calls must come from ONE task (the app
 * task that polls the servers).  The handle table is not locked.
 * netio_local_ip_be/netio_mac may be called from any task; netio_rssi
 * only from the API/app task (unlocked statics).
 *
 * Close semantics: netio_close() on a connected TCP socket does a graceful
 * shutdown (FIN after queued data is sent) and frees the socket later from
 * netio_reap() (run from netio_tcp_accept() and netio_close(), i.e. on
 * every server poll), so a final reply, e.g.
 * "HTTP 200" before an OTA reboot, is not lost.
 */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"

#include <wmerrno.h>
#include <wmstdio.h>
#include <wlan.h>

#include "netio.h"
#include "net_mw300.h"
#include "net_mw300_internal.h"

#define NETIO_MAX_SOCKETS   12
#define CLOSE_LINGER_MS     3000
#define RSSI_REFRESH_MS     5000


enum { SLOT_FREE = 0, SLOT_TCP_LISTEN, SLOT_TCP, SLOT_UDP, SLOT_CLOSING };

static struct {
	Socket_t s;
	uint8_t kind;
	TickType_t close_start;
} tab[NETIO_MAX_SOCKETS];

static int slot_alloc(Socket_t s, uint8_t kind)
{
	int i;
	for (i = 0; i < NETIO_MAX_SOCKETS; i++) {
		if (tab[i].kind == SLOT_FREE) {
			tab[i].s = s;
			tab[i].kind = kind;
			return i;
		}
	}
	return -1;
}

static Socket_t slot_get(netio_sock h, uint8_t kind)
{
	if (h < 0 || h >= NETIO_MAX_SOCKETS || tab[h].kind != kind)
		return NULL;
	return tab[h].s;
}

static void set_nonblocking(Socket_t s)
{
	static const TickType_t zero = 0;
	(void)FreeRTOS_setsockopt(s, 0, FREERTOS_SO_RCVTIMEO, &zero, sizeof(zero));
	(void)FreeRTOS_setsockopt(s, 0, FREERTOS_SO_SNDTIMEO, &zero, sizeof(zero));
}

static int valid(Socket_t s)
{
	return s != FREERTOS_INVALID_SOCKET && s != NULL;
}

netio_sock netio_tcp_listen(uint16_t port)
{
	struct freertos_sockaddr addr;
	Socket_t s;
	int h;

	s = FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_STREAM, FREERTOS_IPPROTO_TCP);
	if (!valid(s))
		return -1;
	set_nonblocking(s);
	memset(&addr, 0, sizeof(addr));
	addr.sin_port = FreeRTOS_htons(port);
	if (FreeRTOS_bind(s, &addr, sizeof(addr)) != 0 || FreeRTOS_listen(s, 4) != 0) {
		FreeRTOS_closesocket(s);
		return -1;
	}
	h = slot_alloc(s, SLOT_TCP_LISTEN);
	if (h < 0)
		FreeRTOS_closesocket(s);
	return h;
}

static void netio_reap(void);

netio_sock netio_tcp_accept(netio_sock listener)
{
	struct freertos_sockaddr peer;
	socklen_t len = sizeof(peer);
	Socket_t ls = slot_get(listener, SLOT_TCP_LISTEN), c;
	int h;

	netio_reap();
	if (!ls)
		return -1;
	c = FreeRTOS_accept(ls, &peer, &len);
	if (!valid(c))
		return NETIO_AGAIN;
	set_nonblocking(c);
	h = slot_alloc(c, SLOT_TCP);
	if (h < 0) {
		/* Table full: refuse this client. */
		FreeRTOS_closesocket(c);
		return NETIO_AGAIN;
	}
	return h;
}

int netio_recv(netio_sock h, void *buf, size_t len)
{
	Socket_t s = slot_get(h, SLOT_TCP);
	BaseType_t r;

	if (!s)
		return -1;
	if (len == 0)
		return NETIO_AGAIN;
	r = FreeRTOS_recv(s, buf, len, 0);
	if (r > 0)
		return (int)r;
	if (r == 0)
		return NETIO_AGAIN;              /* no data yet */
	if (r == -pdFREERTOS_ERRNO_ENOTCONN)
		return 0;                        /* peer closed, buffer drained */
	return -1;                               /* ENOMEM, EINVAL, ... */
}

int netio_send(netio_sock h, const void *buf, size_t len)
{
	Socket_t s = slot_get(h, SLOT_TCP);
	BaseType_t r;

	if (!s)
		return -1;
	if (len == 0)
		return 0;
	r = FreeRTOS_send(s, buf, len, 0);
	if (r > 0)
		return (int)r;
	if (r == 0 || r == -pdFREERTOS_ERRNO_ENOSPC)
		return NETIO_AGAIN;              /* TX stream full */
	return -1;                               /* ENOTCONN / ENOMEM */
}

void netio_close(netio_sock h)
{
	netio_reap();
	if (h < 0 || h >= NETIO_MAX_SOCKETS)
		return;
	switch (tab[h].kind) {
	case SLOT_TCP:
		if (FreeRTOS_issocketconnected(tab[h].s) == pdTRUE) {
			(void)FreeRTOS_shutdown(tab[h].s, FREERTOS_SHUT_RDWR);
			tab[h].kind = SLOT_CLOSING;
			tab[h].close_start = xTaskGetTickCount();
			return;                  /* freed by netio_reap() */
		}
		/* fall through */
	case SLOT_TCP_LISTEN:
	case SLOT_UDP:
		FreeRTOS_closesocket(tab[h].s);
		tab[h].s = NULL;
		tab[h].kind = SLOT_FREE;
		break;
	default:
		break;
	}
}

static void netio_reap(void)
{
	static uint8_t drain[64];
	int i;
	for (i = 0; i < NETIO_MAX_SOCKETS; i++) {
		if (tab[i].kind != SLOT_CLOSING)
			continue;
		/* Drain anything the peer still sends so its FIN can arrive. */
		BaseType_t r = FreeRTOS_recv(tab[i].s, drain, sizeof(drain), 0);
		if (r < 0 || FreeRTOS_issocketconnected(tab[i].s) != pdTRUE ||
		    xTaskGetTickCount() - tab[i].close_start > pdMS_TO_TICKS(CLOSE_LINGER_MS)) {
			FreeRTOS_closesocket(tab[i].s);
			tab[i].s = NULL;
			tab[i].kind = SLOT_FREE;
		}
	}
}

/* ---- UDP ---- */

netio_sock netio_udp_bind(uint16_t port, uint32_t mcast_group_be)
{
	struct freertos_sockaddr addr;
	Socket_t s;
	int h;

	s = FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_DGRAM, FREERTOS_IPPROTO_UDP);
	if (!valid(s))
		return -1;
	set_nonblocking(s);
	memset(&addr, 0, sizeof(addr));
	addr.sin_port = FreeRTOS_htons(port);
	if (FreeRTOS_bind(s, &addr, sizeof(addr)) != 0) {
		FreeRTOS_closesocket(s);
		return -1;
	}
	if (mcast_group_be != 0 && net_mcast_join(mcast_group_be) != 0) {
		FreeRTOS_closesocket(s);
		return -1;
	}
	h = slot_alloc(s, SLOT_UDP);
	if (h < 0)
		FreeRTOS_closesocket(s);
	return h;
}

int netio_udp_recvfrom(netio_sock h, void *buf, size_t len,
		       uint32_t *src_ip_be, uint16_t *src_port)
{
	struct freertos_sockaddr from;
	socklen_t flen = sizeof(from);
	Socket_t s = slot_get(h, SLOT_UDP);
	int32_t r;

	if (!s)
		return -1;
	r = FreeRTOS_recvfrom(s, buf, len, 0, &from, &flen);
	if (r > 0) {
		if (src_ip_be)
			*src_ip_be = from.sin_addr;
		if (src_port)
			*src_port = FreeRTOS_ntohs(from.sin_port);
		return (int)r;
	}
	if (r == 0 || r == -pdFREERTOS_ERRNO_EWOULDBLOCK)
		return NETIO_AGAIN;
	return -1;
}

int netio_udp_sendto(netio_sock h, const void *buf, size_t len,
		     uint32_t dst_ip_be, uint16_t dst_port)
{
	struct freertos_sockaddr to;
	Socket_t s = slot_get(h, SLOT_UDP);
	int32_t r;

	if (!s)
		return -1;
	if (!net_mw300_is_up())
		return NETIO_AGAIN;
	memset(&to, 0, sizeof(to));
	to.sin_addr = dst_ip_be;
	to.sin_port = FreeRTOS_htons(dst_port);
	r = FreeRTOS_sendto(s, buf, len, 0, &to, sizeof(to));
	if (r > 0)
		return (int)r;
	return NETIO_AGAIN;                      /* no buffer / ARP pending */
}

/* ---- status ---- */

uint32_t netio_local_ip_be(void)
{
	return net_mw300_is_up() ? FreeRTOS_GetIPAddress() : 0;
}

/*
 * RSSI (evidence/rssi-unknown.md).
 *
 * Primary source, same as stock (appfwA 0x1f02d8f0): wlan_get_current_rssi(),
 * which only reads the SNR/NF the driver saved from the RxPD of the last
 * received data frame (mlan_get_payload(), called for every frame in
 * NetworkInterface.c).  No firmware command, no command lock, cannot block.
 * It returns (u8)(snr - nf) - 256, i.e. -256 before any frame arrived.
 *
 * No fallback to wlan_get_current_signal_strength(): it sends RSSI_INFO
 * (0xa4), the prebuilt libwlcmgr ignores the command's result (returns a
 * stale/zero value as success) and it can block the caller up to 10 s on a
 * command timeout (evidence/feature-gaps-review.md).  Until the first data
 * frame the entity simply stays unknown.
 *
 * A transient bad reading keeps the last good value for RSSI_HOLD_MS.
 * Link gate: IP up or wlcmgr associated/connecting/connected, not only
 * WLAN_CONNECTED (the API can be serving while wlcmgr is mid-renegotiation).
 */
#define RSSI_HOLD_MS        300000
#define RSSI_MIN_DBM        (-120)
#define RSSI_MAX_DBM        (-1)

static int rssi_plausible(int r)
{
	return r >= RSSI_MIN_DBM && r <= RSSI_MAX_DBM;
}

static int rssi_link_ok(void)
{
	enum wlan_connection_state st;

	if (net_mw300_is_up())
		return 1;
	if (wlan_get_connection_state(&st) != WM_SUCCESS)
		return 0;
	return st == WLAN_CONNECTED || st == WLAN_ASSOCIATED || st == WLAN_CONNECTING;
}

int netio_rssi(void)
{
	static TickType_t last_query, last_good;
	static int cached;              /* last good dBm, 0 = none */
	static int8_t logged_src = -1;  /* console: log source changes once */
	TickType_t now;
	short d = 0;
	int src;

	if (!net_wifi_driver_ready() || !rssi_link_ok()) {
		cached = 0;
		logged_src = -1;
		return 0;
	}
	now = xTaskGetTickCount();
	if (cached != 0 && now - last_query < pdMS_TO_TICKS(RSSI_REFRESH_MS))
		return cached;

	/* Passive read of a driver field: cheap, may be retried every call. */
	src = wlan_get_current_rssi(&d) == WM_SUCCESS && rssi_plausible(d);
	if (src) {
		cached = d;
		last_good = now;
		last_query = now;
	} else if (cached != 0 && now - last_good > pdMS_TO_TICKS(RSSI_HOLD_MS)) {
		cached = 0;             /* nothing valid for too long: unknown */
	}
	if (src != logged_src) {
		logged_src = (int8_t)src;
		wmprintf("[net] rssi %d dBm (%s)\r\n", src ? (int)d : cached,
			 src ? "data" : cached ? "held" : "no valid source");
	}
	return cached;
}

void netio_mac(uint8_t mac[6])
{
	if (!net_wifi_driver_ready() || wlan_get_mac_address(mac) != WM_SUCCESS)
		memset(mac, 0, 6);
}
