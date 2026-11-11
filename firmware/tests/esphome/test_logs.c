/* test_logs.c - api_server.c log streaming (R3) against a scripted
 * in-memory netio, plaintext framing: history replay on SubscribeLogs,
 * live lines, per-client level, partial-line flush, a slow client that is
 * lapped by the ring ("bytes dropped"), and requests still answered while
 * logs are pending. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esphome_api.h"
#include "hal.h"
#include "netio.h"
#include "purifier.h"
#include "api_internal.h"
#include "logring.h"

static uint32_t now_ms = 1000;
uint32_t hal_millis(void) { return now_ms; }

int api_platform_entropy(void *buf, size_t len)
{
	memset(buf, 0x5a, len);
	return 0;
}

/* ---------------- mock netio ---------------- */
#define LISTENER 100
#define MAXC 3
typedef struct {
	int open, pending;
	uint8_t in[4096];
	size_t in_len, in_pos;
	uint8_t out[1 << 18];
	size_t out_len, out_pos;   /* out_pos: parsed so far */
	size_t send_cap;
} mconn;
static mconn mc[MAXC];

netio_sock netio_tcp_listen(uint16_t port) { (void)port; return LISTENER; }
netio_sock netio_tcp_accept(netio_sock l)
{
	(void)l;
	for (int i = 0; i < MAXC; i++)
		if (mc[i].pending) {
			mc[i].pending = 0;
			mc[i].open = 1;
			return 200 + i;
		}
	return NETIO_AGAIN;
}
static mconn *M(netio_sock s)
{
	assert(s >= 200 && s < 200 + MAXC);
	return &mc[s - 200];
}
int netio_recv(netio_sock s, void *buf, size_t len)
{
	mconn *c = M(s);
	size_t n = c->in_len - c->in_pos;
	if (!n)
		return NETIO_AGAIN;
	if (n > len)
		n = len;
	memcpy(buf, c->in + c->in_pos, n);
	c->in_pos += n;
	return (int)n;
}
int netio_send(netio_sock s, const void *buf, size_t len)
{
	mconn *c = M(s);
	size_t n = len < c->send_cap ? len : c->send_cap;
	if (!n)
		return NETIO_AGAIN;
	assert(c->out_len + n <= sizeof(c->out));
	memcpy(c->out + c->out_len, buf, n);
	c->out_len += n;
	return (int)n;
}
void netio_close(netio_sock s)
{
	if (s != LISTENER)
		M(s)->open = 0;
}
netio_sock netio_udp_bind(uint16_t p, uint32_t g) { (void)p; (void)g; return -1; }
int netio_udp_recvfrom(netio_sock s, void *b, size_t l, uint32_t *ip, uint16_t *p)
{
	(void)s; (void)b; (void)l; (void)ip; (void)p;
	return -1;
}
int netio_udp_sendto(netio_sock s, const void *b, size_t l, uint32_t ip, uint16_t p)
{
	(void)s; (void)b; (void)l; (void)ip; (void)p;
	return -1;
}
uint32_t netio_local_ip_be(void) { return 0x0100007f; }
int netio_rssi(void) { return -50; }
void netio_mac(uint8_t m[6]) { memset(m, 0x22, 6); }

/* ---------------- helpers ---------------- */
static void push(mconn *c, unsigned type, const uint8_t *p, size_t len)
{
	uint8_t *o = c->in + c->in_len;
	size_t n = 0;
	o[n++] = 0;
	o[n++] = (uint8_t)len;          /* < 128 */
	o[n++] = (uint8_t)type;         /* < 128 */
	memcpy(o + n, p, len);
	c->in_len += n + len;
}

static const uint8_t HELLO[] = { 0x0a, 0x04, 't', 'e', 's', 't', 0x10, 0x01, 0x18, 0x13 };

static void subscribe_logs(mconn *c, unsigned level, int dump)
{
	uint8_t p[4] = { 0x08, (uint8_t)level, 0x10, (uint8_t)dump };
	push(c, MSG_SUBSCRIBE_LOGS_REQ, p, sizeof(p));
}

static uint64_t varint(const uint8_t **p)
{
	uint64_t v = 0;
	unsigned sh = 0;
	do {
		v |= (uint64_t)(**p & 0x7f) << sh;
		sh += 7;
	} while (*(*p)++ & 0x80);
	return v;
}

/* Next frame from the client's output: type, or -1 at the end.  For a
 * SubscribeLogsResponse, *level and msg (NUL-terminated) are decoded. */
static int next_msg(mconn *c, unsigned *level, char *msg, size_t cap)
{
	const uint8_t *p = c->out + c->out_pos, *end, *q;
	uint64_t len, type;
	if (c->out_pos >= c->out_len)
		return -1;
	assert(*p == 0);
	p++;
	len = varint(&p);
	type = varint(&p);
	end = p + len;
	assert(end <= c->out + c->out_len);
	msg[0] = 0;
	*level = 0;
	if (type == MSG_SUBSCRIBE_LOGS_RESP) {
		for (q = p; q < end;) {
			uint64_t key = varint(&q);
			if (key == 0x08) {
				*level = (unsigned)varint(&q);
			} else if (key == 0x1a) {
				size_t n = (size_t)varint(&q);
				assert(n < cap);
				memcpy(msg, q, n);
				msg[n] = 0;
				q += n;
			} else {
				assert(0);
			}
		}
	}
	c->out_pos = (size_t)(end - c->out);
	return (int)type;
}

static void poll_n(int n)
{
	for (int i = 0; i < n; i++)
		api_server_poll();
}

static mconn *connect_client(int i)
{
	memset(&mc[i], 0, sizeof(mc[i]));
	mc[i].pending = 1;
	mc[i].send_cap = 1u << 20;
	push(&mc[i], MSG_HELLO_REQ, HELLO, sizeof(HELLO));
	poll_n(2);
	assert(mc[i].open);
	return &mc[i];
}

/* Collect all log messages currently in the output (others skipped). */
#define MAXLOG 2048
static char logs[MAXLOG][200];
static unsigned levels[MAXLOG];
static int collect(mconn *c, int *other_types, int nother)
{
	int n = 0, t, k = 0;
	unsigned lv;
	char m[200];
	while ((t = next_msg(c, &lv, m, sizeof(m))) >= 0) {
		if (t != MSG_SUBSCRIBE_LOGS_RESP) {
			if (other_types && k < nother)
				other_types[k++] = t;
			continue;
		}
		assert(n < MAXLOG);
		strcpy(logs[n], m);
		levels[n++] = lv;
	}
	return n;
}

static int find(int n, const char *s)
{
	for (int i = 0; i < n; i++)
		if (strstr(logs[i], s))
			return i;
	return -1;
}

/* ---------------- tests ---------------- */

static void test_replay_live_levels(void)
{
	mconn *a, *b;
	int n, i0, i1, i2;

	logring_puts("\r\n=== boot ===\r\n[boot] main reached\r\n");
	logring_puts("[net] wlan_connect failed -5 (will retry)\r\n");
	logring_puts("\r\n*** malloc failed (free 12) at 3s\r\n");
	logring_puts("[app] servers started\r\n");

	a = connect_client(0);
	subscribe_logs(a, API_LOG_DEBUG, 0);
	poll_n(3);
	n = collect(a, NULL, 0);
	i0 = find(n, "[I] === boot ===");
	i1 = find(n, "[I][boot] main reached");
	i2 = find(n, "[W][net] wlan_connect failed -5 (will retry)");
	assert(i0 >= 0 && i1 == i0 + 1 && i2 == i1 + 1);
	assert(levels[i0] == API_LOG_INFO && levels[i2] == API_LOG_WARN);
	assert(find(n, "[E] *** malloc failed") == i2 + 1 && levels[i2 + 1] == API_LOG_ERROR);
	assert(find(n, "[I][app] servers started") == i2 + 2);
	printf("  ok  subscribe -> boot history replayed in order with levels (%d lines)\n", n);

	logring_puts("[app] live line\r\n");
	api_log(API_LOG_WARN, "api warning");
	poll_n(2);
	n = collect(a, NULL, 0);
	assert(n == 2 && !strcmp(logs[0], "[I][app] live line") &&
	       !strcmp(logs[1], "[W][api]: api warning") && levels[1] == API_LOG_WARN);
	printf("  ok  live console lines and api_log() lines streamed\n");

	/* a second client at WARN: replay filtered by its level */
	b = connect_client(1);
	subscribe_logs(b, API_LOG_WARN, 0);
	poll_n(3);
	n = collect(b, NULL, 0);
	for (int i = 0; i < n; i++)
		assert(levels[i] <= API_LOG_WARN);
	assert(find(n, "wlan_connect failed") >= 0 && find(n, "malloc failed") >= 0 &&
	       find(n, "api warning") >= 0 && find(n, "main reached") < 0);
	printf("  ok  per-client level: WARN client gets only warnings/errors (%d lines)\n", n);

	/* re-subscribing changes the level, no second replay */
	subscribe_logs(a, API_LOG_DEBUG, 0);
	poll_n(2);
	assert(collect(a, NULL, 0) == 0);
	/* dump_config lines come immediately */
	subscribe_logs(a, API_LOG_DEBUG, 1);
	poll_n(2);
	n = collect(a, NULL, 0);
	assert(n == 3 && levels[0] == API_LOG_CONFIG);
	printf("  ok  re-subscribe: no duplicate replay; dump_config still answered\n");

	/* unterminated tail: only after LOG_PARTIAL_MS of quiet */
	logring_puts("[net] connecting");
	poll_n(2);
	assert(collect(a, NULL, 0) == 0);
	now_ms += 600;
	poll_n(2);
	n = collect(a, NULL, 0);
	assert(n == 1 && !strcmp(logs[0], "[I][net] connecting"));
	collect(b, NULL, 0);
	printf("  ok  unterminated line flushed after 500 ms idle\n");

	a->open = 0;
	b->open = 0;
	a->in_len = a->in_pos = b->in_len = b->in_pos = 0;
	/* peers gone: the server notices on its next send/recv */
	now_ms += 200000;
	poll_n(3);
}

static void test_slow_client(void)
{
	mconn *c = connect_client(0);
	char b[80];
	int n, types[64], drop, last;

	subscribe_logs(c, API_LOG_DEBUG, 0);
	poll_n(2);
	collect(c, NULL, 0);

	/* peer stops reading; 12 KB of logs arrive (3x the ring) */
	c->send_cap = 0;
	for (int i = 0; i < 200; i++) {
		snprintf(b, sizeof(b), "[test] flood %04d ..........................................\r\n", i);
		logring_puts(b);
		poll_n(1);              /* never blocks */
	}
	/* a request while logs are pending is still parsed: the log stream
	 * leaves the request reserve free */
	push(c, MSG_PING_REQ, NULL, 0);
	poll_n(3);
	assert(c->in_pos == c->in_len);
	c->send_cap = 1u << 20;
	poll_n(400);
	memset(types, 0, sizeof(types));
	n = collect(c, types, 64);
	{
		int pong = 0;
		for (int i = 0; i < 64; i++)
			pong |= types[i] == MSG_PING_RESP;
		assert(pong);
	}
	drop = find(n, "[W][log]: ");
	assert(drop >= 0 && strstr(logs[drop], " bytes dropped") && levels[drop] == API_LOG_WARN);
	last = find(n, "flood 0199");
	assert(last > drop && last == n - 1);
	for (int i = drop + 2; i <= last; i++) {
		int a, b2;
		assert(sscanf(logs[i - 1], "[I][test] flood %d", &a) == 1 &&
		       sscanf(logs[i], "[I][test] flood %d", &b2) == 1 && b2 == a + 1);
	}
	printf("  ok  lapped slow client: '%s', then contiguous lines to the newest; ping answered\n",
	       logs[drop] + 10);
	c->open = 0;
	now_ms += 200000;
	poll_n(3);
}

int main(void)
{
	api_config cfg = { "unit", "Unit", "", "0", "22:22:22:22:22:22", "" };
	purifier_init();
	purifier_take_changes();
	assert(api_server_init(&cfg) == 0);
	printf("test_logs:\n");
	test_replay_live_levels();
	test_slow_client();
	printf("test_logs: OK\n");
	return 0;
}
