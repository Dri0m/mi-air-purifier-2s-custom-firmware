/* netio_posix.c - netio.h over non-blocking POSIX sockets, loopback only.
 * Test-only (end-to-end test of ota_server + tools/ota_upload.py). */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "netio.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

netio_sock netio_tcp_listen(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    nonblock(fd);
    return fd;
}

netio_sock netio_tcp_accept(netio_sock l)
{
    int fd = accept(l, NULL, NULL);
    if (fd < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? NETIO_AGAIN : -1;
    nonblock(fd);
    return fd;
}

int netio_recv(netio_sock s, void *buf, size_t len)
{
    ssize_t r = recv(s, buf, len, 0);
    if (r < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? NETIO_AGAIN : -1;
    return (int)r;
}

int netio_send(netio_sock s, const void *buf, size_t len)
{
    ssize_t r = send(s, buf, len, MSG_NOSIGNAL);
    if (r < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? NETIO_AGAIN : -1;
    return (int)r;
}

void netio_close(netio_sock s)
{
    if (s >= 0)
        close(s);
}

netio_sock netio_udp_bind(uint16_t port, uint32_t g) { (void)port; (void)g; return -1; }
int netio_udp_recvfrom(netio_sock s, void *b, size_t l, uint32_t *ip, uint16_t *p)
{ (void)s; (void)b; (void)l; (void)ip; (void)p; return -1; }
int netio_udp_sendto(netio_sock s, const void *b, size_t l, uint32_t ip, uint16_t p)
{ (void)s; (void)b; (void)l; (void)ip; (void)p; return -1; }
uint32_t netio_local_ip_be(void) { return htonl(INADDR_LOOPBACK); }
int netio_rssi(void) { return 0; }
void netio_mac(uint8_t mac[6]) { memset(mac, 0, 6); }
