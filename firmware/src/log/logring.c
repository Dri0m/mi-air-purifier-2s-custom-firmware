/* logring.c - console log ring buffer + retained last-boot record.
 * See include/logring.h for the contract.
 *
 * Concurrency: every access to the ring state (and to the retained record)
 * happens inside lr_lock()/lr_unlock().  On the device that is PRIMASK
 * (cpsid i): safe from any task, any ISR, fault handlers and before the
 * scheduler starts, and it never blocks.  Writes are copied in chunks of at
 * most LR_CHUNK bytes per critical section, reads copy at most one line, so
 * interrupts are masked for a few hundred cycles at most. */
#include <string.h>
#include "logring.h"

#define MASK (LOGRING_SIZE - 1u)
#define RMASK (LOGRING_RETAIN_LOG - 1u)
#define LR_CHUNK 64u

#if (LOGRING_SIZE & MASK) || (LOGRING_RETAIN_LOG & RMASK) || (LOGRING_SIZE % LOGRING_RETAIN_LOG) || \
	(LOGRING_REASON_MAX % 4) || LOGRING_LINE_MAX < 16
#error "logring sizes"
#endif

static char ring[LOGRING_SIZE];
static uint32_t head;              /* absolute write position */
static uint8_t filled;             /* head has passed LOGRING_SIZE once */
static logring_retain *retain;     /* NULL until attached */

static char prev_log[LOGRING_RETAIN_LOG];
static unsigned prev_len;
static char prev_reason[LOGRING_REASON_MAX + 1];
static uint8_t prev_ok;

#if defined(__arm__)
typedef uint32_t lr_state;
static inline lr_state lr_lock(void)
{
	uint32_t p;
	__asm volatile ("mrs %0, primask\n\tcpsid i" : "=r"(p) : : "memory");
	return p;
}
static inline void lr_unlock(lr_state p)
{
	__asm volatile ("msr primask, %0" : : "r"(p) : "memory");
}
#else
typedef int lr_state;
static void (*host_lock)(void), (*host_unlock)(void);
static lr_state lr_lock(void)
{
	if (host_lock)
		host_lock();
	return 0;
}
static void lr_unlock(lr_state s)
{
	(void)s;
	if (host_unlock)
		host_unlock();
}
void logring_set_lock(void (*lock)(void), void (*unlock)(void))
{
	host_lock = lock;
	host_unlock = unlock;
}
void logring_reset(void)
{
	lr_state s = lr_lock();
	head = 0;
	filled = 0;
	retain = NULL;
	memset(ring, 0, sizeof(ring));
	lr_unlock(s);
}
#endif

static uint32_t crc32_bytes(const uint8_t *p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	}
	return ~c;
}

/* Mirror ring bytes [from, to) into the retained record (word stores),
 * only the last LOGRING_RETAIN_LOG of them.  Called locked. */
static void mirror(uint32_t from, uint32_t to)
{
	volatile uint32_t *log;
	if (!retain || from == to)
		return;
	if (to - from > LOGRING_RETAIN_LOG)
		from = to - LOGRING_RETAIN_LOG;
	log = retain->log;
	for (uint32_t p = from & ~3u; p != ((to + 3u) & ~3u); p += 4) {
		uint32_t w = (uint32_t)(uint8_t)ring[p & MASK] |
			     (uint32_t)(uint8_t)ring[(p + 1) & MASK] << 8 |
			     (uint32_t)(uint8_t)ring[(p + 2) & MASK] << 16 |
			     (uint32_t)(uint8_t)ring[(p + 3) & MASK] << 24;
		log[(p & RMASK) >> 2] = w;
	}
	((volatile logring_retain *)retain)->head = to;
}

void logring_write(const char *s, size_t n)
{
	while (n) {
		size_t k = n < LR_CHUNK ? n : LR_CHUNK;
		lr_state st = lr_lock();
		uint32_t start = head;
		for (size_t i = 0; i < k; i++) {
			char c = s[i];
			if (c == '\r' || c == 0)
				continue;
			ring[head & MASK] = c;
			head++;
			if (head == LOGRING_SIZE)
				filled = 1;
		}
		mirror(start, head);
		lr_unlock(st);
		s += k;
		n -= k;
	}
}

void logring_puts(const char *s)
{
	logring_write(s, strlen(s));
}

void logring_set_reason(const char *s)
{
	uint8_t b[LOGRING_REASON_MAX];
	size_t n = 0;
	lr_state st;
	memset(b, 0, sizeof(b));
	while (s[n] && s[n] != '\r' && s[n] != '\n' && n < sizeof(b)) {
		b[n] = (uint8_t)s[n];
		n++;
	}
	st = lr_lock();
	if (retain) {
		volatile logring_retain *r = retain;
		for (size_t i = 0; i < sizeof(b) / 4; i++)
			r->reason[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 |
				       (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
		r->reason_crc = crc32_bytes(b, n);
		r->reason_len = (uint32_t)n;
	}
	lr_unlock(st);
}

uint32_t logring_head(void)
{
	lr_state st = lr_lock();
	uint32_t h = head;
	lr_unlock(st);
	return h;
}

/* First line start at or after p (or p itself if no '\n' up to head).
 * Locked. */
static uint32_t next_line_start(uint32_t p)
{
	for (uint32_t q = p; q != head; q++)
		if (ring[q & MASK] == '\n')
			return q + 1;
	return p;
}

uint32_t logring_replay_cursor(void)
{
	lr_state st = lr_lock();
	uint32_t c = filled ? next_line_start(head - LOGRING_SIZE) : 0;
	lr_unlock(st);
	return c;
}

int logring_read_line(uint32_t *cur, char *buf, size_t cap, int flush_partial, uint32_t *lost)
{
	int ret = 0;
	lr_state st = lr_lock();
	uint32_t c = *cur;

	if (head - c > LOGRING_SIZE) {
		uint32_t p = next_line_start(head - LOGRING_SIZE);
		*lost = p - c;
		*cur = p;
		lr_unlock(st);
		return LOGRING_LOST;
	}
	for (;;) {
		uint32_t avail = head - c, lim = (uint32_t)cap - 1u, i = 0;
		if (!avail)
			break;
		if (lim > avail)
			lim = avail;
		while (i < lim && ring[(c + i) & MASK] != '\n')
			i++;
		if (i == 0 && lim) {
			c++;                    /* empty line: skip */
			continue;
		}
		if (i == lim && lim == avail && lim < (uint32_t)cap - 1u && !flush_partial)
			break;                  /* incomplete line: wait for more */
		/* a complete line, a piece of a long one, or a flushed tail */
		for (uint32_t k = 0; k < i; k++)
			buf[k] = ring[(c + k) & MASK];
		buf[i] = 0;
		c += i;
		if (c != head && ring[c & MASK] == '\n')
			c++;
		ret = (int)i;
		break;
	}
	*cur = c;
	lr_unlock(st);
	if (ret == 0 && cap)
		buf[0] = 0;
	return ret;
}

static int lower(int ch)
{
	return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
}

static int has(const char *s, const char *w)
{
	for (; *s; s++) {
		size_t i = 0;
		while (w[i] && lower((unsigned char)s[i]) == w[i])
			i++;
		if (!w[i])
			return 1;
	}
	return 0;
}

uint8_t logring_line_level(const char *s)
{
	static const char *const warn[] = {
		"fail", "error", "warning", "invalid", "refused", "timeout",
		"cannot", "unusable", "not found", "unavailable", "disabled",
		"dropped", "bad ", "reverting", "lost", NULL
	};
	if (s[0] == '[' && s[1] && s[2] == ']') {
		switch (s[1]) {
		case 'E': return LOGRING_LVL_ERROR;
		case 'W': return LOGRING_LVL_WARN;
		case 'I': return LOGRING_LVL_INFO;
		case 'C': return LOGRING_LVL_CONFIG;
		case 'D': return LOGRING_LVL_DEBUG;
		case 'V': return 6;
		default: break;
		}
	}
	if (has(s, "***"))
		return LOGRING_LVL_ERROR;
	for (unsigned i = 0; warn[i]; i++)
		if (has(s, warn[i]))
			return LOGRING_LVL_WARN;
	return LOGRING_LVL_INFO;
}

/* ---- retention ---- */

static char printable(uint8_t ch)
{
	return (ch == '\n' || (ch >= 0x20 && ch < 0x7f)) ? (char)ch : '?';
}

void logring_retain_attach(logring_retain *r)
{
	volatile logring_retain *v = r;
	lr_state st;

	prev_ok = 0;
	prev_len = 0;
	prev_reason[0] = 0;
	if (v->magic == LOGRING_RETAIN_MAGIC && v->magic_inv == ~LOGRING_RETAIN_MAGIC) {
		uint32_t h = v->head, n = h - v->log_base;
		unsigned skip = 0;
		uint32_t rl = v->reason_len;
		/* lapped: the word holding the oldest bytes was partly
		 * overwritten by the newest ones; skip that word */
		if (n > LOGRING_RETAIN_LOG)
			n = LOGRING_RETAIN_LOG - 4;
		for (uint32_t i = 0; i < n; i++) {
			uint32_t q = h - n + i;
			uint32_t w = v->log[(q & RMASK) >> 2];
			prev_log[i] = printable((uint8_t)(w >> (8 * (q & 3u))));
		}
		/* lapped: the first line is a fragment */
		if (h - v->log_base > LOGRING_RETAIN_LOG) {
			while (skip < n && prev_log[skip] != '\n')
				skip++;
			if (skip < n)
				skip++;
			memmove(prev_log, prev_log + skip, n - skip);
			n -= skip;
		}
		prev_len = n;
		if (rl && rl <= LOGRING_REASON_MAX) {
			uint8_t b[LOGRING_REASON_MAX];
			for (uint32_t i = 0; i < LOGRING_REASON_MAX / 4; i++) {
				uint32_t w = v->reason[i];
				b[4 * i] = (uint8_t)w;
				b[4 * i + 1] = (uint8_t)(w >> 8);
				b[4 * i + 2] = (uint8_t)(w >> 16);
				b[4 * i + 3] = (uint8_t)(w >> 24);
			}
			if (crc32_bytes(b, rl) == v->reason_crc) {
				for (uint32_t i = 0; i < rl; i++)
					prev_reason[i] = b[i] == '\n' ? ' ' : printable(b[i]);
				prev_reason[rl] = 0;
			}
		}
		prev_ok = 1;
	}
	st = lr_lock();
	v->magic = 0;                   /* invalid while re-arming */
	v->reason_len = 0;
	v->reason_crc = 0;
	v->log_base = head;
	v->head = head;
	v->magic_inv = ~LOGRING_RETAIN_MAGIC;
	v->magic = LOGRING_RETAIN_MAGIC;
	retain = r;
	lr_unlock(st);
}

int logring_prev_valid(void)
{
	return prev_ok;
}

const char *logring_prev_reason(void)
{
	return prev_reason;
}

int logring_prev_line(unsigned *pos, char *buf, size_t cap)
{
	unsigned p = *pos, i = 0;
	while (p < prev_len && prev_log[p] == '\n')
		p++;
	while (p < prev_len && prev_log[p] != '\n' && i < cap - 1)
		buf[i++] = prev_log[p++];
	buf[i] = 0;
	*pos = p;
	return (int)i;
}
