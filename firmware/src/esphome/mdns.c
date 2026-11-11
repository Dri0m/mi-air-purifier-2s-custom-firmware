/* mdns.c - minimal mDNS responder (RFC 6762/6763) advertising the ESPHome
 * API like ESPHome's mdns component does:
 *
 *   <name>.local                     A    -> our IPv4
 *   _esphomelib._tcp.local           PTR  -> <name>._esphomelib._tcp.local
 *   <name>._esphomelib._tcp.local    SRV  -> <name>.local:6053
 *   <name>._esphomelib._tcp.local    TXT  friendly_name= version= mac=
 *                                         platform= board= network=wifi
 *                                         project_name= project_version=
 *                                         [api_encryption=Noise_NNpsk0_...]
 *   _services._dns-sd._udp.local     PTR  -> _esphomelib._tcp.local
 *
 * Announces twice (1 s apart) at start-up and whenever the IP changes.
 * Answers multicast queries by multicast, QU queries by unicast and legacy
 * (source port != 5353) queries by unicast with the question echoed.
 * Not implemented: probing/conflict resolution, known-answer suppression,
 * response delay/aggregation, IPv6/AAAA, NSEC. */
#include <string.h>
#include "api_internal.h"
#include "noise.h"
#include "hal.h"
#include "netio.h"

#define MDNS_PORT 5353
#define MDNS_BUF 512
#define TTL_HOST 120               /* A, SRV (RFC 6762 10: host records) */
#define TTL_OTHER 4500             /* PTR, TXT */
#define TTL_LEGACY 10
#define ANNOUNCE_COUNT 2
#define ANNOUNCE_GAP_MS 1000u
#define REBIND_MS 5000u

#define T_A 1
#define T_PTR 12
#define T_TXT 16
#define T_AAAA 28
#define T_SRV 33
#define T_ANY 255
#define CLASS_IN 1
#define CACHE_FLUSH 0x8000
#define QU_BIT 0x8000

/* record bits */
#define R_A    0x01
#define R_PTR  0x02
#define R_SRV  0x04
#define R_TXT  0x08
#define R_DPTR 0x10

static const char SVC[] = "_esphomelib._tcp.local";
static const char DNSSD[] = "_services._dns-sd._udp.local";
static char host[48];                  /* "<name>.local" */
static char inst[64];                  /* "<name>._esphomelib._tcp.local" */
static char txt[320];                  /* length-prefixed TXT rdata */
static size_t txt_len;
static netio_sock sock = -1;
static uint32_t cur_ip;
static uint8_t announce_left;
static uint32_t announce_ms, rebind_ms;
static bool inited;
static uint8_t rxb[MDNS_BUF], txb[MDNS_BUF];

static uint32_t mcast_be(void)
{
	static const uint8_t g[4] = { 224, 0, 0, 251 };
	uint32_t v;
	memcpy(&v, g, 4);
	return v;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool name_eq(const char *a, const char *b)
{
	while (*a && lower(*a) == lower(*b)) {
		a++;
		b++;
	}
	return !*a && !*b;
}

/* ---- packet writer with suffix compression ---- */
typedef struct {
	uint8_t *buf;
	size_t len, cap;
	bool overflow;
	uint8_t ndict;
	const char *dict_name[8];      /* suffix strings already written */
	uint16_t dict_off[8];
} pkt_w;

static void put(pkt_w *w, const void *d, size_t n)
{
	if (w->overflow || w->cap - w->len < n) {
		w->overflow = true;
		return;
	}
	memcpy(w->buf + w->len, d, n);
	w->len += n;
}

static void put16(pkt_w *w, uint16_t v)
{
	uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
	put(w, b, 2);
}

static void put32(pkt_w *w, uint32_t v)
{
	put16(w, (uint16_t)(v >> 16));
	put16(w, (uint16_t)v);
}

static void put_name(pkt_w *w, const char *name)
{
	while (*name) {
		const char *dot;
		size_t n;
		for (unsigned i = 0; i < w->ndict; i++)
			if (name_eq(w->dict_name[i], name)) {
				put16(w, (uint16_t)(0xC000 | w->dict_off[i]));
				return;
			}
		if (w->ndict < 8 && w->len < 0x3FFF) {
			w->dict_name[w->ndict] = name;
			w->dict_off[w->ndict++] = (uint16_t)w->len;
		}
		dot = strchr(name, '.');
		n = dot ? (size_t)(dot - name) : strlen(name);
		if (n > 63)
			n = 63;
		{
			uint8_t l = (uint8_t)n;
			put(w, &l, 1);
		}
		put(w, name, n);
		name += n;
		if (*name == '.')
			name++;
	}
	put(w, "", 1);
}

/* Write an RR header; returns offset of the rdlength field. */
static size_t rr_begin(pkt_w *w, const char *name, uint16_t type, bool flush,
		       uint32_t ttl)
{
	size_t at;
	put_name(w, name);
	put16(w, type);
	put16(w, (uint16_t)(CLASS_IN | (flush ? CACHE_FLUSH : 0)));
	put32(w, ttl);
	at = w->len;
	put16(w, 0);
	return at;
}

static void rr_end(pkt_w *w, size_t at)
{
	size_t n = w->len - at - 2;
	if (w->overflow)
		return;
	w->buf[at] = (uint8_t)(n >> 8);
	w->buf[at + 1] = (uint8_t)n;
}

static void put_records(pkt_w *w, unsigned bits, bool legacy, uint16_t *count)
{
	size_t at;
	bool fl = !legacy;
	uint32_t tt = legacy ? TTL_LEGACY : TTL_OTHER;
	uint32_t th = legacy ? TTL_LEGACY : TTL_HOST;

	if (bits & R_DPTR) {
		at = rr_begin(w, DNSSD, T_PTR, false, tt);
		put_name(w, SVC);
		rr_end(w, at);
		(*count)++;
	}
	if (bits & R_PTR) {
		at = rr_begin(w, SVC, T_PTR, false, tt);
		put_name(w, inst);
		rr_end(w, at);
		(*count)++;
	}
	if (bits & R_SRV) {
		at = rr_begin(w, inst, T_SRV, fl, th);
		put16(w, 0);             /* priority */
		put16(w, 0);             /* weight */
		put16(w, API_PORT);
		put_name(w, host);
		rr_end(w, at);
		(*count)++;
	}
	if (bits & R_TXT) {
		at = rr_begin(w, inst, T_TXT, fl, tt);
		put(w, txt, txt_len);
		rr_end(w, at);
		(*count)++;
	}
	if ((bits & R_A) && cur_ip) {
		at = rr_begin(w, host, T_A, fl, th);
		put(w, &cur_ip, 4);      /* already network order */
		rr_end(w, at);
		(*count)++;
	}
}

static void send_response(unsigned ans, unsigned add, uint16_t id, const uint8_t *q,
			  size_t qlen, uint16_t qdcount, uint32_t dst_ip, uint16_t dst_port)
{
	pkt_w w;
	uint16_t an = 0, ar = 0;
	bool legacy = dst_port != MDNS_PORT;

	memset(&w, 0, sizeof(w));
	w.buf = txb;
	w.cap = sizeof(txb);
	put16(&w, legacy ? id : 0);
	put16(&w, 0x8400);               /* QR | AA */
	put16(&w, legacy ? qdcount : 0);
	put16(&w, 0);
	put16(&w, 0);
	put16(&w, 0);
	if (legacy) {
		/* Echoed raw; any compression pointers in it stay valid
		 * because the question section starts at the same offset. */
		put(&w, q, qlen);
		w.ndict = 0;
	}
	put_records(&w, ans, legacy, &an);
	{
		size_t before = w.len;
		put_records(&w, add, legacy, &ar);
		if (w.overflow) {        /* drop additionals rather than all */
			w.overflow = false;
			w.len = before;
			ar = 0;
		}
	}
	if (w.overflow || !an)
		return;
	txb[6] = (uint8_t)(an >> 8);
	txb[7] = (uint8_t)an;
	txb[10] = (uint8_t)(ar >> 8);
	txb[11] = (uint8_t)ar;
	netio_udp_sendto(sock, txb, w.len, dst_ip, dst_port);
}

/* Read a (possibly compressed) name as a dotted string. */
static int read_name(const uint8_t *p, size_t len, size_t *off, char *out, size_t outsz)
{
	size_t o = *off, n = 0;
	bool jumped = false;
	int hops = 0;
	for (;;) {
		uint8_t l;
		if (o >= len)
			return -1;
		l = p[o];
		if ((l & 0xC0) == 0xC0) {
			if (o + 1 >= len || ++hops > 16)
				return -1;
			if (!jumped)
				*off = o + 2;
			jumped = true;
			o = ((size_t)(l & 0x3F) << 8) | p[o + 1];
			continue;
		}
		if (l & 0xC0)
			return -1;
		o++;
		if (!l)
			break;
		if (o + l > len)
			return -1;
		if (n && n < outsz)
			out[n++] = '.';
		for (unsigned i = 0; i < l; i++)
			if (n < outsz)
				out[n++] = (char)p[o + i];
		o += l;
	}
	if (!jumped)
		*off = o;
	if (n >= outsz)
		return -1;              /* too long to be one of ours */
	out[n] = 0;
	return 0;
}

static void handle_packet(const uint8_t *p, size_t len, uint32_t src_ip, uint16_t src_port)
{
	uint16_t id, flags, qd;
	size_t off = 12, qstart = 12;
	unsigned ans = 0, add = 0;
	bool unicast = src_port != MDNS_PORT;
	char name[96];

	if (len < 12)
		return;
	id = (uint16_t)(p[0] << 8 | p[1]);
	flags = (uint16_t)(p[2] << 8 | p[3]);
	qd = (uint16_t)(p[4] << 8 | p[5]);
	if (flags & 0x8000)             /* a response, not a query */
		return;
	if (flags & 0x7800)             /* opcode != QUERY */
		return;
	for (unsigned i = 0; i < qd; i++) {
		uint16_t qt, qc;
		if (read_name(p, len, &off, name, sizeof(name)) < 0 || off + 4 > len)
			return;
		qt = (uint16_t)(p[off] << 8 | p[off + 1]);
		qc = (uint16_t)(p[off + 2] << 8 | p[off + 3]);
		off += 4;
		if ((qc & 0x7FFF) != CLASS_IN && (qc & 0x7FFF) != 255)
			continue;
		if (qc & QU_BIT)
			unicast = true;
		if (name_eq(name, host)) {
			if (qt == T_A || qt == T_ANY)
				ans |= R_A;
		} else if (name_eq(name, SVC)) {
			if (qt == T_PTR || qt == T_ANY) {
				ans |= R_PTR;
				add |= R_SRV | R_TXT | R_A;
			}
		} else if (name_eq(name, inst)) {
			if (qt == T_SRV || qt == T_ANY) {
				ans |= R_SRV;
				add |= R_A;
			}
			if (qt == T_TXT || qt == T_ANY)
				ans |= R_TXT;
		} else if (name_eq(name, DNSSD)) {
			if (qt == T_PTR || qt == T_ANY)
				ans |= R_DPTR;
		}
	}
	add &= ~ans;
	if (!ans)
		return;
	if (src_port != MDNS_PORT)
		send_response(ans, add, id, p + qstart, off - qstart, qd, src_ip, src_port);
	else if (unicast)
		send_response(ans, add, 0, NULL, 0, 0, src_ip, MDNS_PORT);
	else
		send_response(ans, add, 0, NULL, 0, 0, mcast_be(), MDNS_PORT);
}

static char *txt_add(char *d, const char *end, const char *k, const char *v)
{
	char *lenp = d++;
	if (d >= end)
		return lenp;
	d = api_strcpy_end(d, end, k);
	d = api_strcpy_end(d, end, "=");
	d = api_strcpy_end(d, end, v);
	if (d - lenp - 1 > 255)
		d = lenp + 256;
	*lenp = (char)(uint8_t)(d - lenp - 1);
	return d;
}

void mdns_init(const api_config *cfg)
{
	char mac[13];
	char *d, *end;
	const char *name = cfg->name ? cfg->name : "esphome";

	end = host + sizeof(host) - 1;
	*api_strcpy_end(api_strcpy_end(host, end - 6, name), end, ".local") = 0;
	end = inst + sizeof(inst) - 1;
	d = api_strcpy_end(inst, end - (sizeof(SVC)), name);
	d = api_strcpy_end(d, end, ".");
	*api_strcpy_end(d, end, SVC) = 0;

	/* mac= is lowercase hex without separators, as ESPHome sends it */
	{
		uint8_t m[6];
		static const char hx[] = "0123456789abcdef";
		const char *s = cfg->mac_str;
		size_t n = 0;
		if (s && *s) {
			for (; *s && n < 12; s++)
				if (*s != ':' && *s != '-')
					mac[n++] = lower(*s);
		} else {
			netio_mac(m);
			for (int i = 0; i < 6; i++) {
				mac[n++] = hx[m[i] >> 4];
				mac[n++] = hx[m[i] & 15];
			}
		}
		mac[n] = 0;
	}
	d = txt;
	end = txt + sizeof(txt);
	if (cfg->friendly_name && cfg->friendly_name[0])
		d = txt_add(d, end, "friendly_name", cfg->friendly_name);
	d = txt_add(d, end, "version", ESPHOME_COMPAT_VERSION);
	d = txt_add(d, end, "mac", mac);
	d = txt_add(d, end, "platform", API_PLATFORM);
	d = txt_add(d, end, "board", API_BOARD);
	d = txt_add(d, end, "network", "wifi");
	{
		/* as ESPHome with a key: HA asks the user for the key */
		uint8_t k[32];
		if (cfg->encryption_key && cfg->encryption_key[0] &&
		    noise_psk_from_base64(cfg->encryption_key, k) == 0)
			d = txt_add(d, end, "api_encryption", NOISE_PROTOCOL_NAME);
		noise_wipe(k, sizeof(k));
	}
	d = txt_add(d, end, "project_name", API_PROJECT_NAME);
	d = txt_add(d, end, "project_version", cfg->version ? cfg->version : "");
	txt_len = (size_t)(d - txt);

	inited = true;
	cur_ip = 0;
	sock = -1;
	rebind_ms = hal_millis() - REBIND_MS;
	mdns_poll();                    /* binds + announces if we have an IP */
}

void mdns_poll(void)
{
	uint32_t now = hal_millis();
	uint32_t ip;
	if (!inited)
		return;

	ip = netio_local_ip_be();
	if (ip != cur_ip) {
		/* New/changed address: rebind (multicast membership is per
		 * interface address) and announce. */
		cur_ip = ip;
		if (sock >= 0) {
			netio_close(sock);
			sock = -1;
		}
		rebind_ms = now - REBIND_MS;
		announce_left = ip ? ANNOUNCE_COUNT : 0;
		announce_ms = now - ANNOUNCE_GAP_MS;
	}
	if (!cur_ip)
		return;
	if (sock < 0) {
		if (now - rebind_ms < REBIND_MS)
			return;
		rebind_ms = now;
		sock = netio_udp_bind(MDNS_PORT, mcast_be());
		if (sock < 0)
			return;
	}
	if (announce_left && now - announce_ms >= ANNOUNCE_GAP_MS) {
		announce_ms = now;
		announce_left--;
		send_response(R_PTR | R_SRV | R_TXT | R_A, 0, 0, NULL, 0, 0, mcast_be(),
			      MDNS_PORT);
	}
	for (int i = 0; i < 4; i++) {
		uint32_t src;
		uint16_t port;
		int n = netio_udp_recvfrom(sock, rxb, sizeof(rxb), &src, &port);
		if (n == NETIO_AGAIN || n == 0)
			break;
		if (n < 0) {
			netio_close(sock);
			sock = -1;
			break;
		}
		handle_packet(rxb, (size_t)n, src, port);
	}
}
