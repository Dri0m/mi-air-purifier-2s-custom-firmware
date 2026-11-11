/* netio_posix.c - netio.h over POSIX sockets, for host builds and tests.
 *
 * Environment overrides (all optional):
 *   NETIO_IP    dotted IPv4 to report from netio_local_ip_be() (default:
 *               first non-loopback IPv4 interface that is up)
 *   NETIO_RSSI  value for netio_rssi() (default -55)
 *   NETIO_MAC   "AA:BB:CC:DD:EE:FF" (default 02:00:00:2A:02:53, locally administered) */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "netio.h"

static int set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void no_sigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
	(void)fd;
#endif
}

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

netio_sock netio_tcp_listen(uint16_t port)
{
	struct sockaddr_in a;
	int one = 1;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0 ||
	    set_nonblock(fd) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

netio_sock netio_tcp_accept(netio_sock listener)
{
	int one = 1;
	int fd = accept(listener, NULL, NULL);
	if (fd < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? NETIO_AGAIN : -1;
	set_nonblock(fd);
	no_sigpipe(fd);
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}

int netio_recv(netio_sock s, void *buf, size_t len)
{
	ssize_t n = recv(s, buf, len, 0);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ?
		       NETIO_AGAIN : -1;
	return (int)n;
}

int netio_send(netio_sock s, const void *buf, size_t len)
{
	ssize_t n = send(s, buf, len, SEND_FLAGS);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ?
		       NETIO_AGAIN : -1;
	return (int)n;
}

void netio_close(netio_sock s)
{
	if (s >= 0)
		close(s);
}

netio_sock netio_udp_bind(uint16_t port, uint32_t mcast_group_be)
{
	struct sockaddr_in a;
	int one = 1;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	/* share 5353 with the OS responder (mDNSResponder / avahi) */
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
		goto fail;
	if (mcast_group_be) {
		struct ip_mreq m;
		uint32_t local = netio_local_ip_be();
		unsigned char ttl = 255, loop = 1;
		m.imr_multiaddr.s_addr = mcast_group_be;
		m.imr_interface.s_addr = local ? local : htonl(INADDR_ANY);
		if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) < 0)
			goto fail;
		if (local) {
			struct in_addr ifa;
			ifa.s_addr = local;
			setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
		}
		setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
		setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
	}
	if (set_nonblock(fd) < 0)
		goto fail;
	return fd;
fail:
	close(fd);
	return -1;
}

int netio_udp_recvfrom(netio_sock s, void *buf, size_t len, uint32_t *src_ip_be,
		       uint16_t *src_port)
{
	struct sockaddr_in a;
	socklen_t al = sizeof(a);
	ssize_t n = recvfrom(s, buf, len, 0, (struct sockaddr *)&a, &al);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ?
		       NETIO_AGAIN : -1;
	if (src_ip_be)
		*src_ip_be = a.sin_addr.s_addr;
	if (src_port)
		*src_port = ntohs(a.sin_port);
	return (int)n;
}

int netio_udp_sendto(netio_sock s, const void *buf, size_t len, uint32_t dst_ip_be,
		     uint16_t dst_port)
{
	struct sockaddr_in a;
	ssize_t n;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(dst_port);
	a.sin_addr.s_addr = dst_ip_be;
	n = sendto(s, buf, len, 0, (struct sockaddr *)&a, sizeof(a));
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? NETIO_AGAIN : -1;
	return (int)n;
}

uint32_t netio_local_ip_be(void)
{
	const char *env = getenv("NETIO_IP");
	struct ifaddrs *ifs, *i;
	uint32_t ip = 0;
	if (env) {
		struct in_addr a;
		return inet_pton(AF_INET, env, &a) == 1 ? a.s_addr : 0;
	}
	if (getifaddrs(&ifs) < 0)
		return 0;
	for (i = ifs; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
			continue;
		if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) ||
		    !(i->ifa_flags & IFF_MULTICAST))
			continue;
		ip = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
		break;
	}
	freeifaddrs(ifs);
	return ip;
}

int netio_rssi(void)
{
	const char *env = getenv("NETIO_RSSI");
	return env ? atoi(env) : -55;
}

void netio_mac(uint8_t mac[6])
{
	static const uint8_t def[6] = { 0x02, 0x00, 0x00, 0x2a, 0x02, 0x53 };
	const char *env = getenv("NETIO_MAC");
	unsigned v[6];
	if (env && sscanf(env, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4],
			  &v[5]) == 6) {
		for (int i = 0; i < 6; i++)
			mac[i] = (uint8_t)v[i];
		return;
	}
	memcpy(mac, def, 6);
}
