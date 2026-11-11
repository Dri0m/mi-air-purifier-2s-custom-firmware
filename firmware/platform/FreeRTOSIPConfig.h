/* FreeRTOSIPConfig.h - FreeRTOS+TCP V2.3.2 configuration for the purifier.
 *
 * Derived from boards/mw300_rd/aws_demos/config_files/FreeRTOSIPConfig.h,
 * sized down for 384+128 KiB SRAM and a handful of LAN sockets
 * (ESPHome API 6053, OTA HTTP, mDNS 5353).
 *
 * Differences from the SDK demo that matter:
 *  - ipconfigETHERNET_DRIVER_FILTERS_PACKETS = 1: our NetworkInterface
 *    (src/net_mw300/NetworkInterface.c) does the IP destination filter so it
 *    can admit joined multicast groups (224.0.0.251 for mDNS).  Stock
 *    FreeRTOS+TCP 2.3.2 drops all multicast except LLMNR.
 *  - ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM = 0: the stack verifies RX
 *    checksums itself (the Wi-Fi driver gives no offload guarantee).
 *  - no DHCP hook (the demo's hook could silently switch to a static IP).
 *  - UDP TTL 255 (RFC 6762 recommends it for mDNS).
 */
#ifndef FREERTOS_IP_CONFIG_H
#define FREERTOS_IP_CONFIG_H

#define ipconfigHAS_DEBUG_PRINTF                0
#define ipconfigHAS_PRINTF                      0

#define ipconfigBYTE_ORDER                      pdFREERTOS_LITTLE_ENDIAN
#define ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM  0
#define ipconfigDRIVER_INCLUDED_TX_IP_CHECKSUM  0
#define ipconfigETHERNET_DRIVER_FILTERS_FRAME_TYPES 1
#define ipconfigETHERNET_DRIVER_FILTERS_PACKETS 1
#define ipconfigFILTER_OUT_NON_ETHERNET_II_FRAMES 1

/* Blocking defaults; netio sets 0 per socket for non-blocking use. */
#define ipconfigSOCK_DEFAULT_RECEIVE_BLOCK_TIME (pdMS_TO_TICKS(5000))
#define ipconfigSOCK_DEFAULT_SEND_BLOCK_TIME    (pdMS_TO_TICKS(5000))

#define ipconfigUSE_DNS                         1
#define ipconfigUSE_DNS_CACHE                   1
#define ipconfigDNS_CACHE_ENTRIES               4
#define ipconfigDNS_CACHE_ADDRESSES_PER_ENTRY   2
#define ipconfigDNS_REQUEST_ATTEMPTS            4
#define ipconfigUSE_LLMNR                       0
#define ipconfigUSE_NBNS                        0

#define ipconfigIP_TASK_PRIORITY                (configMAX_PRIORITIES - 2)
#define ipconfigIP_TASK_STACK_SIZE_WORDS        (configMINIMAL_STACK_SIZE * 4)

extern uint32_t ulRand(void);
#define ipconfigRAND32()                        ulRand()

#define ipconfigUSE_NETWORK_EVENT_HOOK          1
#define ipconfigUDP_MAX_SEND_BLOCK_TIME_TICKS   (pdMS_TO_TICKS(1000))

#define ipconfigUSE_DHCP                        1
#define ipconfigDHCP_REGISTER_HOSTNAME          1
#define ipconfigDHCP_USES_UNICAST               1
#define ipconfigUSE_DHCP_HOOK                   0
#define ipconfigMAXIMUM_DISCOVER_TX_PERIOD      (pdMS_TO_TICKS(30000))

#define ipconfigARP_CACHE_ENTRIES               8
#define ipconfigMAX_ARP_RETRANSMISSIONS         (5)
#define ipconfigMAX_ARP_AGE                     150
#define ipconfigINCLUDE_FULL_INET_ADDR          1

/* BufferAllocation_2: buffers come from the heap on demand; the descriptor
 * count caps concurrent frames in flight. */
#define ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS  24U
#define ipconfigEVENT_QUEUE_LENGTH              (ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS + 5)
#define ipconfigALLOW_SOCKET_SEND_WITHOUT_BIND  1
#define ipconfigUDP_TIME_TO_LIVE                255
#define ipconfigTCP_TIME_TO_LIVE                128

#define ipconfigUSE_TCP                         1
#define ipconfigUSE_TCP_WIN                     1
#define ipconfigNETWORK_MTU                     1500U
#define ipconfigTCP_MSS                         1460U
#define ipconfigTCP_WIN_SEG_COUNT               32
/* Per-socket stream buffers (allocated on first use).  The OTA receiver may
 * enlarge its own with FREERTOS_SO_RCVBUF / FREERTOS_SO_WIN_PROPERTIES. */
#define ipconfigTCP_RX_BUFFER_LENGTH            (3U * 1460U)
#define ipconfigTCP_TX_BUFFER_LENGTH            (3U * 1460U)
#define ipconfigTCP_KEEP_ALIVE                  1
#define ipconfigTCP_KEEP_ALIVE_INTERVAL         20 /* s */
#define ipconfigTCP_HANG_PROTECTION             1
#define ipconfigTCP_HANG_PROTECTION_TIME        30 /* s */

#define ipconfigREPLY_TO_INCOMING_PINGS         1
#define ipconfigSUPPORT_OUTGOING_PINGS          0
#define ipconfigSUPPORT_SELECT_FUNCTION         0
#define ipconfigSOCKET_HAS_USER_SEMAPHORE       0
#define ipconfigSOCKET_HAS_USER_WAKE_CALLBACK   0
#define ipconfigUSE_CALLBACKS                   0
#define ipconfigPACKET_FILLER_SIZE              2U
#define ipconfigIS_VALID_PROG_ADDRESS(x)        ((x) != NULL)
#define ipconfigZERO_COPY_RX_DRIVER             0
#define ipconfigZERO_COPY_TX_DRIVER             0

/* Unused static-address fallback (only if DHCP is disabled). */
#define configIP_ADDR0  0
#define configIP_ADDR1  0
#define configIP_ADDR2  0
#define configIP_ADDR3  0

#define portINLINE      __inline

#endif /* FREERTOS_IP_CONFIG_H */
