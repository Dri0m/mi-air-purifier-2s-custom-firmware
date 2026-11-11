/* netio.h - tiny socket layer for the portable network code.
 * Device: src/net_mw300 (FreeRTOS+TCP).  Host: src/host (POSIX).
 * All calls are non-blocking.  Return <0 on error, NETIO_AGAIN if no data
 * / would block, 0 on orderly close for recv. */
#ifndef NETIO_H
#define NETIO_H
#include <stddef.h>
#include <stdint.h>

#define NETIO_AGAIN (-2)
typedef int netio_sock;                    /* <0 = invalid */

netio_sock netio_tcp_listen(uint16_t port);
netio_sock netio_tcp_accept(netio_sock listener);
int netio_recv(netio_sock s, void *buf, size_t len);
int netio_send(netio_sock s, const void *buf, size_t len);
void netio_close(netio_sock s);

/* UDP, for mDNS (224.0.0.251:5353). */
netio_sock netio_udp_bind(uint16_t port, uint32_t mcast_group_be);
int netio_udp_recvfrom(netio_sock s, void *buf, size_t len,
                       uint32_t *src_ip_be, uint16_t *src_port);
int netio_udp_sendto(netio_sock s, const void *buf, size_t len,
                     uint32_t dst_ip_be, uint16_t dst_port);

/* Network status. */
uint32_t netio_local_ip_be(void);          /* 0 if not connected */
int netio_rssi(void);                      /* dBm, 0 if unknown */
void netio_mac(uint8_t mac[6]);
#endif
