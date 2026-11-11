/* netio_fake.c - scripted in-memory netio for the HTTP tests. */
#include <string.h>
#include "netio.h"
#include "sim.h"


fake_conn fake_conns[FAKE_MAX_CONN];
static int nconns;
static int listening;

void fake_net_reset(void)
{
    memset(fake_conns, 0, sizeof fake_conns);
    nconns = 0;
    listening = 0;
}

fake_conn *fake_connect(const uint8_t *req, size_t len, size_t chunk)
{
    if (nconns >= FAKE_MAX_CONN)
        return NULL;
    fake_conn *c = &fake_conns[nconns++];
    memset(c, 0, sizeof *c);
    c->in = req;
    c->in_len = len;
    c->stop_at = len;
    c->chunk = chunk ? chunk : 1460;
    c->close_at_end = 1;
    return c;
}

static fake_conn *conn_of(netio_sock s)
{
    int i = s - 200;
    return (i >= 0 && i < nconns) ? &fake_conns[i] : NULL;
}

netio_sock netio_tcp_listen(uint16_t port)
{
    (void)port;
    listening = 1;
    return 100;
}

netio_sock netio_tcp_accept(netio_sock l)
{
    if (l != 100 || !listening)
        return -1;
    for (int i = 0; i < nconns; i++)
        if (!fake_conns[i].accepted) {
            fake_conns[i].accepted = 1;
            return 200 + i;
        }
    return NETIO_AGAIN;
}

int netio_recv(netio_sock s, void *buf, size_t len)
{
    fake_conn *c = conn_of(s);
    if (!c || c->closed)
        return -1;
    if (c->in_pos >= c->stop_at) {
        if (c->close_at_end && c->in_pos >= c->in_len)
            return 0;
        if (c->close_at_end && c->stop_at < c->in_len)
            return 0;               /* client hung up early (truncation) */
        return NETIO_AGAIN;
    }
    size_t n = c->stop_at - c->in_pos;
    if (n > c->chunk) n = c->chunk;
    if (n > len) n = len;
    memcpy(buf, c->in + c->in_pos, n);
    c->in_pos += n;
    return (int)n;
}

int netio_send(netio_sock s, const void *buf, size_t len)
{
    fake_conn *c = conn_of(s);
    if (!c || c->closed)
        return -1;
    if (c->max_send && len > (size_t)c->max_send)
        len = (size_t)c->max_send;
    if (len > sizeof c->out - c->out_len)
        len = sizeof c->out - c->out_len;
    memcpy(c->out + c->out_len, buf, len);
    c->out_len += len;
    return (int)len;
}

void netio_close(netio_sock s)
{
    fake_conn *c = conn_of(s);
    if (c)
        c->closed = 1;
    if (s == 100)
        listening = 0;
}

netio_sock netio_udp_bind(uint16_t port, uint32_t g) { (void)port; (void)g; return -1; }
int netio_udp_recvfrom(netio_sock s, void *b, size_t l, uint32_t *ip, uint16_t *p)
{ (void)s; (void)b; (void)l; (void)ip; (void)p; return -1; }
int netio_udp_sendto(netio_sock s, const void *b, size_t l, uint32_t ip, uint16_t p)
{ (void)s; (void)b; (void)l; (void)ip; (void)p; return -1; }
uint32_t netio_local_ip_be(void) { return 0x0100007Fu; }
int netio_rssi(void) { return 0; }
void netio_mac(uint8_t mac[6]) { memset(mac, 0, 6); }
