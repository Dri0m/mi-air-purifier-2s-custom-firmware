/* api_server.c - ESPHome native API server, plaintext or Noise transport.
 *
 * Plaintext (api.proto header comment): 0x00, varint payload length,
 * varint message type, protobuf payload.
 * Noise (esphome api_frame_helper_noise.cpp): every frame is 0x01, be16
 * length, payload.  Handshake: client hello frame (contents only go into
 * the prologue), server hello [0x01 "chosen proto", name\0, mac\0], then
 * NNpsk0 message 1 / message 2 each prefixed by a 0x00 status byte; a
 * reject is [0x01, reason text].  Data: ChaCha20-Poly1305 of
 * be16 type, be16 length, protobuf payload.
 * Matches esphome 2026.10.2 (API 1.19) as spoken by aioesphomeapi 46.x /
 * current Home Assistant.
 *
 * Single-threaded: api_server_poll() and api_log*() must be called from the
 * same task (api_log() itself only writes the log ring, which is safe from
 * anywhere).  All memory is static; per client one RX and one TX buffer.
 * Backpressure: a request is only parsed when the TX buffer has room for
 * its response; entity lists and state pushes are produced incrementally
 * as TX space frees up, so a slow client never needs a bigger buffer.
 *
 * Logs (R3): every console line goes into the log ring (include/logring.h).
 * A SubscribeLogsRequest gives the client a read cursor at the oldest line
 * still in the ring (history replay: the boot log), then new lines follow
 * as they arrive.  Lines are sent only from TX space beyond the request
 * reserve, so logs never delay request handling; a client too slow to keep
 * up is lapped by the ring and gets "[log] N bytes dropped" instead. */
#include <stddef.h>
#include <string.h>
#include "api_internal.h"
#include "logring.h"
#include "noise.h"
#include "hal.h"
#include "netio.h"
#include "purifier.h"
#include "diag.h"

#ifndef API_MAX_CLIENTS
#define API_MAX_CLIENTS 3          /* HA + one spare (reconnect, CLI logs) */
#endif
#ifndef API_RX_BUF
#define API_RX_BUF 256             /* bigger requests are skipped, not fatal */
#endif
#ifndef API_TX_BUF
#define API_TX_BUF 768
#endif
#define API_SCRATCH 288            /* largest single encoded message */
/* worst-case reply framing: Noise header 3 + type/len 4 + MAC 16 */
#define API_RESP_RESERVE (API_SCRATCH + 24)
#define API_MAX_MSG 65535u
#define KEEPALIVE_MS 60000u        /* ESPHome: ping after 60 s of silence */
#define KEEPALIVE_DROP_MS 150000u  /* ESPHome: drop after 2.5 x keepalive */
#define HELLO_TIMEOUT_MS 60000u    /* ESPHome HANDSHAKE_TIMEOUT_MS (Noise + Hello) */
#define CLOSE_LINGER_MS 2000u
#define SENSOR_MIN_MS 1000u        /* sensor state pushes at most 1/s */
#define RSSI_PERIOD_MS 60000u      /* like ESPHome's wifi_signal default */
#define LISTEN_RETRY_MS 2000u
#define LOG_PARTIAL_MS 500u        /* send an unterminated tail after this idle */
#define LOG_LINES_PER_POLL 16      /* per client */
/* worst-case SubscribeLogsResponse frame: "[L] " + line, pb + Noise framing */
#define LOG_FRAME_MAX (LOGRING_LINE_MAX + 4 + 8 + 24)

enum { C_FREE = 0, C_NEW, C_SETUP, C_AUTH, C_CLOSING };
enum { NS_HELLO = 0, NS_HANDSHAKE, NS_DATA };   /* Noise transport state */
#define NOISE_MAX_HANDSHAKE 128    /* esphome noise::MAX_HANDSHAKE_SIZE */

typedef struct {
	netio_sock s;
	uint8_t state;
	uint8_t ping_sent;
	uint8_t subscribed;
	uint8_t log_level;
	uint8_t logs_on;           /* SubscribeLogsRequest seen */
	int8_t list_next;          /* -1 idle, 0..E_COUNT-1, E_COUNT = Done */
	uint16_t rx_len, tx_len;
	uint32_t rx_skip;          /* bytes of an oversized frame to discard */
	uint32_t dirty;            /* entity states still to send */
	uint32_t last_rx_ms, state_ms;
	uint32_t log_cur;          /* log ring read cursor (logs_on) */
	uint8_t noise_st;          /* NS_*, Noise mode only */
	union {
		uint8_t h[32];     /* NS_HANDSHAKE: hash after the prologue */
		struct {
			noise_cipher send, recv;
		} c;               /* NS_DATA */
	} nz;
	uint8_t rx[API_RX_BUF];
	uint8_t tx[API_TX_BUF];
} api_conn;

static api_conn conns[API_MAX_CLIENTS];
static uint8_t scratch[API_SCRATCH];
static api_config cfg;
static char mac_buf[18];
static netio_sock listener = -1;
static uint32_t listen_retry_ms, sensor_flush_ms, rssi_ms;
static uint32_t sensor_pending;
static int last_rssi;
static bool noise_on;              /* encryption key set: Noise only */
static bool api_disabled;          /* bad key / no entropy: serve nothing */
static uint8_t psk[32];
static uint32_t log_seen_head, log_seen_ms;

/* ---------- helpers ---------- */

char *api_strcpy_end(char *d, const char *end, const char *s)
{
	while (*s && d < end)
		*d++ = *s++;
	return d;
}

char *api_itoa_end(char *d, const char *end, long v)
{
	char tmp[12];
	int n = 0;
	unsigned long u = v < 0 ? 0ul - (unsigned long)v : (unsigned long)v;
	do {
		tmp[n++] = (char)('0' + u % 10);
		u /= 10;
	} while (u && n < 11);
	if (v < 0)
		tmp[n++] = '-';
	while (n && d < end)
		*d++ = tmp[--n];
	return d;
}

static void conn_close(api_conn *c)
{
	if (c->state != C_FREE)
		netio_close(c->s);
	c->state = C_FREE;
	c->s = -1;
	noise_wipe(&c->nz, sizeof(c->nz));
}

static size_t tx_free(const api_conn *c) { return API_TX_BUF - c->tx_len; }

/* Append a raw Noise frame (0x01, be16 len, a || b). */
static bool noise_frame(api_conn *c, const uint8_t *a, size_t alen, const uint8_t *b,
			size_t blen)
{
	uint8_t *o = c->tx + c->tx_len;
	size_t len = alen + blen;
	if (tx_free(c) < 3 + len)
		return false;
	o[0] = 0x01;
	o[1] = (uint8_t)(len >> 8);
	o[2] = (uint8_t)len;
	if (alen)
		memcpy(o + 3, a, alen);
	if (blen)
		memcpy(o + 3 + alen, b, blen);
	c->tx_len = (uint16_t)(c->tx_len + 3 + len);
	return true;
}

/* Append one message, encrypted in place in the TX buffer. */
static bool noise_append(api_conn *c, uint16_t type, const uint8_t *p, size_t len)
{
	uint8_t *o = c->tx + c->tx_len;
	size_t enc;
	if (tx_free(c) < 3 + 4 + len + NOISE_MAC_LEN)
		return false;
	o[3] = (uint8_t)(type >> 8);
	o[4] = (uint8_t)type;
	o[5] = (uint8_t)(len >> 8);
	o[6] = (uint8_t)len;
	if (len)
		memcpy(o + 7, p, len);
	enc = noise_encrypt(&c->nz.c.send, o + 3, 4 + len);
	o[0] = 0x01;
	o[1] = (uint8_t)(enc >> 8);
	o[2] = (uint8_t)enc;
	c->tx_len = (uint16_t)(c->tx_len + 3 + enc);
	return true;
}

/* Append one framed message; false (nothing written) if it doesn't fit. */
static bool frame_append(api_conn *c, uint16_t type, const uint8_t *p, size_t len)
{
	uint8_t hdr[1 + 5 + 5];
	size_t h = 0;
	if (noise_on)
		return c->noise_st == NS_DATA && noise_append(c, type, p, len);
	hdr[h++] = 0x00;
	h += pb_encode_varint(hdr + h, len);
	h += pb_encode_varint(hdr + h, type);
	if (tx_free(c) < h + len)
		return false;
	memcpy(c->tx + c->tx_len, hdr, h);
	if (len)
		memcpy(c->tx + c->tx_len + h, p, len);
	c->tx_len = (uint16_t)(c->tx_len + h + len);
	return true;
}

static bool send_w(api_conn *c, uint16_t type, const pb_writer *w)
{
	if (w->overflow)
		return true;            /* bug: message > scratch; drop, don't stall */
	return frame_append(c, type, w->buf, w->len);
}

static bool send_empty(api_conn *c, uint16_t type)
{
	return frame_append(c, type, NULL, 0);
}

static void flush(api_conn *c)
{
	while (c->tx_len) {
		int n = netio_send(c->s, c->tx, c->tx_len);
		if (n == NETIO_AGAIN || n == 0)
			return;
		if (n < 0) {
			conn_close(c);
			return;
		}
		memmove(c->tx, c->tx + n, c->tx_len - (size_t)n);
		c->tx_len = (uint16_t)(c->tx_len - n);
	}
}

/* ---------- logging to API clients ---------- */

static void log_to(api_conn *c, uint8_t level, const char *text)
{
	static const char lv[] = "?EWICDV";
	char line[120];
	char *end = line + sizeof(line), *d = line;
	pb_writer w;

	d = api_strcpy_end(d, end, "[");
	*d++ = lv[level < 7 ? level : 0];
	d = api_strcpy_end(d, end, "][api]: ");
	d = api_strcpy_end(d, end, text);
	pb_writer_init(&w, scratch, sizeof(scratch));
	pb_w_uint32(&w, 1, level);                       /* level (force) */
	pb_w_bytes(&w, 3, line, (size_t)(d - line));     /* message */
	send_w(c, MSG_SUBSCRIBE_LOGS_RESP, &w);          /* dropped if full */
}

/* API-layer lines go through the log ring like all console output, so
 * they are ordered with it and part of the replayed history.  Ring only:
 * they never went to the UART. */
void api_log(uint8_t level, const char *msg)
{
	static const char lv[] = "?EWICDV";
	char line[120];
	char *end = line + sizeof(line) - 1, *d = line;
	d = api_strcpy_end(d, end, "[");
	*d++ = lv[level < 7 ? level : 0];
	d = api_strcpy_end(d, end, "][api]: ");
	d = api_strcpy_end(d, end, msg);
	*d++ = '\n';
	logring_write(line, (size_t)(d - line));
}

/* One ring line as a SubscribeLogsResponse.  Lines without an ESPHome
 * "[L]" level prefix get one, e.g. "[W][net] wlan: ...". */
static bool log_line_to(api_conn *c, uint8_t level, const char *text)
{
	static const char lv[] = "?EWICDV";
	char msg[LOGRING_LINE_MAX + 4];
	char *end = msg + sizeof(msg), *d = msg;
	pb_writer w;

	if (!(text[0] == '[' && text[1] && text[2] == ']')) {
		*d++ = '[';
		*d++ = lv[level < 7 ? level : 0];
		*d++ = ']';
		if (text[0] != '[')
			*d++ = ' ';
	}
	d = api_strcpy_end(d, end, text);
	pb_writer_init(&w, scratch, sizeof(scratch));
	pb_w_uint32(&w, 1, level);
	pb_w_bytes(&w, 3, msg, (size_t)(d - msg));
	return send_w(c, MSG_SUBSCRIBE_LOGS_RESP, &w);
}

/* Stream new ring lines to a log subscriber, within its TX space beyond
 * the request reserve (never blocks, never grows a buffer). */
static void do_logs(api_conn *c, bool flush_partial)
{
	char line[LOGRING_LINE_MAX];
	if (c->state != C_AUTH || !c->logs_on)
		return;
	for (int n = 0; n < LOG_LINES_PER_POLL; n++) {
		uint32_t lost = 0;
		int r;
		uint8_t lvl;
		if (tx_free(c) < API_RESP_RESERVE + LOG_FRAME_MAX)
			return;
		r = logring_read_line(&c->log_cur, line, sizeof(line), flush_partial, &lost);
		if (r == 0)
			return;
		if (r == LOGRING_LOST) {
			char *d = line, *end = line + sizeof(line) - 1;
			d = api_strcpy_end(d, end, "[W][log]: ");
			d = api_itoa_end(d, end, (long)lost);
			d = api_strcpy_end(d, end, " bytes dropped");
			*d = 0;
			if (c->log_level >= API_LOG_WARN)
				log_line_to(c, API_LOG_WARN, line);
			continue;
		}
		lvl = logring_line_level(line);
		if (lvl <= c->log_level)
			log_line_to(c, lvl, line);
	}
}

void api_log_int(uint8_t level, const char *msg, long v)
{
	char buf[80];
	char *end = buf + sizeof(buf) - 1, *d = buf;
	d = api_strcpy_end(d, end, msg);
	d = api_strcpy_end(d, end, ": ");
	d = api_itoa_end(d, end, v);
	*d = 0;
	api_log(level, buf);
}

/* ---------- message handlers ---------- */

static void mark_dirty_all(uint32_t bits)
{
	for (unsigned i = 0; i < API_MAX_CLIENTS; i++)
		if (conns[i].state == C_AUTH && conns[i].subscribed)
			conns[i].dirty |= bits;
}

/* Authenticated + subscribed sessions (HA); used by the OTA trial
 * fallback self-confirm (src/app/main.c, ota_trial_poll). App task only. */
int api_server_client_count(void)
{
	int n = 0;
	for (unsigned i = 0; i < API_MAX_CLIENTS; i++)
		n += conns[i].state == C_AUTH && conns[i].subscribed;
	return n;
}

/* HelloRequest fields (client_info, client API version) are not needed:
 * every client we serve speaks 1.x and we always answer 1.19. */
static void send_hello(api_conn *c)
{
	pb_writer w;

	pb_writer_init(&w, scratch, sizeof(scratch));
	pb_w_uint32(&w, 1, API_VERSION_MAJOR);
	pb_w_uint32(&w, 2, API_VERSION_MINOR);
	pb_w_string(&w, 3, ESPHOME_COMPAT_VERSION);       /* server_info */
	pb_w_string(&w, 4, cfg.name);
	send_w(c, MSG_HELLO_RESP, &w);
	if (c->state == C_NEW) {
		/* ESPHome >= 2026.1 authenticates at hello; with a legacy
		 * password we wait for AuthenticationRequest like 2025.x did. */
		c->state = (cfg.password && cfg.password[0]) ? C_SETUP : C_AUTH;
		c->state_ms = hal_millis();
	}
}

static bool str_eq_ct(const pb_field *f, const char *s)
{
	size_t n = strlen(s);
	uint8_t diff = (uint8_t)(f->len != n);
	for (size_t i = 0; i < f->len; i++)
		diff |= (uint8_t)(f->data[i] ^ (uint8_t)s[i < n ? i : 0]);
	return diff == 0;
}

static void handle_auth(api_conn *c, const uint8_t *d, size_t len)
{
	pb_reader r;
	pb_field f, pw = { 0 };
	pb_writer w;

	pb_reader_init(&r, d, len);
	while (pb_next(&r, &f) > 0)
		if (f.field == 1 && f.wt == PB_WT_LEN)
			pw = f;
	pb_writer_init(&w, scratch, sizeof(scratch));
	if (cfg.password && cfg.password[0] && !str_eq_ct(&pw, cfg.password)) {
		pb_w_bool(&w, 1, true);                   /* invalid_password */
		send_w(c, MSG_AUTH_RESP, &w);
		c->state = C_CLOSING;
		c->state_ms = hal_millis();
		return;
	}
	/* Valid (or no password configured): empty AuthenticationResponse, as
	 * ESPHome < 2026.1 sent; newer clients don't wait for it. */
	send_w(c, MSG_AUTH_RESP, &w);
	c->state = C_AUTH;
}

static void send_device_info(api_conn *c)
{
	pb_writer w;
	pb_writer_init(&w, scratch, sizeof(scratch));
	pb_w_bool(&w, 1, cfg.password && cfg.password[0]);   /* uses_password */
	pb_w_string(&w, 2, cfg.name);
	pb_w_string(&w, 3, cfg.mac_str);
	pb_w_string(&w, 4, ESPHOME_COMPAT_VERSION);          /* esphome_version */
	pb_w_string(&w, 5, fw_build_time);                  /* compilation_time */
	pb_w_string(&w, 6, API_MODEL);
	pb_w_string(&w, 8, API_PROJECT_NAME);                /* HA: manufacturer.model */
	pb_w_string(&w, 9, cfg.version);                     /* project_version */
	pb_w_string(&w, 12, API_MANUFACTURER);
	pb_w_string(&w, 13, cfg.friendly_name);
	pb_w_bool(&w, 19, noise_on);                        /* api_encryption_supported */
	send_w(c, MSG_DEVICE_INFO_RESP, &w);
}

static void dump_config(api_conn *c)
{
	log_to(c, API_LOG_CONFIG, "Mi Air Purifier 2S custom firmware (MW300)");
	log_to(c, API_LOG_CONFIG, cfg.version ? cfg.version : "");
	log_to(c, API_LOG_CONFIG, noise_on ?
	       "API on port 6053, protocol 1.19, Noise_NNpsk0_25519_ChaChaPoly_SHA256" :
	       "API on port 6053, protocol 1.19, plaintext");
}

static void handle_msg(api_conn *c, uint16_t type, const uint8_t *d, size_t len)
{
	/* Access rules as in esphome api_pb2_service.cpp read_message_(). */
	switch (type) {
	case MSG_HELLO_REQ:
	case MSG_DISCONNECT_REQ:
	case MSG_DISCONNECT_RESP:
	case MSG_PING_REQ:
	case MSG_PING_RESP:
		break;
	case MSG_DEVICE_INFO_REQ:
	case MSG_AUTH_REQ:
		if (c->state == C_NEW) {
			conn_close(c);  /* on_no_setup_connection */
			return;
		}
		break;
	default:
		if (c->state != C_AUTH) {
			conn_close(c);  /* not authenticated */
			return;
		}
		break;
	}

	switch (type) {
	case MSG_HELLO_REQ:
		send_hello(c);
		break;
	case MSG_AUTH_REQ:
		handle_auth(c, d, len);
		break;
	case MSG_DISCONNECT_REQ:
		send_empty(c, MSG_DISCONNECT_RESP);
		c->state = C_CLOSING;
		c->state_ms = hal_millis();
		break;
	case MSG_DISCONNECT_RESP:
		conn_close(c);
		break;
	case MSG_PING_REQ:
		send_empty(c, MSG_PING_RESP);
		break;
	case MSG_PING_RESP:
		break;              /* last_rx_ms already refreshed */
	case MSG_DEVICE_INFO_REQ:
		send_device_info(c);
		break;
	case MSG_DEVICE_CAPS_REQ:
		send_empty(c, MSG_DEVICE_CAPS_RESP);   /* no optional features */
		break;
	case MSG_LIST_ENTITIES_REQ:
		c->list_next = 0;
		break;
	case MSG_SUBSCRIBE_STATES_REQ:
		c->subscribed = 1;
		c->dirty |= api_entity_stateful_mask();
		break;
	case MSG_SUBSCRIBE_LOGS_REQ: {
		pb_reader r;
		pb_field f;
		bool dump = false;
		pb_reader_init(&r, d, len);
		while (pb_next(&r, &f) > 0) {
			if (f.field == 1)
				c->log_level = (uint8_t)(f.u64 > 7 ? 7 : f.u64);
			else if (f.field == 2)
				dump = f.u64 != 0;
		}
		if (dump)
			dump_config(c);
		if (!c->logs_on) {      /* first subscribe: replay the history */
			c->logs_on = 1;
			c->log_cur = logring_replay_cursor();
		}
		break;
	}
	case MSG_SUBSCRIBE_HA_SERVICES_REQ:
	case MSG_SUBSCRIBE_HA_STATES_REQ:
		break;              /* accepted: we never call HA services/states */
	case MSG_FAN_CMD:
	case MSG_SWITCH_CMD:
	case MSG_BUTTON_CMD:
	case MSG_NUMBER_CMD:
	case MSG_SELECT_CMD:
		mark_dirty_all(api_entity_command(type, d, len));
		break;
	default:
		break;              /* unknown: ignored, connection kept (ESPHome) */
	}
}

static void rx_consume(api_conn *c, size_t n)
{
	memmove(c->rx, c->rx + n, c->rx_len - n);
	c->rx_len = (uint16_t)(c->rx_len - n);
}

/* Noise handshake reject (esphome send_explicit_handshake_reject_): a frame
 * [0x01 status, reason], then the connection is closed. */
static int noise_reject(api_conn *c, const char *reason)
{
	static const uint8_t st = 0x01;
	noise_frame(c, &st, 1, (const uint8_t *)reason, strlen(reason));
	c->state = C_CLOSING;
	c->state_ms = hal_millis();
	c->rx_len = 0;
	return -1;
}

static void noise_server_hello(api_conn *c)
{
	/* chosen proto 0x01, name\0, mac\0 (lowercase hex, no separators,
	 * like esphome get_mac_address_into_buffer()) */
	uint8_t m[1 + 32 + 13];
	size_t n = 0, nl = strlen(cfg.name);
	if (nl > 31)
		nl = 31;
	m[n++] = 0x01;
	memcpy(m + n, cfg.name, nl);
	n += nl;
	m[n++] = 0;
	for (const char *s = cfg.mac_str; *s && n < sizeof(m) - 1; s++)
		if (*s != ':' && *s != '-')
			m[n++] = (uint8_t)((*s >= 'A' && *s <= 'F') ? *s + 32 : *s);
	m[n++] = 0;
	noise_frame(c, m, n, NULL, 0);
}

static int noise_handshake(api_conn *c, const uint8_t *p, size_t len)
{
	static const uint8_t ok = 0x00;
	uint8_t h[32], e[32], msg2[NOISE_MSG2_LEN];
	int rc;

	if (!len)
		return noise_reject(c, "Empty handshake message");
	if (p[0] != 0x00)
		return noise_reject(c, "Bad handshake error byte");
	memcpy(h, c->nz.h, 32);
	noise_rng_mix(p, len);          /* peer ephemeral: uniqueness only */
	if (noise_rng_bytes(e, sizeof(e)) != 0) {
		conn_close(c);          /* no entropy: never use a weak key */
		return -1;
	}
	rc = noise_respond(h, psk, p + 1, len - 1, e, msg2, &c->nz.c.send, &c->nz.c.recv);
	noise_wipe(h, sizeof(h));
	if (rc == NOISE_ERR_MAC)
		return noise_reject(c, "Handshake MAC failure");
	if (rc != NOISE_OK)
		return noise_reject(c, "Handshake error");
	noise_frame(c, &ok, 1, msg2, sizeof(msg2));
	c->noise_st = NS_DATA;
	return 1;
}

static int parse_noise(api_conn *c)
{
	size_t flen;
	uint8_t *p = c->rx + 3;
	int n;

	if (c->rx_len < 3)
		return 0;
	if (c->rx[0] != 0x01)
		return noise_reject(c, "Bad indicator byte");   /* e.g. plaintext client */
	flen = (size_t)c->rx[1] << 8 | c->rx[2];
	if (c->noise_st != NS_DATA && flen > NOISE_MAX_HANDSHAKE)
		return noise_reject(c, "Bad handshake packet len");
	if (3 + flen > API_RX_BUF) {
		/* Too big for us: skip it unread (it is dropped, so not having
		 * authenticated it is harmless) and keep the nonces in step. */
		c->rx_skip = (uint32_t)(3 + flen - c->rx_len);
		c->rx_len = 0;
		c->nz.c.recv.n++;
		api_log(API_LOG_WARN, "skipped oversized encrypted message");
		return 1;
	}
	if (c->rx_len < 3 + flen)
		return 0;
	switch (c->noise_st) {
	case NS_HELLO:
		/* contents unused, but they are part of the prologue */
		noise_prologue_hash(c->nz.h, p, flen);
		noise_server_hello(c);
		c->noise_st = NS_HANDSHAKE;
		break;
	case NS_HANDSHAKE:
		if (noise_handshake(c, p, flen) < 0)
			return -1;
		break;
	default:
		n = noise_decrypt(&c->nz.c.recv, p, flen);
		if (n < 4 || ((size_t)p[2] << 8 | p[3]) > (size_t)n - 4) {
			/* bad MAC / bad packet: fatal, like ESPHome.  Replies to the
			 * earlier, authentic messages go out first (best effort; ESPHome
			 * has already written them by then). */
			flush(c);
			if (c->state != C_FREE)
				conn_close(c);
			return -1;
		}
		handle_msg(c, (uint16_t)(p[0] << 8 | p[1]), p + 4, (size_t)p[2] << 8 | p[3]);
		if (c->state == C_FREE)
			return -1;
		break;
	}
	if (c->state == C_CLOSING)
		return -1;
	rx_consume(c, 3 + flen);
	return 1;
}

/* Parse one frame from the RX buffer.  1 = consumed, 0 = need more data,
 * -1 = connection closed / closing. */
static int parse_frame(api_conn *c)
{
	const uint8_t *p, *end = c->rx + c->rx_len;
	uint64_t len, type;
	size_t hdr;

	if (c->rx_skip) {
		size_t n = c->rx_skip < c->rx_len ? c->rx_skip : c->rx_len;
		rx_consume(c, n);
		c->rx_skip -= (uint32_t)n;
		return c->rx_skip ? 0 : 1;
	}
	if (noise_on)
		return parse_noise(c);
	if (!c->rx_len)
		return 0;
	if (c->rx[0] != 0x00) {
		/* Like ESPHome: tell e.g. a Noise client we only speak plaintext. */
		static const uint8_t bad[] = "\x00" "Bad indicator byte";
		c->tx_len = 0;
		memcpy(c->tx, bad, sizeof(bad) - 1);
		c->tx_len = sizeof(bad) - 1;
		flush(c);
		conn_close(c);
		return -1;
	}
	p = c->rx + 1;
	if (pb_read_varint(&p, end, &len) < 0 || pb_read_varint(&p, end, &type) < 0) {
		if (c->rx_len >= 1 + 5 + 5)
			goto bad;
		return 0;
	}
	if (len > API_MAX_MSG || type > 0xffff)
		goto bad;
	hdr = (size_t)(p - c->rx);
	if (hdr + len > API_RX_BUF) {
		/* Too big for us (e.g. a long HA state string): skip the frame
		 * but keep the connection. */
		c->rx_skip = (uint32_t)(hdr + len - c->rx_len);
		c->rx_len = 0;
		api_log_int(API_LOG_WARN, "skipped oversized message type", (long)type);
		return 1;
	}
	if (c->rx_len < hdr + len)
		return 0;
	handle_msg(c, (uint16_t)type, c->rx + hdr, (size_t)len);
	if (c->state == C_FREE)
		return -1;
	rx_consume(c, hdr + (size_t)len);
	return 1;
bad:
	conn_close(c);
	return -1;
}

static void do_rx(api_conn *c, uint32_t now)
{
	for (int iter = 0; iter < 8 && c->state != C_FREE && c->state != C_CLOSING; iter++) {
		bool progress = false;
		if (c->rx_len < API_RX_BUF) {
			int n = netio_recv(c->s, c->rx + c->rx_len, API_RX_BUF - c->rx_len);
			if (n == 0 || (n < 0 && n != NETIO_AGAIN)) {
				conn_close(c);
				return;
			}
			if (n > 0) {
				if (noise_on)
					noise_rng_event();      /* arrival timing */
				c->rx_len = (uint16_t)(c->rx_len + n);
				c->last_rx_ms = now;
				c->ping_sent = 0;
				progress = true;
			}
		}
		/* Only parse when the response is guaranteed to fit. */
		while (c->state != C_FREE && c->state != C_CLOSING &&
		       tx_free(c) >= API_RESP_RESERVE) {
			int r = parse_frame(c);
			if (r <= 0)
				break;
			progress = true;
		}
		if (c->state == C_FREE)
			return;
		if (!progress)
			break;
		if (tx_free(c) < API_RESP_RESERVE)
			flush(c);
	}
}

static void do_jobs(api_conn *c)
{
	pb_writer w;
	if (c->state != C_AUTH)
		return;
	while (c->list_next >= 0) {
		uint16_t t;
		if (c->list_next >= E_COUNT) {
			if (!send_empty(c, MSG_LIST_DONE_RESP))
				return;
			c->list_next = -1;
			break;
		}
		pb_writer_init(&w, scratch, sizeof(scratch));
		t = api_entity_encode_list((unsigned)c->list_next, &w);
		if (!send_w(c, t, &w))
			return;
		c->list_next++;
	}
	if (!c->subscribed)
		return;
	while (c->dirty) {
		unsigned i = 0;
		uint16_t t;
		while (!(c->dirty & (1u << i)))
			i++;
		pb_writer_init(&w, scratch, sizeof(scratch));
		t = api_entity_encode_state(i, &w);
		if (t && !send_w(c, t, &w))
			return;
		c->dirty &= ~(1u << i);
	}
}

static void do_timers(api_conn *c, uint32_t now)
{
	uint32_t idle = now - c->last_rx_ms;
	switch (c->state) {
	case C_NEW:
	case C_SETUP:
		if (now - c->state_ms > HELLO_TIMEOUT_MS)
			conn_close(c);
		break;
	case C_AUTH:
		if (idle > KEEPALIVE_DROP_MS) {
			conn_close(c);
		} else if (idle > KEEPALIVE_MS && !c->ping_sent) {
			if (send_empty(c, MSG_PING_REQ))
				c->ping_sent = 1;
		}
		break;
	case C_CLOSING:
		if (!c->tx_len || now - c->state_ms > CLOSE_LINGER_MS)
			conn_close(c);
		break;
	}
}

static void accept_clients(uint32_t now)
{
	netio_sock s;
	if (api_disabled)
		return;                 /* never fall back to plaintext */
	if (listener < 0) {
		if (now - listen_retry_ms < LISTEN_RETRY_MS)
			return;
		listen_retry_ms = now;
		listener = netio_tcp_listen(API_PORT);
		if (listener < 0)
			return;
	}
	while ((s = netio_tcp_accept(listener)) >= 0) {
		api_conn *c = NULL;
		for (unsigned i = 0; i < API_MAX_CLIENTS; i++)
			if (conns[i].state == C_FREE) {
				c = &conns[i];
				break;
			}
		if (!c) {
			netio_close(s); /* full: refuse */
			continue;
		}
		memset(c, 0, offsetof(api_conn, rx));
		if (noise_on)
			noise_rng_event();
		c->s = s;
		c->state = C_NEW;
		c->list_next = -1;
		c->last_rx_ms = c->state_ms = now;
	}
}

static void track_changes(uint32_t now)
{
	uint32_t bits = api_entity_dirty_from_pf(purifier_take_changes());
	uint32_t slow = api_entity_throttled_mask();

	if (now - rssi_ms >= RSSI_PERIOD_MS) {
		int r = netio_rssi();
		rssi_ms = now;
		if (r != last_rssi) {
			last_rssi = r;
			bits |= 1u << E_RSSI;
		}
	}
	mark_dirty_all(bits & ~slow);   /* fan/switches/door: immediately */
	sensor_pending |= bits & slow;
	if (sensor_pending && now - sensor_flush_ms >= SENSOR_MIN_MS) {
		mark_dirty_all(sensor_pending);
		sensor_pending = 0;
		sensor_flush_ms = now;
	}
}

/* ---------- public ---------- */

int api_server_init(const api_config *c)
{
	cfg = *c;
	if (!cfg.name)
		cfg.name = "esphome";
	if (!cfg.friendly_name)
		cfg.friendly_name = cfg.name;
	if (!cfg.version)
		cfg.version = "";
	if (!cfg.mac_str || !cfg.mac_str[0]) {
		static const char hx[] = "0123456789ABCDEF";
		uint8_t m[6];
		netio_mac(m);
		for (int i = 0; i < 6; i++) {
			mac_buf[i * 3] = hx[m[i] >> 4];
			mac_buf[i * 3 + 1] = hx[m[i] & 15];
			mac_buf[i * 3 + 2] = i < 5 ? ':' : 0;
		}
		cfg.mac_str = mac_buf;
	}
	for (unsigned i = 0; i < API_MAX_CLIENTS; i++) {
		conns[i].state = C_FREE;
		conns[i].s = -1;
	}
	if (listener >= 0)
		netio_close(listener);
	listener = -1;
	noise_on = false;
	api_disabled = true;            /* until the key (if any) is accepted */
	if (cfg.encryption_key && cfg.encryption_key[0]) {
		/* Never fall back to plaintext on a bad key: refuse to serve,
		 * now and on later listen retries. */
		if (noise_psk_from_base64(cfg.encryption_key, psk) != 0)
			return -2;
		/* the PSK (secret) and MAC (unique) also go into the RNG pool */
		uint8_t extra[32 + 18] = { 0 };
		memcpy(extra, psk, 32);
		memcpy(extra + 32, cfg.mac_str, strlen(cfg.mac_str) < 18 ? strlen(cfg.mac_str) : 18);
		int rc = noise_rng_init(extra, sizeof(extra));
		noise_wipe(extra, sizeof(extra));
		if (rc != 0)
			return -3;
		noise_on = true;
	}
	api_disabled = false;
	api_entities_init();
	listen_retry_ms = hal_millis();
	rssi_ms = listen_retry_ms - RSSI_PERIOD_MS;
	listener = netio_tcp_listen(API_PORT);
	return listener < 0 ? -1 : 0;
}

void api_server_poll(void)
{
	uint32_t now = hal_millis();
	uint32_t lh = logring_head();
	bool log_partial;
	if (lh != log_seen_head) {
		log_seen_head = lh;
		log_seen_ms = now;
	}
	log_partial = now - log_seen_ms >= LOG_PARTIAL_MS;
	accept_clients(now);
	track_changes(now);
	for (unsigned i = 0; i < API_MAX_CLIENTS; i++) {
		api_conn *c = &conns[i];
		if (c->state == C_FREE)
			continue;
		flush(c);
		if (c->state == C_FREE)
			continue;
		do_rx(c, now);
		if (c->state == C_FREE)
			continue;
		do_jobs(c);
		do_logs(c, log_partial);
		flush(c);
		if (c->state == C_FREE)
			continue;
		do_timers(c, now);
	}
}
