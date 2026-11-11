/* ota_server.c - minimal non-blocking HTTP/1.1 OTA receiver over netio.h.
 *
 *   POST /update     headers: Content-Length (required), X-OTA-Password
 *                    (required), X-OTA-MD5 (optional, 32 hex), Expect:
 *                    100-continue (optional).  Body: raw MRVL image.
 *                    -> 200 {"ok":true,...} then reboot after ~1 s.
 *   POST /confirm    header X-OTA-Password (required, same lockout as
 *                    /update), no body.  Confirms a trial image:
 *                    -> 200 {"ok":true,"confirmed":true,...}
 *                    -> 200 {"ok":true,"confirmed":false,"trial":false}
 *                       (not a trial: nothing done)
 *                    -> 409 {"ok":false,"error":"too_early","retry_in_ms":N}
 *   GET  /version    -> 200 {"running":"A",...} (no password)
 *
 * One client at a time (others get 503).  RAM: one 4 KiB buffer shared by
 * the request headers and the streamed body, plus a small reply buffer.
 * Chunked transfer encoding is not supported (411). */
#include <stdio.h>
#include <string.h>
#include "netio.h"
#include "hal.h"
#include "ota_internal.h"
#include "diag.h"                /* fw_build_time */

#ifndef OTA_HTTP_BUF
#define OTA_HTTP_BUF 4096u
#endif
/* Device: FW_VERSION = `git describe` from build/gen/fw_version.h
 * (force-included by the Makefile), fw_build_time = the link time from
 * build/gen/build_info.c (strong definition).  Host tests: these defaults. */
#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif
__attribute__((weak)) const char fw_build_time[] = "unknown";
#define HDR_MAX          2048u
#define IO_TIMEOUT_MS    15000u
#define LINGER_MS        1500u
#define DRAIN_MS         5000u
#define REBOOT_DELAY_MS  1000u
#define LOCKOUT_MS       3000u
#define RELISTEN_MS      5000u
#define BODY_READS_PER_POLL 8
#define PW_MAX           64u

enum { P_NONE, P_HDR, P_BODY, P_TX, P_DRAIN };
enum { AFTER_DRAIN, AFTER_BODY };

static uint8_t buf[OTA_HTTP_BUF];

static struct {
    uint8_t inited, pw_len, pw_set, has_md5, after, reboot_armed, locked, body_ota;
    netio_sock ls, cs;
    uint16_t port;
    char pw[PW_MAX + 1];
    int phase;
    size_t hlen;
    uint32_t clen, got;
    uint8_t md5[16];
    uint32_t last_io, drain_until, reboot_at, lock_until, relisten_at;
    char tx[800];
    size_t tx_len, tx_off;
} sv;

void ota_server_reset_state(void)
{
    memset(&sv, 0, sizeof sv);
    sv.ls = sv.cs = -1;
}

static int after_ms(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

/* ------------------------------------------------------------ helpers */

static int ieq(const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    if (alen != blen)
        return 0;
    for (size_t i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y)
            return 0;
    }
    return 1;
}

/* Constant-time for a given configured password length. */
static int pw_equal(const char *v, size_t vlen)
{
    unsigned diff = (unsigned)(vlen ^ sv.pw_len);
    for (size_t i = 0; i < PW_MAX; i++) {
        unsigned char a = i < vlen ? (unsigned char)v[i] : 0;
        unsigned char b = i < sv.pw_len ? (unsigned char)sv.pw[i] : 0;
        diff |= (unsigned)(a ^ b);
    }
    return vlen <= PW_MAX && diff == 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_md5(const char *v, size_t len, uint8_t out[16])
{
    if (len != 32)
        return -1;
    for (int i = 0; i < 16; i++) {
        int h = hexval(v[2 * i]), l = hexval(v[2 * i + 1]);
        if (h < 0 || l < 0)
            return -1;
        out[i] = (uint8_t)(h << 4 | l);
    }
    return 0;
}

static int parse_u32(const char *v, size_t len, uint32_t *out)
{
    uint32_t x = 0;
    if (len == 0 || len > 10)
        return -1;
    for (size_t i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9')
            return -1;
        uint32_t d = (uint32_t)(v[i] - '0');
        if (x > (0xFFFFFFFFu - d) / 10)
            return -1;
        x = x * 10 + d;
    }
    *out = x;
    return 0;
}

static const char *reason(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

static int status_for(int err)
{
    switch (err) {
    case OTA_E_SIZE: return 413;
    case OTA_E_BUSY:
    case OTA_E_EARLY:
    case OTA_E_TRIAL: return 409;
    case OTA_E_LENGTH:
    case OTA_E_MD5:
    case OTA_E_IMAGE:
    case OTA_E_CRC: return 422;
    default: return 500;
    }
}

/* ------------------------------------------------------------ connection */

static void close_client(void)
{
    if (sv.body_ota)
        ota_abort();
    sv.body_ota = 0;
    if (sv.cs >= 0)
        netio_close(sv.cs);
    sv.cs = -1;
    sv.phase = P_NONE;
}

static void queue_raw(const char *s, size_t n, int after)
{
    if (n > sizeof sv.tx)
        n = sizeof sv.tx;
    memcpy(sv.tx, s, n);
    sv.tx_len = n;
    sv.tx_off = 0;
    sv.after = (uint8_t)after;
    sv.phase = P_TX;
    sv.last_io = hal_millis();
}

/* Queue a JSON reply; afterwards linger/drain, then close. */
static void respond(int code, const char *json)
{
    char *t = sv.tx;
    int n = snprintf(t, sizeof sv.tx,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                     "Content-Length: %u\r\nConnection: close\r\n\r\n%s",
                     code, reason(code), (unsigned)strlen(json), json);
    if (n < 0 || (size_t)n >= sizeof sv.tx)
        n = snprintf(t, sizeof sv.tx, "HTTP/1.1 500 Internal Server Error\r\n"
                     "Content-Length: 0\r\nConnection: close\r\n\r\n");
    sv.tx_len = (size_t)n;
    sv.tx_off = 0;
    sv.after = AFTER_DRAIN;
    sv.phase = P_TX;
    sv.last_io = hal_millis();
    /* While the reply drains, keep reading (and discarding) any body the
     * client is still sending so the reply is not lost to a TCP reset. */
    sv.drain_until = sv.last_io + ((sv.got < sv.clen) ? DRAIN_MS : LINGER_MS);
    if (sv.body_ota) {
        ota_abort();
        sv.body_ota = 0;
    }
}

static void respond_err(int code, const char *err)
{
    char j[128];
    snprintf(j, sizeof j, "{\"ok\":false,\"error\":\"%s\"}", err);
    respond(code, j);
}

static void send_version(void)
{
    ota_info_t in;
    char j[560];
    int rc = ota_get_info(&in);
    /* confirm: "post" = this server has POST /confirm (old images: absent). */
    snprintf(j, sizeof j,
             "{\"running\":\"%c\",\"running_addr\":\"0x%06x\",\"next_boot\":\"%c\","
             "\"version\":\"%s\",\"build\":\"%s\",\"image_time\":%u,"
             "\"table_gen\":%u,\"gen_a\":%u,\"gen_b\":%u,\"trial\":%s,"
             "\"confirm_left_s\":%u,\"update_pending\":%s,\"in_progress\":%s,"
             "\"last_error\":\"%s\",\"table\":\"%s\",\"confirm\":\"post\","
             "\"uptime_s\":%u,\"trial_up_s\":%u,\"confirm_in_s\":%u,"
             "\"api_seen\":%s,\"self_confirm\":%s}",
             in.running, (unsigned)in.running_addr, in.next_boot, FW_VERSION,
             fw_build_time, (unsigned)in.image_time, (unsigned)in.table_gen,
             (unsigned)in.gen[0], (unsigned)in.gen[1], in.trial ? "true" : "false",
             (unsigned)(in.confirm_left_ms / 1000u), in.update_pending ? "true" : "false",
             in.in_progress ? "true" : "false",
             in.last_error ? ota_strerror(in.last_error) : "", rc ? ota_strerror(rc) : "ok",
             (unsigned)(hal_millis() / 1000u), (unsigned)(in.trial_up_ms / 1000u),
             (unsigned)((in.confirm_wait_ms + 999u) / 1000u), in.api_seen ? "true" : "false",
             in.api_hook ? "true" : "false");
    respond(200, j);
}

static void finish_upload(void)
{
    sv.body_ota = 0;
    int rc = ota_end_md5(sv.has_md5 ? sv.md5 : NULL);
    if (rc != OTA_OK) {
        respond_err(status_for(rc), ota_strerror(rc));
        return;
    }
    ota_info_t in;
    char j[200];
    (void)ota_get_info(&in);
    snprintf(j, sizeof j, "{\"ok\":true,\"next_boot\":\"%c\",\"bytes\":%u,"
             "\"reboot_in_ms\":%u,\"needs_confirm\":%s}", in.next_boot,
             (unsigned)sv.clen, (unsigned)REBOOT_DELAY_MS, OTA_BOOT_CONFIRM ? "true" : "false");
    respond(200, j);
    sv.reboot_armed = 1;        /* fires REBOOT_DELAY_MS after the close */
    sv.reboot_at = 0;
}

static void handle_confirm(void)
{
    char j[160];
    int rc = ota_confirm_request();
    if (rc == 1) {
        respond(200, "{\"ok\":true,\"confirmed\":false,\"trial\":false}");
        return;
    }
    if (rc == OTA_E_EARLY) {
        ota_info_t in;
        (void)ota_get_info(&in);
        snprintf(j, sizeof j, "{\"ok\":false,\"error\":\"too_early\",\"retry_in_ms\":%u}",
                 (unsigned)in.confirm_wait_ms);
        respond(409, j);
        return;
    }
    if (rc != OTA_OK) {
        respond_err(status_for(rc), ota_strerror(rc));
        return;
    }
    ota_info_t in;
    (void)ota_get_info(&in);
    snprintf(j, sizeof j, "{\"ok\":true,\"confirmed\":true,\"running\":\"%c\","
             "\"next_boot\":\"%c\"}", in.running, in.next_boot);
    respond(200, j);
}

/* Password check shared by /update and /confirm.  Returns 1 if the request
 * may proceed; otherwise a reply is queued. */
static int auth_ok(const char *pw, size_t pwl)
{
    uint32_t now = hal_millis();
    if (!sv.pw_set) { respond_err(403, "ota_password_not_configured"); return 0; }
    if (sv.locked && !after_ms(now, sv.lock_until)) { respond_err(429, "locked_out"); return 0; }
    sv.locked = 0;
    if (!pw || !pw_equal(pw, pwl)) {
        sv.locked = 1;
        sv.lock_until = now + LOCKOUT_MS;
        OTA_LOG("http: bad password\r\n");
        respond_err(401, ota_strerror(OTA_E_AUTH));
        return 0;
    }
    return 1;
}

static void feed_body(const uint8_t *p, uint32_t n)
{
    int rc = ota_write(p, n);
    sv.got += n;
    if (rc != OTA_OK) {
        respond_err(status_for(rc), ota_strerror(rc));
        return;
    }
    if (sv.got == sv.clen)
        finish_upload();
}

static void handle_request(size_t hdr_end)
{
    const char *h = (const char *)buf;
    const char *end = h + hdr_end - 2;          /* points at final CRLF */
    const char *line_end = h;
    while (line_end < end && line_end[0] != '\r')
        line_end++;

    /* request line: METHOD SP PATH SP VERSION */
    const char *sp1 = memchr(h, ' ', (size_t)(line_end - h));
    const char *sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(line_end - sp1 - 1)) : NULL;
    if (!sp1 || !sp2) {
        respond_err(400, "bad_request_line");
        return;
    }
    size_t mlen = (size_t)(sp1 - h);
    const char *path = sp1 + 1;
    size_t plen = (size_t)(sp2 - path);
    for (size_t i = 0; i < plen; i++)
        if (path[i] == '?') { plen = i; break; }

    const char *pw = NULL, *md5 = NULL, *cl = NULL;
    size_t pwl = 0, md5l = 0, cll = 0;
    int chunked = 0, expect100 = 0;
    const char *p = line_end + 2;
    while (p < end) {
        const char *le = p;
        while (le < end && le[0] != '\r')
            le++;
        const char *colon = memchr(p, ':', (size_t)(le - p));
        if (colon) {
            const char *v = colon + 1, *ve = le;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            size_t nl = (size_t)(colon - p), vl = (size_t)(ve - v);
            if (ieq(p, nl, "content-length")) { cl = v; cll = vl; }
            else if (ieq(p, nl, "x-ota-password")) { pw = v; pwl = vl; }
            else if (ieq(p, nl, "x-ota-md5")) { md5 = v; md5l = vl; }
            else if (ieq(p, nl, "transfer-encoding")) chunked = !ieq(v, vl, "identity");
            else if (ieq(p, nl, "expect")) expect100 = ieq(v, vl, "100-continue");
        }
        p = le + 2;
    }

    size_t extra = sv.hlen - hdr_end;           /* body bytes already read */
    sv.clen = 0;
    sv.got = 0;
    if (cl && parse_u32(cl, cll, &sv.clen) != 0) {
        respond_err(400, "bad_content_length");
        return;
    }

    if (ieq(path, plen, "/version") || ieq(path, plen, "/")) {
        if (!ieq(h, mlen, "GET")) { respond_err(405, "method_not_allowed"); return; }
        sv.got = sv.clen;
        send_version();
        return;
    }
    if (ieq(path, plen, "/confirm")) {
        if (!ieq(h, mlen, "POST")) { respond_err(405, "method_not_allowed"); return; }
        if (!auth_ok(pw, pwl))
            return;
        if (chunked || sv.clen > 0) { respond_err(400, "no_body_expected"); return; }
        handle_confirm();
        return;
    }
    if (!ieq(path, plen, "/update")) {
        respond_err(404, "not_found");
        return;
    }
    if (!ieq(h, mlen, "POST")) { respond_err(405, "method_not_allowed"); return; }
    if (!auth_ok(pw, pwl))
        return;
    if (chunked) { respond_err(411, "chunked_not_supported"); return; }
    if (!cl) { respond_err(411, "content_length_required"); return; }
    sv.has_md5 = 0;
    if (md5) {
        if (parse_md5(md5, md5l, sv.md5) != 0) { respond_err(400, "bad_md5_header"); return; }
        sv.has_md5 = 1;
    }
    int rc = ota_begin(sv.clen);
    if (rc != OTA_OK) {
        respond_err(status_for(rc), ota_strerror(rc));
        return;
    }
    sv.body_ota = 1;
    sv.phase = P_BODY;
    if (extra) {
        uint32_t n = extra > sv.clen ? sv.clen : (uint32_t)extra;
        feed_body(buf + hdr_end, n);
        if (sv.phase != P_BODY)
            return;
    }
    if (expect100 && sv.got == 0) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        queue_raw(cont, sizeof cont - 1, AFTER_BODY);
    }
}

/* ------------------------------------------------------------ phases */

static void do_headers(uint32_t now)
{
    int r = netio_recv(sv.cs, buf + sv.hlen, HDR_MAX - sv.hlen);
    if (r == NETIO_AGAIN) {
        if (after_ms(now, sv.last_io + IO_TIMEOUT_MS))
            close_client();
        return;
    }
    if (r <= 0) {
        close_client();
        return;
    }
    sv.last_io = now;
    size_t from = sv.hlen > 3 ? sv.hlen - 3 : 0;
    sv.hlen += (size_t)r;
    for (size_t i = from; i + 3 < sv.hlen; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            handle_request(i + 4);
            return;
        }
    }
    if (sv.hlen >= HDR_MAX)
        respond_err(431, "headers_too_large");
}

static void do_body(uint32_t now)
{
    for (int k = 0; k < BODY_READS_PER_POLL && sv.phase == P_BODY; k++) {
        uint32_t want = sv.clen - sv.got;
        if (want > sizeof buf)
            want = sizeof buf;
        int r = netio_recv(sv.cs, buf, want);
        if (r == NETIO_AGAIN) {
            if (after_ms(now, sv.last_io + IO_TIMEOUT_MS)) {
                OTA_LOG("http: body timeout at %u/%u\r\n", (unsigned)sv.got, (unsigned)sv.clen);
                respond_err(408, "timeout");
            }
            return;
        }
        if (r <= 0) {
            OTA_LOG("http: client gone at %u/%u\r\n", (unsigned)sv.got, (unsigned)sv.clen);
            close_client();             /* truncated upload: aborted, no switch */
            return;
        }
        sv.last_io = now;
        feed_body(buf, (uint32_t)r);
    }
}

static void do_tx(uint32_t now)
{
    while (sv.tx_off < sv.tx_len) {
        int r = netio_send(sv.cs, sv.tx + sv.tx_off, sv.tx_len - sv.tx_off);
        if (r == NETIO_AGAIN || r == 0) {
            if (after_ms(now, sv.last_io + IO_TIMEOUT_MS))
                close_client();
            return;
        }
        if (r < 0) {
            close_client();
            return;
        }
        sv.tx_off += (size_t)r;
        sv.last_io = now;
    }
    if (sv.after == AFTER_BODY) {
        sv.phase = P_BODY;
    } else {
        sv.phase = P_DRAIN;
        if ((int32_t)(sv.drain_until - now) < 0)
            sv.drain_until = now;
    }
}

static void do_drain(uint32_t now)
{
    for (int k = 0; k < BODY_READS_PER_POLL; k++) {
        int r = netio_recv(sv.cs, buf, sizeof buf);
        if (r == NETIO_AGAIN)
            break;
        if (r <= 0) {
            close_client();
            return;
        }
    }
    if (after_ms(now, sv.drain_until))
        close_client();
}

/* ------------------------------------------------------------ API */

int ota_server_init(uint16_t port, const char *password)
{
    if (sv.inited) {
        close_client();
        if (sv.ls >= 0)
            netio_close(sv.ls);
    }
    ota_server_reset_state();
    size_t n = password ? strlen(password) : 0;
    if (n > PW_MAX)
        return OTA_E_AUTH;
    memcpy(sv.pw, password ? password : "", n);
    sv.pw_len = (uint8_t)n;
    sv.pw_set = n > 0;
    sv.port = port;
    sv.inited = 1;
    (void)ota_boot_check();             /* in case the app did not call it */
    sv.ls = netio_tcp_listen(port);
    if (sv.ls < 0) {
        sv.relisten_at = hal_millis() + RELISTEN_MS;
        return OTA_E_STATE;             /* keeps retrying from ota_server_poll() */
    }
    return OTA_OK;
}

/* 1 = initialised with a password and currently listening: the device can
 * be updated over the air right now (used to gate trial confirmation). */
int ota_server_ready(void)
{
    return sv.inited && sv.pw_set && sv.ls >= 0;
}

void ota_server_poll(void)
{
    if (!sv.inited)
        return;
    uint32_t now = hal_millis();
    ota_tick();

    if (sv.reboot_armed && sv.cs < 0) {
        if (sv.reboot_at == 0)
            sv.reboot_at = now + REBOOT_DELAY_MS;
        else if (after_ms(now, sv.reboot_at)) {
            OTA_LOG("rebooting into the new image\r\n");
            sv.reboot_armed = 0;
            hal_reboot();
            return;
        }
    }
    if (sv.ls < 0 && after_ms(now, sv.relisten_at)) {
        sv.ls = netio_tcp_listen(sv.port);
        if (sv.ls < 0)
            sv.relisten_at = now + RELISTEN_MS;
    }
    if (sv.cs >= 0) {
        switch (sv.phase) {
        case P_HDR: do_headers(now); break;
        case P_BODY: do_body(now); break;
        case P_TX: do_tx(now); break;
        case P_DRAIN: do_drain(now); break;
        default: close_client(); break;
        }
        /* A phase may have queued a reply: try to send it right away. */
        if (sv.cs >= 0 && sv.phase == P_TX)
            do_tx(now);
    }
    if (sv.ls >= 0) {
        netio_sock c = netio_tcp_accept(sv.ls);
        if (c >= 0 && sv.cs >= 0 && sv.phase == P_DRAIN && !sv.reboot_armed)
            close_client();             /* only lingering after its reply: preempt */
        if (c >= 0) {
            if (sv.cs >= 0 || sv.reboot_armed) {
                static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\n"
                    "Content-Type: application/json\r\nContent-Length: 27\r\n"
                    "Connection: close\r\n\r\n{\"ok\":false,\"error\":\"busy\"}";
                (void)netio_send(c, busy, sizeof busy - 1);
                netio_close(c);
            } else {
                sv.cs = c;
                sv.phase = P_HDR;
                sv.hlen = 0;
                sv.clen = sv.got = 0;
                sv.last_io = now;
            }
        } else if (c != NETIO_AGAIN) {
            netio_close(sv.ls);         /* listener broke (e.g. Wi-Fi down) */
            sv.ls = -1;
            sv.relisten_at = now + RELISTEN_MS;
        }
    }
}
