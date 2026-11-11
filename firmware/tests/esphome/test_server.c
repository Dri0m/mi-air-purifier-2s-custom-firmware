/* test_server.c - deterministic tests of api_server.c framing and
 * backpressure, with a scripted in-memory netio (no sockets, no timing).
 *
 * The mock controls exactly how many bytes each netio_recv() delivers and
 * how many each netio_send() accepts, so every interleaving is reproducible.
 * Property checked throughout: every poll processes every complete buffered
 * frame whose reply fits, without needing new bytes to arrive. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esphome_api.h"
#include "hal.h"
#include "netio.h"
#include "purifier.h"
#include "api_internal.h"
#include "noise.h"
#include "diag.h"
#include "diag_stub.h"
#include "crypto/monocypher/monocypher.h"

/* ---------------- mock clock ---------------- */
static uint32_t now_ms = 1000;
uint32_t hal_millis(void) { return now_ms; }

/* ---------------- mock netio ---------------- */
#define LISTENER 100
#define MAXC 4
typedef struct {
	int open;          /* server side still open */
	int pending;       /* waiting in the accept queue */
	uint8_t in[16384];
	size_t in_len, in_pos;
	size_t recv_max;   /* bytes per netio_recv() call (0 = all) */
	uint8_t out[65536];
	size_t out_len;
	size_t send_cap;   /* bytes accepted per netio_send() call */
	int peer_closed;
	struct nini *nz;    /* Noise initiator, NULL = plaintext client */
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
	assert(c->open);
	if (!n)
		return c->peer_closed ? 0 : NETIO_AGAIN;
	if (c->recv_max && n > c->recv_max)
		n = c->recv_max;
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
	assert(c->open);
	if (!n)
		return NETIO_AGAIN;
	assert(c->out_len + n <= sizeof(c->out));
	memcpy(c->out + c->out_len, buf, n);
	c->out_len += n;
	return (int)n;
}
void netio_close(netio_sock s)
{
	if (s == LISTENER)
		return;
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
static uint32_t mock_ip;          /* network order */
uint32_t netio_local_ip_be(void) { return mock_ip; }
int netio_rssi(void) { return -50; }
void netio_mac(uint8_t m[6]) { memset(m, 0x22, 6); }

/* ---------------- helpers ---------------- */
static size_t frame(uint8_t *o, unsigned type, const uint8_t *p, size_t len)
{
	size_t n = 0;
	o[n++] = 0;
	for (size_t v = len; ; v >>= 7) {
		o[n++] = (uint8_t)((v & 0x7f) | (v >= 0x80 ? 0x80 : 0));
		if (v < 0x80)
			break;
	}
	for (unsigned v = type; ; v >>= 7) {
		o[n++] = (uint8_t)((v & 0x7f) | (v >= 0x80 ? 0x80 : 0));
		if (v < 0x80)
			break;
	}
	if (len)
		memcpy(o + n, p, len);
	return n + len;
}

/* ---------------- Noise initiator (written from the Noise spec on top
 * of the noise.h primitives + Monocypher; independent of noise_respond) */
struct nini {
	uint8_t ck[32], h[32], k[32], e[32], epub[32];
	noise_cipher send, recv, recv0;
	size_t data_off;            /* where encrypted data starts in out[] */
};
static uint8_t PSK[32];

static void hkdf(const uint8_t ck[32], const void *ikm, size_t n, uint8_t o1[32],
		 uint8_t o2[32], uint8_t *o3)
{
	uint8_t t[32], one = 1, two = 2, three = 3, a[32];
	noise_hmac_sha256(t, ck, 32, ikm, n, NULL, 0);
	noise_hmac_sha256(a, t, 32, &one, 1, NULL, 0);
	noise_hmac_sha256(o2, t, 32, a, 32, &two, 1);
	if (o3)
		noise_hmac_sha256(o3, t, 32, o2, 32, &three, 1);
	memcpy(o1, a, 32);
}

static void mixhash(struct nini *s, const void *d, size_t n)
{
	noise_sha256(s->h, s->h, 32, d, n);
}

/* -> msg1 (48 bytes) */
static void ini_start(struct nini *s, const uint8_t psk[32], uint8_t seed, uint8_t msg1[48])
{
	static const uint8_t zero12[12];
	uint8_t tmp[32];
	memset(s, 0, sizeof(*s));
	noise_sha256(s->h, NOISE_PROTOCOL_NAME, 36, NULL, 0);
	memcpy(s->ck, s->h, 32);
	mixhash(s, "NoiseAPIInit\0\0", 14);
	hkdf(s->ck, psk, 32, s->ck, tmp, s->k);          /* psk */
	mixhash(s, tmp, 32);
	memset(s->e, seed, 32);
	crypto_x25519_public_key(s->epub, s->e);         /* e */
	mixhash(s, s->epub, 32);
	hkdf(s->ck, s->epub, 32, s->ck, s->k, NULL);
	memcpy(msg1, s->epub, 32);
	noise_aead_encrypt(s->k, zero12, s->h, 32, NULL, 0, msg1 + 32);
	mixhash(s, msg1 + 32, 16);
}

static int ini_finish(struct nini *s, const uint8_t msg2[48])
{
	static const uint8_t zero12[12];
	uint8_t dh[32];
	mixhash(s, msg2, 32);                            /* re */
	hkdf(s->ck, msg2, 32, s->ck, s->k, NULL);
	crypto_x25519(dh, s->e, msg2);                   /* ee */
	hkdf(s->ck, dh, 32, s->ck, s->k, NULL);
	if (noise_aead_decrypt(s->k, zero12, s->h, 32, NULL, 0, msg2 + 32))
		return -1;
	mixhash(s, msg2 + 32, 16);
	hkdf(s->ck, NULL, 0, s->send.k, s->recv.k, NULL); /* split */
	s->send.n = s->recv.n = 0;
	s->recv0 = s->recv;
	return 0;
}

static void push_raw(mconn *c, const void *p, size_t n)
{
	memcpy(c->in + c->in_len, p, n);
	c->in_len += n;
}

static void push(mconn *c, unsigned type, const uint8_t *p, size_t len)
{
	if (!c->nz) {
		c->in_len += frame(c->in + c->in_len, type, p, len);
		return;
	}
	uint8_t *o = c->in + c->in_len;
	o[3] = (uint8_t)(type >> 8);
	o[4] = (uint8_t)type;
	o[5] = (uint8_t)(len >> 8);
	o[6] = (uint8_t)len;
	if (len)
		memcpy(o + 7, p, len);
	size_t n = noise_encrypt(&c->nz->send, o + 3, 4 + len);
	o[0] = 1;
	o[1] = (uint8_t)(n >> 8);
	o[2] = (uint8_t)n;
	c->in_len += 3 + n;
}

static const uint8_t HELLO[] = { 0x0a, 0x04, 't', 'e', 's', 't', 0x10, 0x01, 0x18, 0x13 };

/* Parse the output stream into message types; asserts it is well framed. */
static int out_types(const mconn *c, unsigned *types, int max)
{
	size_t p = 0;
	int n = 0;
	if (c->nz) {                /* decrypt every data frame */
		static uint8_t b[65536];
		noise_cipher r = c->nz->recv0;
		p = c->nz->data_off;
		while (p < c->out_len) {
			size_t fl;
			int m;
			assert(p + 3 <= c->out_len && c->out[p] == 1);
			fl = (size_t)c->out[p + 1] << 8 | c->out[p + 2];
			assert(p + 3 + fl <= c->out_len);
			memcpy(b, c->out + p + 3, fl);
			m = noise_decrypt(&r, b, fl);
			assert(m >= 4 && (size_t)(b[2] << 8 | b[3]) == (size_t)m - 4);
			if (n < max)
				types[n] = (unsigned)(b[0] << 8 | b[1]);
			n++;
			p += 3 + fl;
		}
		return n;
	}
	while (p < c->out_len) {
		uint64_t len = 0, type = 0;
		unsigned sh = 0;
		assert(c->out[p] == 0);
		p++;
		do { len |= (uint64_t)(c->out[p] & 0x7f) << sh; sh += 7; } while (c->out[p++] & 0x80);
		sh = 0;
		do { type |= (uint64_t)(c->out[p] & 0x7f) << sh; sh += 7; } while (c->out[p++] & 0x80);
		p += len;
		assert(p <= c->out_len);
		if (n < max)
			types[n] = (unsigned)type;
		n++;
	}
	return n;
}

static int count(const unsigned *t, int n, unsigned type)
{
	int k = 0;
	for (int i = 0; i < n; i++)
		k += t[i] == type;
	return k;
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
	return &mc[i];
}

static void disconnect_client(mconn *c)
{
	c->peer_closed = 1;
	poll_n(2);
	assert(!c->open);
}

static unsigned T[4096];

/* 1: hello byte by byte, the ping's 3 bytes arriving in the SAME recv as
 * the hello's last byte; both replies must come out without new data. */
static void test_coalesced_tail(void)
{
	mconn *c = connect_client(0);
	size_t hello_len;
	push(c, 1, HELLO, sizeof(HELLO));
	hello_len = c->in_len;
	push(c, 7, NULL, 0);
	c->recv_max = 1;
	poll_n(1);                              /* accept */
	while (c->in_pos < hello_len - 1)
		poll_n(1);
	c->recv_max = 0;                        /* last hello byte + whole ping */
	poll_n(1);
	assert(c->in_pos == c->in_len);
	int n = out_types(c, T, 64);
	assert(n == 2 && T[0] == 2 && T[1] == 8);
	disconnect_client(c);
	printf("  ok  hello tail + ping in one recv -> HelloResponse + PingResponse, same poll\n");
}

/* 2: many requests buffered while the peer reads nothing (TX full), then
 * NO further input: every deferred request is answered as TX drains. */
static void test_deferred_without_new_data(void)
{
	mconn *c = connect_client(0);
	int pings = 400, n;   /* 1.2 KB: more than RX (256) + replies TX can hold */
	push(c, 1, HELLO, sizeof(HELLO));
	push(c, 11, NULL, 0);                   /* ListEntities */
	push(c, 20, NULL, 0);                   /* SubscribeStates */
	for (int i = 0; i < pings; i++)
		push(c, 7, NULL, 0);
	push(c, 9, NULL, 0);                    /* DeviceInfo last */
	c->send_cap = 0;                        /* peer not reading */
	poll_n(5);
	assert(c->in_pos < c->in_len);          /* backpressure: input waits */
	c->send_cap = 7;                        /* trickle out, 7 B per send */
	for (int i = 0; i < 5000; i++) {
		size_t before = c->out_len;
		api_server_poll();
		if (c->in_pos == c->in_len && c->out_len == before)
			break;              /* quiescent */
	}
	assert(c->in_pos == c->in_len);
	n = out_types(c, T, 4096);
	assert(count(T, n, 2) == 1);
	assert(count(T, n, 8) == pings);
	assert(count(T, n, 19) == 1);           /* ListEntitiesDone */
	assert(count(T, n, 10) == 1);           /* DeviceInfoResponse */
	assert(count(T, n, 25) == 11 && count(T, n, 26) == 3 && count(T, n, 23) == 1 &&
	       count(T, n, 21) == 3 && count(T, n, 50) == 2 &&
	       count(T, n, 53) == 1 && count(T, n, 27) == 4);   /* all 25 initial states */
	assert(count(T, n, 18) == 4 && count(T, n, 16) == 11 && count(T, n, 61) == 2);
	disconnect_client(c);
	printf("  ok  %d pipelined requests with TX blocked, then no new input -> all answered\n",
	       pings + 4);
}

/* 3: oversized frame dribbled in, followed by a ping. */
static void test_oversized_split(void)
{
	static uint8_t big[3000];
	mconn *c = connect_client(0);
	int n;
	memset(big, 'x', sizeof(big));
	push(c, 1, HELLO, sizeof(HELLO));
	push(c, 40, big, sizeof(big));
	push(c, 7, NULL, 0);
	c->recv_max = 97;
	poll_n(200);
	assert(c->in_pos == c->in_len && c->open);
	n = out_types(c, T, 64);
	assert(n == 2 && T[0] == 2 && T[1] == 8);
	disconnect_client(c);
	printf("  ok  3 KB frame in 97 B pieces skipped, following ping answered\n");
}

/* 4: seeded random chunking of input and output, two clients at once;
 * the reply sequence must be identical to a single-shot run. */
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static int build_script(mconn *c)
{
	push(c, 1, HELLO, sizeof(HELLO));
	push(c, 9, NULL, 0);
	push(c, 11, NULL, 0);
	push(c, 20, NULL, 0);
	for (int i = 0; i < 40; i++) {
		push(c, 7, NULL, 0);
		if (i % 7 == 0)
			push(c, 9999, (const uint8_t *)"\x08\x01", 2);   /* unknown */
	}
	return 0;
}

/* Message class: 0 = reply to a request, 1 = entity list, 2 = state.
 * List and state messages are produced as TX space allows, so they may
 * legitimately interleave with request replies; within a class the order
 * must be exactly the reference order. */
static int msg_class(unsigned t)
{
	switch (t) {
	case 12: case 14: case 16: case 17: case 18: case 19: case 49: case 52: case 61: return 1;
	case 21: case 23: case 25: case 26: case 27: case 50: case 53: return 2;
	default: return 0;
	}
}

static void check_same(const mconn *c, const unsigned *ref, int nref, int round)
{
	int n = out_types(c, T, 4096);
	assert(n == nref);
	for (int cls = 0; cls < 3; cls++) {
		int i = 0, j = 0;
		for (;;) {
			while (i < n && msg_class(T[i]) != cls)
				i++;
			while (j < nref && msg_class(ref[j]) != cls)
				j++;
			if (i == n || j == nref)
				break;
			if (T[i] != ref[j]) {
				fprintf(stderr, "round %d client %d: class %d differs\n", round,
					(int)(c - mc), cls);
				abort();
			}
			i++;
			j++;
		}
		assert(i == n && j == nref);
	}
}

static void test_random_chunking(void)
{
	static unsigned ref[4096];
	int nref;
	mconn *a = connect_client(0);
	build_script(a);
	poll_n(50);
	nref = out_types(a, ref, 4096);
	disconnect_client(a);

	for (int round = 0; round < 300; round++) {
		mconn *x = connect_client(0), *y = connect_client(1);
		build_script(x);
		build_script(y);
		for (int i = 0; i < 20000; i++) {
			x->recv_max = 1 + rnd() % 40;
			y->recv_max = 1 + rnd() % 40;
			x->send_cap = rnd() % 64;
			y->send_cap = rnd() % 64;
			now_ms += rnd() % 3;
			api_server_poll();
			if (x->in_pos == x->in_len && y->in_pos == y->in_len && i > 500)
				break;
		}
		x->send_cap = y->send_cap = 1u << 20;
		poll_n(5);
		assert(x->in_pos == x->in_len && y->in_pos == y->in_len);
		check_same(x, ref, nref, round);
		check_same(y, ref, nref, round);
		disconnect_client(x);
		disconnect_client(y);
	}
	printf("  ok  300 rounds x 2 clients, random recv/send chunking -> same replies, in order (%d msgs)\n",
	       nref);
}

/* 5: entity encoding of the fault states (B2/B4): PM2.5 missing_state
 * while the value is a placeholder, problem binary sensors, PF mapping. */
typedef struct { uint32_t key; int has_state; float state; int state_b; int missing; } st_msg;

static st_msg decode_state(unsigned idx)
{
	uint8_t buf[64];
	pb_writer w;
	pb_reader r;
	pb_field f;
	st_msg m = { 0, 0, 0, 0, 0 };
	pb_writer_init(&w, buf, sizeof(buf));
	api_entity_encode_state(idx, &w);
	assert(!w.overflow);
	pb_reader_init(&r, buf, w.len);
	while (pb_next(&r, &f) > 0) {
		if (f.field == 1)
			m.key = (uint32_t)f.u64;
		else if (f.field == 2 && f.wt == PB_WT_I32) {
			uint32_t u = (uint32_t)f.u64;
			memcpy(&m.state, &u, 4);
			m.has_state = 1;
		} else if (f.field == 2)
			m.state_b = f.u64 != 0;
		else if (f.field == 3)
			m.missing = f.u64 != 0;
	}
	assert(m.key == api_entity_key(idx));
	return m;
}

static void test_fault_entities(void)
{
	/* the stub's state lives behind purifier_get() (non-const static) */
	purifier_state *s = (purifier_state *)purifier_get();
	st_msg m;

	assert(!strcmp(api_entity_object_id(E_MOTOR_FAULT), "motor_fault"));
	assert(!strcmp(api_entity_object_id(E_PM_FAULT), "pm_sensor_fault"));
	assert(api_entity_dirty_from_pf(PF_PM_LIVE) == 1u << E_PM25);
	assert(api_entity_dirty_from_pf(PF_PM) == 1u << E_PM25);
	assert(api_entity_dirty_from_pf(PF_PM_FAULT) == 1u << E_PM_FAULT);
	assert(api_entity_dirty_from_pf(PF_MOTOR_FAULT) == 1u << E_MOTOR_FAULT);
	assert(api_entity_throttled_mask() & (1u << E_PM25));
	assert(!(api_entity_throttled_mask() & (3u << E_MOTOR_FAULT)));

	m = decode_state(E_PM25);
	assert(m.has_state && m.state == 12 && !m.missing);
	m = decode_state(E_PM_FAULT);
	assert(!m.state_b);
	m = decode_state(E_MOTOR_FAULT);
	assert(!m.state_b);

	s->pm_fault = s->pm25_unknown = true;
	s->pm25 = 23;                       /* the panel placeholder */
	m = decode_state(E_PM25);
	assert(m.missing && m.has_state && m.state != m.state);   /* NaN */
	assert(decode_state(E_PM_FAULT).state_b);
	assert(!decode_state(E_MOTOR_FAULT).state_b);
	assert(!decode_state(E_DOOR).state_b);

	s->pm_fault = s->pm25_unknown = false;
	s->pm25 = 12;
	s->motor_fault = true;
	m = decode_state(E_PM25);
	assert(m.state == 12 && !m.missing);
	assert(!decode_state(E_PM_FAULT).state_b);
	assert(decode_state(E_MOTOR_FAULT).state_b);
	s->motor_fault = false;
	printf("  ok  PM2.5 NaN + missing_state while a placeholder; motor/PM fault binary sensors; PF mapping\n");
}

/* 6: the fan level number + mode select: list fields, command -> purifier
 * call -> state echo (also over the scripted connection). */
static float f32(const pb_field *f)
{
	uint32_t u = (uint32_t)f->u64;
	float v;
	memcpy(&v, &u, 4);
	return v;
}

static size_t enc_number_cmd(uint8_t *buf, uint32_t key, float v)
{
	pb_writer w;
	pb_writer_init(&w, buf, 32);
	pb_w_fixed32_force(&w, 1, key);
	pb_w_float(&w, 2, v);
	return w.len;
}

static size_t enc_select_cmd(uint8_t *buf, uint32_t key, const char *opt)
{
	pb_writer w;
	pb_writer_init(&w, buf, 32);
	pb_w_fixed32_force(&w, 1, key);
	pb_w_string(&w, 2, opt);
	return w.len;
}

static char sel_state[16];
static int sel_missing;
static float num_state(void)
{
	st_msg m = decode_state(E_FAN_LEVEL);
	assert(m.has_state && !m.missing);
	return m.state;
}
static const char *select_state(void)
{
	uint8_t buf[64];
	pb_writer w;
	pb_reader r;
	pb_field f;
	pb_writer_init(&w, buf, sizeof(buf));
	assert(api_entity_encode_state(E_MODE, &w) == 53);
	pb_reader_init(&r, buf, w.len);
	sel_state[0] = 0;
	sel_missing = 0;
	while (pb_next(&r, &f) > 0) {
		if (f.field == 1)
			assert((uint32_t)f.u64 == api_entity_key(E_MODE));
		else if (f.field == 2)
			pb_field_str(&f, sel_state, sizeof(sel_state));
		else if (f.field == 3)
			sel_missing = f.u64 != 0;
	}
	return sel_state;
}

static void test_fan_controls(void)
{
	purifier_state *s = (purifier_state *)purifier_get();
	const uint32_t all3 = (1u << E_FAN) | (1u << E_FAN_LEVEL) | (1u << E_MODE);
	uint8_t buf[128], cmd[32];
	pb_writer w;
	pb_reader r;
	pb_field f;
	float mn = 0, mx = 0, step = 0;
	unsigned mode = 9, cat = 9, nopt = 0;
	char icon[24] = "", name[24] = "", oid[24] = "", opts[3][12];
	size_t n;

	/* ListEntitiesNumberResponse */
	pb_writer_init(&w, buf, sizeof(buf));
	assert(api_entity_encode_list(E_FAN_LEVEL, &w) == 49 && !w.overflow);
	pb_reader_init(&r, buf, w.len);
	while (pb_next(&r, &f) > 0) {
		switch (f.field) {
		case 1: pb_field_str(&f, oid, sizeof(oid)); break;
		case 2: assert((uint32_t)f.u64 == api_entity_key(E_FAN_LEVEL)); break;
		case 3: pb_field_str(&f, name, sizeof(name)); break;
		case 5: pb_field_str(&f, icon, sizeof(icon)); break;
		case 6: assert(f.wt == PB_WT_I32); mn = f32(&f); break;
		case 7: assert(f.wt == PB_WT_I32); mx = f32(&f); break;
		case 8: assert(f.wt == PB_WT_I32); step = f32(&f); break;
		case 10: cat = (unsigned)f.u64; break;
		case 12: mode = (unsigned)f.u64; break;
		default: assert(0);
		}
	}
	assert(!strcmp(oid, "fan_level") && !strcmp(name, "Fan level") &&
	       !strcmp(icon, "mdi:fan"));
	assert(mn == 1 && mx == 15 && step == 1 && mode == NUMBER_MODE_SLIDER);
	assert(cat == 9);                   /* category NONE (0) is omitted */

	/* ListEntitiesSelectResponse */
	pb_writer_init(&w, buf, sizeof(buf));
	assert(api_entity_encode_list(E_MODE, &w) == 52 && !w.overflow);
	pb_reader_init(&r, buf, w.len);
	icon[0] = name[0] = oid[0] = 0;
	while (pb_next(&r, &f) > 0) {
		switch (f.field) {
		case 1: pb_field_str(&f, oid, sizeof(oid)); break;
		case 2: assert((uint32_t)f.u64 == api_entity_key(E_MODE)); break;
		case 3: pb_field_str(&f, name, sizeof(name)); break;
		case 5: pb_field_str(&f, icon, sizeof(icon)); break;
		case 6: assert(nopt < 3); pb_field_str(&f, opts[nopt++], 12); break;
		default: assert(0);             /* category NONE omitted */
		}
	}
	assert(!strcmp(oid, "mode") && !strcmp(name, "Mode") && !strcmp(icon, "mdi:fan-auto"));
	assert(nopt == 3 && !strcmp(opts[0], "Auto") && !strcmp(opts[1], "Silent") &&
	       !strcmp(opts[2], "Favorite"));

	/* PF mapping: Favorite level -> number (+ fan), mode -> select (+ fan) */
	assert(api_entity_dirty_from_pf(PF_FAV) == ((1u << E_FAN) | (1u << E_FAN_LEVEL)));
	assert(api_entity_dirty_from_pf(PF_MODE) == ((1u << E_FAN) | (1u << E_MODE)));
	assert(!(api_entity_throttled_mask() & ((1u << E_FAN_LEVEL) | (1u << E_MODE))));
	assert(api_entity_stateful_mask() & (1u << E_FAN_LEVEL));
	assert(api_entity_stateful_mask() & (1u << E_MODE));

	/* state: the Favorite level also in Auto/Silent; select = preset */
	s->mode = MODE_AUTO;
	s->favorite_level = 7;
	s->power = true;
	assert(num_state() == 8 && !strcmp(select_state(), "Auto") && !sel_missing);
	s->mode = MODE_SILENT;
	assert(num_state() == 8 && !strcmp(select_state(), "Silent"));
	s->favorite_level = 15;             /* custom rpm: clamped like the fan */
	assert(num_state() == 15);
	s->favorite_level = 0;
	assert(num_state() == 1);           /* 1.0f: not omitted as a default */

	/* number command, unit off in Silent: level + Favorite + power on */
	s->power = false;
	s->favorite_level = 7;
	n = enc_number_cmd(cmd, api_entity_key(E_FAN_LEVEL), 4.0f);
	assert(api_entity_command(MSG_NUMBER_CMD, cmd, n) == all3);
	assert(s->favorite_level == 3 && s->mode == MODE_FAVORITE && s->power);
	assert(num_state() == 4 && !strcmp(select_state(), "Favorite"));
	/* rounding and clamping */
	n = enc_number_cmd(cmd, api_entity_key(E_FAN_LEVEL), 9.6f);
	api_entity_command(MSG_NUMBER_CMD, cmd, n);
	assert(s->favorite_level == 9);
	n = enc_number_cmd(cmd, api_entity_key(E_FAN_LEVEL), 1e30f);
	api_entity_command(MSG_NUMBER_CMD, cmd, n);
	assert(s->favorite_level == 14);
	n = enc_number_cmd(cmd, api_entity_key(E_FAN_LEVEL), -3.0f);
	api_entity_command(MSG_NUMBER_CMD, cmd, n);
	assert(s->favorite_level == 0);
	n = enc_number_cmd(cmd, api_entity_key(E_FAN_LEVEL), NAN);
	assert(api_entity_command(MSG_NUMBER_CMD, cmd, n) == all3);   /* re-send only */
	assert(s->favorite_level == 0);
	/* wrong key / a number key in a select command: ignored */
	n = enc_number_cmd(cmd, api_entity_key(E_FAN), 5.0f);
	assert(api_entity_command(MSG_NUMBER_CMD, cmd, n) == 0);
	n = enc_select_cmd(cmd, api_entity_key(E_FAN_LEVEL), "Auto");
	assert(api_entity_command(MSG_SELECT_CMD, cmd, n) == 0);
	assert(s->favorite_level == 0 && s->mode == MODE_FAVORITE);

	/* select command */
	n = enc_select_cmd(cmd, api_entity_key(E_MODE), "Silent");
	assert(api_entity_command(MSG_SELECT_CMD, cmd, n) == all3);
	assert(s->mode == MODE_SILENT && !strcmp(select_state(), "Silent"));
	assert(num_state() == 1);           /* level kept */
	s->power = false;
	n = enc_select_cmd(cmd, api_entity_key(E_MODE), "Auto");
	api_entity_command(MSG_SELECT_CMD, cmd, n);
	assert(s->mode == MODE_AUTO && s->power);   /* a mode powers on (stock) */
	n = enc_select_cmd(cmd, api_entity_key(E_MODE), "Turbo");
	assert(api_entity_command(MSG_SELECT_CMD, cmd, n) == all3);   /* re-send */
	assert(s->mode == MODE_AUTO);

	/* rejected (door open): state still echoed */
	s->door_open = true;
	n = enc_select_cmd(cmd, api_entity_key(E_MODE), "Favorite");
	assert(api_entity_command(MSG_SELECT_CMD, cmd, n) == all3);
	assert(s->mode == MODE_AUTO);
	s->door_open = false;

	/* over a connection: the command's states are echoed right away */
	{
		mconn *c = connect_client(0);
		int before, after;
		push(c, 1, HELLO, sizeof(HELLO));
		push(c, 20, NULL, 0);
		poll_n(5);
		purifier_take_changes();
		before = out_types(c, T, 4096);
		n = enc_number_cmd(cmd, api_entity_key(E_FAN_LEVEL), 12.0f);
		push(c, MSG_NUMBER_CMD, cmd, n);
		poll_n(2);
		after = out_types(c, T, 4096);
		assert(count(T + before, after - before, 50) >= 1 &&
		       count(T + before, after - before, 53) >= 1 &&
		       count(T + before, after - before, 23) >= 1);
		assert(s->favorite_level == 11 && s->mode == MODE_FAVORITE);
		purifier_take_changes();
		disconnect_client(c);
	}

	/* back to the stub's initial state for the following tests */
	s->mode = MODE_AUTO;
	s->favorite_level = 7;
	s->power = true;
	purifier_take_changes();
	printf("  ok  fan_level number (1..15 slider) + mode select: list fields, "
	       "commands -> purifier, states echoed\n");
}


/* 7: the Auto speed bias number (ours): list fields, command clamps and
 * rounds to the step of 10, state echo. */
static float bias_state(void)
{
	st_msg m = decode_state(E_AUTO_BIAS);
	assert(!m.missing);
	return m.has_state ? m.state : 0.0f;   /* 0.0 omitted (proto3) */
}

static void test_auto_bias(void)
{
	purifier_state *s = (purifier_state *)purifier_get();
	const uint32_t me = 1u << E_AUTO_BIAS;
	uint8_t buf[128], cmd[32];
	pb_writer w;
	pb_reader r;
	pb_field f;
	float mn = 0, mx = 0, step = 0;
	unsigned mode = 9, cat = 9;
	char icon[24] = "", name[24] = "", oid[24] = "", unit[8] = "";
	size_t n;

	pb_writer_init(&w, buf, sizeof(buf));
	assert(api_entity_encode_list(E_AUTO_BIAS, &w) == 49 && !w.overflow);
	pb_reader_init(&r, buf, w.len);
	while (pb_next(&r, &f) > 0) {
		switch (f.field) {
		case 1: pb_field_str(&f, oid, sizeof(oid)); break;
		case 2: assert((uint32_t)f.u64 == api_entity_key(E_AUTO_BIAS)); break;
		case 3: pb_field_str(&f, name, sizeof(name)); break;
		case 5: pb_field_str(&f, icon, sizeof(icon)); break;
		case 6: assert(f.wt == PB_WT_I32); mn = f32(&f); break;
		case 7: assert(f.wt == PB_WT_I32); mx = f32(&f); break;
		case 8: assert(f.wt == PB_WT_I32); step = f32(&f); break;
		case 10: cat = (unsigned)f.u64; break;
		case 11: pb_field_str(&f, unit, sizeof(unit)); break;
		case 12: mode = (unsigned)f.u64; break;
		default: assert(0);
		}
	}
	assert(!strcmp(oid, "auto_bias") && !strcmp(name, "Auto speed bias") &&
	       !strcmp(icon, "mdi:tune-variant") && !strcmp(unit, "%"));
	assert(mn == -50 && mx == 50 && step == 10 && mode == NUMBER_MODE_SLIDER);
	assert(cat == ENTITY_CATEGORY_CONFIG);
	/* the fan level number keeps its own range */
	pb_writer_init(&w, buf, sizeof(buf));
	api_entity_encode_list(E_FAN_LEVEL, &w);
	pb_reader_init(&r, buf, w.len);
	while (pb_next(&r, &f) > 0) {
		if (f.field == 6) assert(f32(&f) == 1);
		if (f.field == 7) assert(f32(&f) == 15);
		if (f.field == 8) assert(f32(&f) == 1);
	}

	assert(api_entity_dirty_from_pf(PF_AUTO_BIAS) == me);
	assert(!(api_entity_dirty_from_pf(PF_FAV | PF_MODE | PF_RPM) & me));
	assert(!(api_entity_throttled_mask() & me));
	assert(api_entity_stateful_mask() & me);
	assert(!strcmp(api_entity_object_id(E_AUTO_BIAS), "auto_bias"));

	assert(s->auto_bias == 0 && bias_state() == 0);
	n = enc_number_cmd(cmd, api_entity_key(E_AUTO_BIAS), 20.0f);
	assert(api_entity_command(MSG_NUMBER_CMD, cmd, n) == me);
	assert(s->auto_bias == 20 && bias_state() == 20);
	assert(s->mode == MODE_AUTO);       /* not a fan level: no mode change */
	/* rounding to the step, halves away from zero */
	static const struct { float in; int out; } rt[] = {
		{ 24.9f, 20 }, { 25.0f, 30 }, { 4.9f, 0 }, { -4.9f, 0 },
		{ -5.0f, -10 }, { -26.0f, -30 }, { 44.0f, 40 }, { -0.0f, 0 },
		/* clamping */
		{ 55.0f, 50 }, { 1e30f, 50 }, { -51.0f, -50 }, { -1e30f, -50 },
		{ 0.0f, 0 },
	};
	for (unsigned i = 0; i < sizeof rt / sizeof rt[0]; i++) {
		n = enc_number_cmd(cmd, api_entity_key(E_AUTO_BIAS), rt[i].in);
		assert(api_entity_command(MSG_NUMBER_CMD, cmd, n) == me);
		if (s->auto_bias != rt[i].out) {
			fprintf(stderr, "bias %g -> %d, want %d\n", rt[i].in,
				s->auto_bias, rt[i].out);
			abort();
		}
		assert(bias_state() == rt[i].out);
	}
	s->auto_bias = 30;
	n = enc_number_cmd(cmd, api_entity_key(E_AUTO_BIAS), NAN);
	assert(api_entity_command(MSG_NUMBER_CMD, cmd, n) == me);  /* re-send */
	assert(s->auto_bias == 30);
	n = enc_select_cmd(cmd, api_entity_key(E_AUTO_BIAS), "Auto");
	assert(api_entity_command(MSG_SELECT_CMD, cmd, n) == 0);
	assert(s->auto_bias == 30);
	s->auto_bias = -40;
	assert(bias_state() == -40);
	s->auto_bias = 0;

	/* over a connection: command -> number state echoed */
	{
		mconn *c = connect_client(0);
		int before, after;
		push(c, 1, HELLO, sizeof(HELLO));
		push(c, 20, NULL, 0);
		poll_n(5);
		purifier_take_changes();
		before = out_types(c, T, 4096);
		n = enc_number_cmd(cmd, api_entity_key(E_AUTO_BIAS), -20.0f);
		push(c, MSG_NUMBER_CMD, cmd, n);
		poll_n(2);
		after = out_types(c, T, 4096);
		assert(count(T + before, after - before, 50) >= 1);
		assert(s->auto_bias == -20);
		purifier_take_changes();
		disconnect_client(c);
	}
	s->auto_bias = 0;
	purifier_take_changes();
	printf("  ok  auto_bias number (-50..50 step 10, %%, config): list fields, "
	       "clamp/round, echo\n");
}

/* 8: diagnostics (U1/U2): text sensors, uptime / heap throttling, the
 * deferred Restart button and its refusals. */
static const char *text_of(unsigned idx, int *missing)
{
	static char t[64];
	uint8_t buf[96];
	pb_writer w;
	pb_reader r;
	pb_field f;
	pb_writer_init(&w, buf, sizeof(buf));
	assert(api_entity_encode_state(idx, &w) == MSG_TEXT_SENSOR_STATE && !w.overflow);
	pb_reader_init(&r, buf, w.len);
	t[0] = 0;
	*missing = 0;
	while (pb_next(&r, &f) > 0) {
		if (f.field == 1)
			assert((uint32_t)f.u64 == api_entity_key(idx));
		else if (f.field == 2)
			pb_field_str(&f, t, sizeof(t));
		else if (f.field == 3)
			*missing = f.u64 != 0;
	}
	return t;
}

typedef struct {
	char oid[24], name[24], icon[24], unit[8], dc[16];
	unsigned cat, sc, acc;
} list_msg;

static list_msg decode_list(unsigned idx, uint16_t want_type)
{
	uint8_t buf[160];
	pb_writer w;
	pb_reader r;
	pb_field f;
	list_msg m;
	memset(&m, 0, sizeof(m));
	pb_writer_init(&w, buf, sizeof(buf));
	assert(api_entity_encode_list(idx, &w) == want_type && !w.overflow);
	pb_reader_init(&r, buf, w.len);
	while (pb_next(&r, &f) > 0) {
		switch (f.field) {
		case 1: pb_field_str(&f, m.oid, sizeof(m.oid)); break;
		case 2: assert((uint32_t)f.u64 == api_entity_key(idx)); break;
		case 3: pb_field_str(&f, m.name, sizeof(m.name)); break;
		case 5: pb_field_str(&f, m.icon, sizeof(m.icon)); break;
		default:
			if (want_type == MSG_LIST_SENSOR_RESP) {
				if (f.field == 6) pb_field_str(&f, m.unit, sizeof(m.unit));
				if (f.field == 7) m.acc = (unsigned)f.u64;
				if (f.field == 9) pb_field_str(&f, m.dc, sizeof(m.dc));
				if (f.field == 10) m.sc = (unsigned)f.u64;
				if (f.field == 13) m.cat = (unsigned)f.u64;
			} else {           /* text sensor and button: same layout */
				if (f.field == 7) m.cat = (unsigned)f.u64;
				if (f.field == 8) pb_field_str(&f, m.dc, sizeof(m.dc));
			}
		}
	}
	return m;
}

static size_t enc_button_cmd(uint8_t *buf, uint32_t key)
{
	pb_writer w;
	pb_writer_init(&w, buf, 16);
	pb_w_fixed32_force(&w, 1, key);
	return w.len;
}

static void press_restart(void)
{
	uint8_t cmd[16];
	size_t n = enc_button_cmd(cmd, api_entity_key(E_RESTART));
	assert(api_entity_command(MSG_BUTTON_CMD, cmd, n) == 0);
}

static void test_diagnostics(void)
{
	list_msg m;
	int miss;
	uint32_t bits, t0;

	/* reset reason mapping (PMU LAST_RST_CAUSE bits, priority order) */
	assert(!strcmp(diag_reason_from_cause(0, 0), "power_on"));
	assert(!strcmp(diag_reason_from_cause(1, 0), "brownout"));
	assert(!strcmp(diag_reason_from_cause(2, 0), "brownout"));
	assert(!strcmp(diag_reason_from_cause(4, 0), "brownout"));
	assert(!strcmp(diag_reason_from_cause(8, 0), "software"));
	assert(!strcmp(diag_reason_from_cause(8, DIAG_REBOOT_RESTART), "restart"));
	assert(!strcmp(diag_reason_from_cause(0, DIAG_REBOOT_RESTART), "power_on"));
	assert(!strcmp(diag_reason_from_cause(8 | 1, 0), "software"));
	assert(!strcmp(diag_reason_from_cause(16 | 8, 0), "lockup"));
	assert(!strcmp(diag_reason_from_cause(32 | 16 | 8, DIAG_REBOOT_RESTART), "watchdog"));

	/* list fields */
	static const struct { unsigned idx; const char *oid, *name, *icon; } texts[] = {
		{ E_FIRMWARE, "firmware", "Firmware", "mdi:chip" },
		{ E_RESET_REASON, "reset_reason", "Reset reason", "mdi:restart-alert" },
		{ E_OTA_SLOT, "ota_slot", "OTA slot", "mdi:swap-horizontal" },
		{ E_IP, "ip_address", "IP address", "mdi:ip-network" },
	};
	for (unsigned i = 0; i < 4; i++) {
		m = decode_list(texts[i].idx, MSG_LIST_TEXT_SENSOR_RESP);
		assert(!strcmp(m.oid, texts[i].oid) && !strcmp(m.name, texts[i].name));
		assert(!strcmp(m.icon, texts[i].icon) && !m.dc[0]);
		assert(m.cat == ENTITY_CATEGORY_DIAGNOSTIC);
	}
	m = decode_list(E_UPTIME, MSG_LIST_SENSOR_RESP);
	assert(!strcmp(m.oid, "uptime") && !strcmp(m.name, "Uptime") && !strcmp(m.unit, "s"));
	assert(!strcmp(m.dc, "duration") && m.sc == STATE_CLASS_TOTAL_INCREASING && m.acc == 0);
	assert(m.cat == ENTITY_CATEGORY_DIAGNOSTIC);
	m = decode_list(E_HEAP, MSG_LIST_SENSOR_RESP);
	assert(!strcmp(m.oid, "free_heap") && !strcmp(m.unit, "B") && !strcmp(m.dc, "data_size"));
	assert(m.sc == STATE_CLASS_MEASUREMENT && m.cat == ENTITY_CATEGORY_DIAGNOSTIC);
	m = decode_list(E_HEAP_MIN, MSG_LIST_SENSOR_RESP);
	assert(!strcmp(m.oid, "min_free_heap") && !strcmp(m.name, "Min free heap"));
	m = decode_list(E_RESTART, MSG_LIST_BUTTON_RESP);
	assert(!strcmp(m.oid, "restart") && !strcmp(m.name, "Restart"));
	assert(!strcmp(m.dc, "restart") && m.cat == ENTITY_CATEGORY_DIAGNOSTIC);
	assert(!(api_entity_stateful_mask() & (1u << E_RESTART)));
	assert(api_entity_stateful_mask() & (1u << E_FIRMWARE));
	assert(api_entity_throttled_mask() & (1u << E_UPTIME));
	assert(!(api_entity_throttled_mask() & (1u << E_IP)));
	/* no diagnostic entity carries a PF flag: purifier changes never push them */
	assert(!(api_entity_dirty_from_pf(0xffffffffu) &
		 ((1u << E_FIRMWARE) | (1u << E_UPTIME) | (1u << E_HEAP) | (1u << E_IP))));

	/* text states */
	assert(!strcmp(text_of(E_FIRMWARE, &miss), "0.1.0-host") && !miss);
	assert(!strcmp(text_of(E_RESET_REASON, &miss), "power_on") && !miss);
	diag_stub.pmu_cause = 8;
	diag_stub.prev_tag = DIAG_REBOOT_RESTART;
	assert(!strcmp(text_of(E_RESET_REASON, &miss), "restart"));
	diag_stub.prev_tag = 0;
	assert(!strcmp(text_of(E_RESET_REASON, &miss), "software"));
	diag_stub.pmu_cause = 0;
	assert(!strcmp(text_of(E_OTA_SLOT, &miss), "A") && !miss);
	assert(!strcmp(text_of(E_IP, &miss), "") && miss);           /* no IP: missing */

	/* IP: checked every 1 s, pushed on change */
	t0 = now_ms;
	api_entity_poll(now_ms);
	mock_ip = 0x3201a8c0u;     /* 192.168.1.50 in network order (LE host) */
	now_ms += 500;
	assert(!(api_entity_poll(now_ms) & (1u << E_IP)));
	now_ms += 600;
	assert(api_entity_poll(now_ms) == (1u << E_IP));
	assert(!strcmp(text_of(E_IP, &miss), "192.168.1.50") && !miss);
	now_ms += 1100;
	assert(api_entity_poll(now_ms) == 0);

	/* OTA slot: trial re-read every 60 s until confirmed, then pushed */
	diag_stub.running = 'B';
	diag_stub.trial = diag_stub.can_revert = 1;
	api_entities_init();       /* boot: slot read once */
	t0 = now_ms;
	assert(!strcmp(text_of(E_OTA_SLOT, &miss), "B (trial)"));
	diag_stub.trial = diag_stub.can_revert = 0;   /* confirmed */
	now_ms = t0 + 59000;
	assert(!(api_entity_poll(now_ms) & (1u << E_OTA_SLOT)));
	now_ms = t0 + 60000;
	bits = api_entity_poll(now_ms);
	assert(bits & (1u << E_OTA_SLOT));
	assert(!strcmp(text_of(E_OTA_SLOT, &miss), "B"));
	diag_stub.running = 'A';
	now_ms += 60000;
	assert(!(api_entity_poll(now_ms) & (1u << E_OTA_SLOT)));   /* not re-read */

	/* uptime / heap: at most once per 60 s; heap only on >= 1 KB moves */
	api_entities_init();
	t0 = now_ms;
	diag_stub.uptime_s += 30;
	diag_stub.heap = 100000 - 900;
	now_ms = t0 + 59999;
	assert(api_entity_poll(now_ms) == 0);
	now_ms = t0 + 60000;
	assert(api_entity_poll(now_ms) == (1u << E_UPTIME));       /* heap: -900 only */
	assert(decode_state(E_UPTIME).state == (float)diag_stub.uptime_s);
	diag_stub.heap = 100000 - 1100;                            /* -1100 vs last sent */
	now_ms += 60000;
	assert(api_entity_poll(now_ms) == ((1u << E_HEAP) | (1u << E_HEAP_MIN)));
	assert(decode_state(E_HEAP).state == 98900.0f && decode_state(E_HEAP_MIN).state == 98900.0f);
	now_ms += 60000;
	assert(api_entity_poll(now_ms) == 0);                      /* nothing moved */
	/* a short dip is caught by the min (sampled every poll) */
	diag_stub.heap = 90000;
	api_entity_poll(now_ms + 10);
	diag_stub.heap = 98900;
	now_ms += 60000;
	assert(api_entity_poll(now_ms) == (1u << E_HEAP_MIN));
	assert(decode_state(E_HEAP_MIN).state == 90000.0f && decode_state(E_HEAP).state == 98900.0f);

	/* over a connection, 10 simulated minutes with uptime ticking and the
	 * heap swinging by 4 KB every poll: <= 1 uptime + 1 heap push a minute */
	{
		mconn *c = connect_client(0);
		int before, after, n25;
		push(c, 1, HELLO, sizeof(HELLO));
		push(c, 20, NULL, 0);
		poll_n(5);
		purifier_take_changes();
		before = out_types(c, T, 4096);
		for (int i = 0; i < 6000; i++) {       /* 100 ms steps */
			now_ms += 100;
			if (i % 10 == 0)
				diag_stub.uptime_s++;
			if (i % 300 == 0)
				push(c, 7, NULL, 0);   /* keepalive: the server drops a silent peer at 150 s */
			diag_stub.heap = (i & 1) ? 96000 : 100000;
			api_server_poll();
		}
		after = out_types(c, T, 4096);
		n25 = count(T + before, after - before, 25);
		/* also: the stub fan ramps (rpm sensor) and RSSI is constant */
		purifier_take_changes();
		assert(n25 >= 10 && n25 <= 3 * 10 + 2);
		disconnect_client(c);
		printf("  ok  10 min: %d sensor pushes (uptime + heap + min, <= 3/min)\n", n25);
	}
	diag_stub.heap = 100000;

	/* Restart: deferred 500 ms, run from the poll */
	api_entities_init();
	diag_stub.reboots = 0;
	diag_stub.last_tag = 0;
	t0 = now_ms;
	press_restart();
	assert(diag_stub.reboots == 0);
	now_ms = t0 + 499;
	api_entity_poll(now_ms);
	assert(diag_stub.reboots == 0);
	press_restart();           /* a second press does not re-arm the delay */
	now_ms = t0 + 500;
	api_entity_poll(now_ms);
	assert(diag_stub.reboots == 1 && diag_stub.last_tag == DIAG_REBOOT_RESTART);
	now_ms += 1000;
	api_entity_poll(now_ms);
	assert(diag_stub.reboots == 1);   /* once */

	/* refused: upload in progress, update committed, unconfirmed trial */
	diag_stub.last_tag = 0;
	for (int k = 0; k < 3; k++) {
		diag_stub.in_progress = k == 0;
		diag_stub.update_pending = k == 1;
		diag_stub.trial = diag_stub.can_revert = k == 2;
		press_restart();
		now_ms += 1000;
		api_entity_poll(now_ms);
		assert(diag_stub.reboots == 1 && diag_stub.last_tag == 0);
	}
	diag_stub.in_progress = diag_stub.update_pending = 0;
	/* a trial without a valid previous slot (cannot revert): allowed */
	diag_stub.trial = 1;
	diag_stub.can_revert = 0;
	press_restart();
	now_ms += 600;
	api_entity_poll(now_ms);
	assert(diag_stub.reboots == 2);
	diag_stub.trial = 0;
	/* an upload that starts during the 500 ms cancels the restart */
	press_restart();
	diag_stub.in_progress = 1;
	now_ms += 600;
	api_entity_poll(now_ms);
	assert(diag_stub.reboots == 2);
	diag_stub.in_progress = 0;
	now_ms += 600;
	api_entity_poll(now_ms);
	assert(diag_stub.reboots == 2);   /* cancelled, not just postponed */

	/* over a connection: ButtonCommandRequest -> reboot from the poll */
	{
		uint8_t cmd[16];
		mconn *c = connect_client(0);
		push(c, 1, HELLO, sizeof(HELLO));
		push(c, 20, NULL, 0);
		push(c, MSG_BUTTON_CMD, cmd, enc_button_cmd(cmd, api_entity_key(E_RESTART)));
		poll_n(5);
		assert(diag_stub.reboots == 2);
		now_ms += 500;
		poll_n(1);
		assert(diag_stub.reboots == 3);
		disconnect_client(c);
	}
	mock_ip = 0;
	api_entities_init();
	printf("  ok  diagnostics: text sensors, reset-reason map, uptime/heap/IP/slot "
	       "push policy, Restart deferred 500 ms + refused during upload/commit/trial\n");
}

/* ======================= Noise (encrypted) mode ======================= */
static const uint8_t SRV_HELLO[] = "\x01\x00\x13\x01unit\0" "222222222222";  /* + final \0 */
#define SRV_HELLO_LEN 22
#define HS_OUT_LEN (SRV_HELLO_LEN + 3 + 1 + 48)
static struct nini NI[MAXC];

/* Connect a Noise client: queue the client hello and msg1 (a real client
 * sends them back to back too).  The handshake completes in poll_noise(). */
static mconn *noise_client(int i, const uint8_t psk[32])
{
	uint8_t m1[48];
	mconn *c = connect_client(i);
	ini_start(&NI[i], psk, (uint8_t)(0x40 + i), m1);
	push_raw(c, "\x01\x00\x00", 3);
	push_raw(c, "\x01\x00\x31\x00", 4);
	push_raw(c, m1, 48);
	return c;
}

/* Once the server hello + msg2 are out, check them and derive the keys. */
static int noise_complete(mconn *c)
{
	if (c->nz || c->out_len < HS_OUT_LEN)
		return 0;
	assert(!memcmp(c->out, SRV_HELLO, SRV_HELLO_LEN));
	assert(!memcmp(c->out + SRV_HELLO_LEN, "\x01\x00\x31\x00", 4));
	assert(ini_finish(&NI[c - mc], c->out + SRV_HELLO_LEN + 4) == 0);
	NI[c - mc].data_off = HS_OUT_LEN;
	c->nz = &NI[c - mc];
	return 1;
}

static void expect_out(const mconn *c, size_t off, const char *bytes, size_t n)
{
	if (c->out_len != off + n || memcmp(c->out + off, bytes, n)) {
		fprintf(stderr, "unexpected output (%zu bytes, wanted %zu)\n", c->out_len - off, n);
		abort();
	}
}

/* N1: handshake + full HA-like script, random chunking both directions,
 * two clients; the decrypted reply types must equal the plaintext run. */
static void test_noise_random_chunking(const unsigned *plain_ref, int nplain)
{
	static unsigned ref[4096];
	int nref;
	mconn *a = noise_client(0, PSK);
	poll_n(3);
	assert(noise_complete(a));
	build_script(a);
	poll_n(50);
	nref = out_types(a, ref, 4096);
	assert(nref == nplain && !memcmp(ref, plain_ref, nref * sizeof(*ref)));
	disconnect_client(a);

	for (int round = 0; round < 200; round++) {
		mconn *x = noise_client(0, PSK), *y = noise_client(1, PSK);
		int scripted = 0;
		for (int i = 0; i < 40000; i++) {
			x->recv_max = 1 + rnd() % 40;
			y->recv_max = 1 + rnd() % 40;
			x->send_cap = rnd() % 64;
			y->send_cap = rnd() % 64;
			now_ms += rnd() % 3;
			api_server_poll();
			if (noise_complete(x))
				build_script(x), scripted++;
			if (noise_complete(y))
				build_script(y), scripted++;
			if (scripted == 2 && x->in_pos == x->in_len && y->in_pos == y->in_len && i > 500)
				break;
		}
		assert(scripted == 2);
		x->send_cap = y->send_cap = 1u << 20;
		poll_n(5);
		assert(x->in_pos == x->in_len && y->in_pos == y->in_len);
		check_same(x, ref, nref, round);
		check_same(y, ref, nref, round);
		disconnect_client(x);
		disconnect_client(y);
	}
	printf("  ok  Noise: 200 rounds x 2 clients, handshake + script with random recv/send\n"
	       "      chunking -> same %d replies as plaintext, all MACs verify\n", nref);
}

static void expect_reject(mconn *c, size_t off, const char *reason)
{
	char b[64];
	size_t n = strlen(reason);
	b[0] = 1;
	b[1] = 0;
	b[2] = (char)(n + 1);
	b[3] = 1;
	memcpy(b + 4, reason, n);
	poll_n(3);
	expect_out(c, off, b, 4 + n);
	assert(!c->open);
}

/* N2: every reject path, byte for byte as ESPHome's frame helper. */
static void test_noise_rejects(void)
{
	uint8_t bad_psk[32], m1[48];
	mconn *c;

	/* plaintext client -> 01 00 13 01 "Bad indicator byte", close */
	c = connect_client(0);
	push(c, 1, HELLO, sizeof(HELLO));
	poll_n(2);
	expect_reject(c, 0, "Bad indicator byte");
	printf("  ok  plaintext client -> \"Bad indicator byte\" + close\n");

	/* wrong PSK -> server hello, then "Handshake MAC failure" */
	memcpy(bad_psk, PSK, 32);
	bad_psk[31] ^= 0x80;
	c = noise_client(0, bad_psk);
	poll_n(2);
	expect_reject(c, SRV_HELLO_LEN, "Handshake MAC failure");
	assert(!memcmp(c->out, SRV_HELLO, SRV_HELLO_LEN));
	printf("  ok  wrong key -> server hello + \"Handshake MAC failure\" + close\n");

	/* empty handshake message */
	c = connect_client(0);
	push_raw(c, "\x01\x00\x00\x01\x00\x00", 6);
	poll_n(2);
	expect_reject(c, SRV_HELLO_LEN, "Empty handshake message");
	/* error byte != 0 */
	c = connect_client(0);
	ini_start(&NI[0], PSK, 0x40, m1);
	push_raw(c, "\x01\x00\x00\x01\x00\x31\x01", 7);
	push_raw(c, m1, 48);
	poll_n(2);
	expect_reject(c, SRV_HELLO_LEN, "Bad handshake error byte");
	/* handshake frame over 128 bytes */
	c = connect_client(0);
	push_raw(c, "\x01\x00\x00\x01\x00\x81", 6);
	poll_n(2);
	expect_reject(c, SRV_HELLO_LEN, "Bad handshake packet len");
	/* client hello over 128 bytes */
	c = connect_client(0);
	push_raw(c, "\x01\x00\x81", 3);
	poll_n(2);
	expect_reject(c, 0, "Bad handshake packet len");
	/* truncated msg1 */
	c = connect_client(0);
	push_raw(c, "\x01\x00\x00\x01\x00\x0b\x00" "0123456789", 17);
	poll_n(2);
	expect_reject(c, SRV_HELLO_LEN, "Handshake error");
	printf("  ok  empty / bad error byte / oversized / truncated handshake -> ESPHome reasons\n");

	/* data phase: a tampered frame closes silently */
	c = noise_client(0, PSK);
	poll_n(2);
	assert(noise_complete(c));
	push(c, 1, HELLO, sizeof(HELLO));
	push(c, 7, NULL, 0);
	c->in[c->in_len - 1] ^= 1;          /* flip a tag bit of the ping */
	poll_n(3);
	assert(!c->open);
	assert(out_types(c, T, 8) == 1 && T[0] == 2);   /* hello answered, then nothing */
	printf("  ok  tampered data frame -> silent close (hello before it answered)\n");

	/* oversized encrypted message skipped, nonces stay in step */
	{
		static uint8_t big[600];
		memset(big, 'y', sizeof(big));
		c = noise_client(0, PSK);
		poll_n(2);
		assert(noise_complete(c));
		push(c, 1, HELLO, sizeof(HELLO));
		push(c, 9999, big, sizeof(big));
		push(c, 7, NULL, 0);
		c->recv_max = 50;
		poll_n(60);
		assert(c->open && c->in_pos == c->in_len);
		assert(out_types(c, T, 8) == 2 && T[0] == 2 && T[1] == 8);
		disconnect_client(c);
		printf("  ok  600 B encrypted message skipped, following ping decrypts and is answered\n");
	}

	/* the handshake times out like ESPHome (60 s) */
	c = noise_client(0, PSK);
	c->in_len = 3;                      /* only the client hello arrives */
	poll_n(2);
	now_ms += 59000;
	poll_n(1);
	assert(c->open);
	now_ms += 2000;
	poll_n(3);
	assert(!c->open);
	printf("  ok  handshake not finished in 60 s -> closed\n");
}

/* N3: a Noise client hitting the plaintext server gets "\0Bad indicator byte". */
static void test_noise_client_vs_plaintext(void)
{
	mconn *c = noise_client(0, PSK);
	poll_n(3);
	expect_out(c, 0, "\x00" "Bad indicator byte", 19);
	assert(!c->open);
	printf("  ok  Noise client vs plaintext server -> \"\\0Bad indicator byte\" + close\n");
}

/* rng.c needs this; deterministic in the unit test */
int api_platform_entropy(void *buf, size_t len)
{
	static uint32_t x = 1;
	uint8_t *p = buf;
	for (size_t i = 0; i < len; i++) {
		x = x * 1664525u + 1013904223u;
		p[i] = (uint8_t)(x >> 24);
	}
	return 0;
}

int main(void)
{
	api_config cfg = { "unit", "Unit", "", "0", "22:22:22:22:22:22", "" };
	static unsigned plain_ref[4096];
	int nplain;
	mconn *a;
	purifier_init();
	purifier_take_changes();
	assert(api_server_init(&cfg) == 0);
	printf("test_server:\n");
	test_coalesced_tail();
	test_deferred_without_new_data();
	test_oversized_split();
	test_random_chunking();
	test_fault_entities();
	test_fan_controls();
	test_auto_bias();
	test_diagnostics();

	a = connect_client(0);
	build_script(a);
	poll_n(50);
	nplain = out_types(a, plain_ref, 4096);
	disconnect_client(a);
	test_noise_client_vs_plaintext();

	/* invalid keys are refused (never a plaintext fallback) */
	cfg.encryption_key = "not base64";
	assert(api_server_init(&cfg) == -2);
	cfg.encryption_key = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHg==";   /* 31 B */
	assert(api_server_init(&cfg) == -2);
	a = connect_client(0);              /* ...and later polls never listen/accept */
	for (int i = 0; i < 20; i++) {
		now_ms += 10000;
		api_server_poll();
	}
	assert(a->pending && !a->open);
	a->pending = 0;
	printf("  ok  invalid keys -> api_server_init() fails, nothing served afterwards\n");

	for (int i = 0; i < 32; i++)
		PSK[i] = (uint8_t)(i * 7 + 3);
	cfg.encryption_key = "AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dw=";
	assert(api_server_init(&cfg) == 0);
	test_noise_rejects();
	test_noise_random_chunking(plain_ref, nplain);
	printf("test_server: OK\n");
	return 0;
}
