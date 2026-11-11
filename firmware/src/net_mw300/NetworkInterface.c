/* NetworkInterface.c - FreeRTOS+TCP <-> WMSDK Wi-Fi driver glue.
 *
 * Based on amazon-freertos libraries/freertos_plus/standard/freertos_plus_tcp/
 * portable/NetworkInterface/mw300_rd/NetworkInterface.c (MIT), with one
 * functional change: the driver does the frame/IP filtering
 * (ipconfigETHERNET_DRIVER_FILTERS_PACKETS = 1) so joined IPv4 multicast
 * groups (mDNS 224.0.0.251) reach the stack.  FreeRTOS+TCP 2.3.2 itself only
 * admits unicast, broadcast and LLMNR.
 *
 * The SDK glue sdk/src/platform/net/freertos-plus-tcp/net.c (compiled
 * unchanged into libsdk.a) calls vNetworkNotifyIFUp/Down() and registers
 * handle_data_packet() with the Wi-Fi driver.
 */
#include <string.h>

#include "FreeRTOS.h"
#include "list.h"
#include "semphr.h"
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "FreeRTOS_IP_Private.h"
#include "NetworkBufferManagement.h"
#include "NetworkInterface.h"

#include "wifi-decl.h"
#include "wmerrno.h"
#include "wifi.h"
#include <wmlog.h>

#include "net_mw300_internal.h"

#define net_e(...) wmlog_e("freertos_tcp", ##__VA_ARGS__)

enum { INTERFACE_DOWN = 0, INTERFACE_UP };

/* TX staging buffer owned by libwifidriver (TxPD + interface header room). */
extern uint8_t outbuf[2048];
extern t_u8 *mlan_get_payload(const t_u8 *rcvdata, t_u16 *payload_len, int *interface);

static volatile uint32_t xInterfaceState = INTERFACE_DOWN;

/* ---- multicast group table (shared with netio_mw300.c) ---- */
static volatile uint32_t mcast_groups[NET_MAX_MCAST_GROUPS];   /* network order, 0 = free */

/* join (app task) and apply (net task) can race; the RX path only reads. */
static SemaphoreHandle_t mcast_mtx;

void net_mcast_init(void)
{
	if (!mcast_mtx)
		mcast_mtx = xSemaphoreCreateMutex();
}

static void mcast_lock(void)
{
	if (mcast_mtx)
		xSemaphoreTake(mcast_mtx, portMAX_DELAY);
}

static void mcast_unlock(void)
{
	if (mcast_mtx)
		xSemaphoreGive(mcast_mtx);
}

static void mcast_apply_locked(void);

static void mcast_mac(uint32_t group_be, uint8_t mac[6])
{
	const uint8_t *ip = (const uint8_t *)&group_be;   /* network order bytes */
	mac[0] = 0x01; mac[1] = 0x00; mac[2] = 0x5e;
	mac[3] = ip[1] & 0x7f; mac[4] = ip[2]; mac[5] = ip[3];
}

int net_mcast_join(uint32_t group_be)
{
	int i, free_slot = -1, ret = 0;
	mcast_lock();
	for (i = 0; i < NET_MAX_MCAST_GROUPS; i++) {
		if (mcast_groups[i] == group_be)
			goto out;
		if (mcast_groups[i] == 0 && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0) {
		ret = -1;
		goto out;
	}
	mcast_groups[free_slot] = group_be;
	mcast_apply_locked();
out:
	mcast_unlock();
	return ret;
}

/* Program the Wi-Fi firmware's multicast MAC filter for every joined
 * group.  Called on join and again after each (re)association, because it
 * is not known whether the firmware keeps the list across reconnects.
 * Must not be called from the wlcmgr callback context (it sends commands). */
void net_mcast_apply_hw(void)
{
	mcast_lock();
	mcast_apply_locked();
	mcast_unlock();
}

/* Hardware (Wi-Fi driver) multicast filter state per group.  NEVER call
 * wifi_remove_mcast_filter() for an address we did not add: on real
 * hardware (2026-11-04, diag1) the prebuilt driver's remove_mcast_ip() ->
 * cmp_mac_addr() walked an invalid list and took a precise bus fault
 * (BFAR 0xF8DF4770) right after Wi-Fi came up.  The driver keeps its own
 * host-side list, so each group is added exactly once per boot. */
static uint8_t mcast_hw_added[NET_MAX_MCAST_GROUPS];

static void mcast_apply_locked(void)
{
	int i;
	uint8_t mac[6];
	if (!net_wifi_driver_ready())
		return;                 /* applied later from the net task */
	for (i = 0; i < NET_MAX_MCAST_GROUPS; i++) {
		if (!mcast_groups[i] || mcast_hw_added[i])
			continue;
		mcast_mac(mcast_groups[i], mac);
		if (wifi_add_mcast_filter(mac) == WM_SUCCESS)
			mcast_hw_added[i] = 1;
		else
			net_e("mcast filter add failed");
	}
}

static int is_joined_group(uint32_t ip_be)
{
	int i;
	for (i = 0; i < NET_MAX_MCAST_GROUPS; i++)
		if (mcast_groups[i] && mcast_groups[i] == ip_be)
			return 1;
	return 0;
}

static int is_joined_group_mac(const uint8_t *mac)
{
	int i;
	uint8_t m[6];
	for (i = 0; i < NET_MAX_MCAST_GROUPS; i++) {
		if (!mcast_groups[i])
			continue;
		mcast_mac(mcast_groups[i], m);
		if (memcmp(m, mac, 6) == 0)
			return 1;
	}
	return 0;
}

/* Replaces eConsiderFrameForProcessing() + the IP checks the stack skips
 * when ipconfigETHERNET_DRIVER_FILTERS_PACKETS == 1 (prvAllowIPPacket). */
static int frame_wanted(const uint8_t *frame, size_t len)
{
	static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	uint16_t type;

	if (len < 14)
		return 0;
	if (memcmp(frame, ipLOCAL_MAC_ADDRESS, 6) != 0 &&
	    memcmp(frame, bcast, 6) != 0 &&
	    !is_joined_group_mac(frame))
		return 0;

	type = (uint16_t)((frame[12] << 8) | frame[13]);
	if (type == 0x0806)                     /* ARP */
		return 1;
	if (type != 0x0800)                     /* IPv6, non-Ethernet-II, ... */
		return 0;

	if (len < 14 + 20)
		return 0;
	{
		const uint8_t *ip = frame + 14;
		uint32_t dst, mine;
		if (ip[0] < 0x45 || ip[0] > 0x4f)       /* IPv4, IHL 5..15 */
			return 0;
		if (((ip[6] << 8) | ip[7]) & 0x3fff)    /* MF or offset: no reassembly */
			return 0;
		memcpy(&dst, ip + 16, 4);
		mine = *ipLOCAL_IP_ADDRESS_POINTER;
		if (dst == mine || dst == ipBROADCAST_IP_ADDRESS ||
		    dst == xNetworkAddressing.ulBroadcastAddress ||
		    mine == 0 ||                        /* DHCP in progress */
		    is_joined_group(dst))
			return 1;
	}
	return 0;
}

static int process_data_packet(const t_u8 *databuf, const t_u16 datalen)
{
	int interface = BSS_TYPE_STA;
	t_u8 *payload;
	t_u16 payload_len = 0;
	const TickType_t xDescriptorWaitTime = pdMS_TO_TICKS(250);
	NetworkBufferDescriptor_t *pxNetworkBuffer;
	IPStackEvent_t xRxEvent = { eNetworkRxEvent, NULL };

	payload = (t_u8 *)mlan_get_payload(databuf, &payload_len, &interface);

	if (!frame_wanted(payload, payload_len))
		return WM_SUCCESS;

	pxNetworkBuffer = pxGetNetworkBufferWithDescriptor(datalen, xDescriptorWaitTime);
	if (pxNetworkBuffer != NULL) {
		pxNetworkBuffer->xDataLength = payload_len;
		memcpy(pxNetworkBuffer->pucEthernetBuffer, payload, payload_len);
		xRxEvent.pvData = (void *)pxNetworkBuffer;
		if (xSendEventStructToIPTask(&xRxEvent, xDescriptorWaitTime) == pdFAIL) {
			vReleaseNetworkBufferAndDescriptor(pxNetworkBuffer);
			return -WM_FAIL;
		}
	}
	return WM_SUCCESS;
}

/* Callback from the Wi-Fi driver (registered by net.c:net_wlan_init()). */
void handle_data_packet(const t_u8 interface, const t_u8 *rcvdata, const t_u16 datalen)
{
	if (interface == BSS_TYPE_STA)
		process_data_packet(rcvdata, datalen);
}

BaseType_t xNetworkInterfaceInitialise(void)
{
	int ret;
	mac_addr_t mac_addr;

	if (!net_wifi_driver_ready())
		return pdFALSE;         /* IP task retries every few seconds */
	ret = wifi_get_device_mac_addr(&mac_addr);
	if (ret == WM_SUCCESS)
		FreeRTOS_UpdateMACAddress(mac_addr.mac);

	return (xInterfaceState == INTERFACE_UP && ret == WM_SUCCESS) ? pdTRUE : pdFALSE;
}

void vNetworkInterfaceAllocateRAMToBuffers(
	NetworkBufferDescriptor_t pxNetworkBuffers[ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS])
{
	(void)pxNetworkBuffers;         /* BufferAllocation_2: heap buffers */
}

BaseType_t xGetPhyLinkStatus(void)
{
	return xInterfaceState == INTERFACE_UP ? pdTRUE : pdFALSE;
}

void vNetworkNotifyIFDown(void)
{
	IPStackEvent_t xRxEvent = { eNetworkDownEvent, NULL };

	xInterfaceState = INTERFACE_DOWN;
	if (xSendEventStructToIPTask(&xRxEvent, 0) != pdPASS)
		net_e("Could not send network down event");
}

void vNetworkNotifyIFUp(void)
{
	xInterfaceState = INTERFACE_UP;
}

BaseType_t xNetworkInterfaceOutput(NetworkBufferDescriptor_t *const pxNetworkBuffer,
				   BaseType_t xReleaseAfterSend)
{
	const size_t hdr = 22 + 4;      /* sizeof(TxPD) + INTF_HEADER_LEN */
	int ret;

	if (pxNetworkBuffer == NULL || pxNetworkBuffer->pucEthernetBuffer == NULL ||
	    pxNetworkBuffer->xDataLength == 0 ||
	    pxNetworkBuffer->xDataLength > sizeof(outbuf) - hdr) {
		if (pxNetworkBuffer != NULL && xReleaseAfterSend != pdFALSE)
			vReleaseNetworkBufferAndDescriptor(pxNetworkBuffer);
		return pdFALSE;
	}

	memset(outbuf, 0x00, hdr);
	memcpy(outbuf + hdr, pxNetworkBuffer->pucEthernetBuffer, pxNetworkBuffer->xDataLength);
	ret = wifi_low_level_output(BSS_TYPE_STA, outbuf + hdr, pxNetworkBuffer->xDataLength);
	if (ret != WM_SUCCESS)
		net_e("Failed output len %d, error %d", (int)pxNetworkBuffer->xDataLength, ret);

	if (xReleaseAfterSend != pdFALSE)
		vReleaseNetworkBufferAndDescriptor(pxNetworkBuffer);

	return ret == WM_SUCCESS ? pdTRUE : pdFALSE;
}
