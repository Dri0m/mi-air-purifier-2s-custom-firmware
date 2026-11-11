/* test_logring.c - host tests of src/log/logring.c: line splitting, wrap,
 * replay cursor, reader lag / drop accounting, interleaved and threaded
 * writers, level classification and the retained last-boot record. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "logring.h"

static char L[LOGRING_LINE_MAX];

static int rd(uint32_t *cur, int partial)
{
	uint32_t lost = 0;
	int r = logring_read_line(cur, L, sizeof(L), partial, &lost);
	assert(r != LOGRING_LOST);
	return r;
}

static void test_lines(void)
{
	uint32_t c;
	logring_reset();
	c = logring_replay_cursor();
	assert(c == 0 && rd(&c, 0) == 0);
	logring_puts("hello\r\nworld\r\n\r\n\npart");
	assert(rd(&c, 0) == 5 && !strcmp(L, "hello"));
	assert(rd(&c, 0) == 5 && !strcmp(L, "world"));
	assert(rd(&c, 0) == 0);                 /* empty lines skipped, "part" pending */
	assert(rd(&c, 1) == 4 && !strcmp(L, "part"));   /* idle flush */
	assert(rd(&c, 1) == 0);
	logring_puts("ial\n");                  /* continuation: its own line */
	assert(rd(&c, 0) == 3 && !strcmp(L, "ial"));
	logring_write("a\0b\n", 4);             /* NUL dropped */
	assert(rd(&c, 0) == 2 && !strcmp(L, "ab"));
	printf("  ok  lines: CR/NUL stripped, empty lines skipped, partial tail only on idle flush\n");
}

static void test_long_line(void)
{
	char big[400];
	uint32_t c;
	char small[8];
	uint32_t lost;
	logring_reset();
	memset(big, 'x', sizeof(big));
	big[sizeof(big) - 1] = '\n';
	logring_write(big, sizeof(big));
	c = logring_replay_cursor();
	assert(rd(&c, 0) == (int)sizeof(L) - 1);
	assert(rd(&c, 0) == (int)sizeof(L) - 1);
	assert(rd(&c, 0) == 399 - 2 * ((int)sizeof(L) - 1));
	assert(rd(&c, 0) == 0 && c == logring_head());
	/* exactly cap-1 bytes then '\n': one piece, newline consumed */
	logring_puts("1234567\n89\n");
	assert(logring_read_line(&c, small, sizeof(small), 0, &lost) == 7 && !strcmp(small, "1234567"));
	assert(logring_read_line(&c, small, sizeof(small), 0, &lost) == 2 && !strcmp(small, "89"));
	printf("  ok  long lines split into cap-1 pieces, nothing lost\n");
}

static void test_wrap_and_replay(void)
{
	char b[64];
	uint32_t c;
	int n = 0, last = -1;
	logring_reset();
	for (int i = 0; i < 1000; i++) {    /* ~16 KB: wraps 4 times */
		snprintf(b, sizeof(b), "line %04d abcdefgh\n", i);
		logring_puts(b);
	}
	c = logring_replay_cursor();
	while (rd(&c, 0) > 0) {
		int v;
		assert(sscanf(L, "line %d abcdefgh", &v) == 1 && strlen(L) == 18);
		assert(last < 0 || v == last + 1);
		last = v;
		n++;
	}
	assert(last == 999);
	assert(n == LOGRING_SIZE / 19 || n == LOGRING_SIZE / 19 - 1);
	printf("  ok  wrap: replay starts at the oldest complete line (%d lines kept)\n", n);
}

static void test_lag_drop(void)
{
	char b[64];
	uint32_t c, lost = 0, h0;
	int r, first;
	logring_reset();
	logring_puts("old line\n");
	c = logring_replay_cursor();             /* a slow reader at 0 */
	h0 = logring_head();
	for (int i = 0; i < 700; i++) {
		snprintf(b, sizeof(b), "lag %04d 0123456789\n", i);
		logring_puts(b);
	}
	r = logring_read_line(&c, L, sizeof(L), 0, &lost);
	assert(r == LOGRING_LOST);
	/* everything before the cursor's new position is accounted as lost */
	assert(lost == c && c > logring_head() - LOGRING_SIZE);
	assert(rd(&c, 0) > 0 && sscanf(L, "lag %d", &first) == 1);
	/* bytes read after the drop + lost == everything written */
	{
		uint32_t got = (uint32_t)strlen(L) + 1;
		while ((r = rd(&c, 0)) > 0)
			got += (uint32_t)r + 1;
		assert(lost + got == logring_head());
	}
	(void)h0;
	printf("  ok  lapped reader: LOST with exact byte count, resumes at a line start (line %d)\n", first);
}

/* Interleaved writers (ISR between task fragments): every whole write that
 * fits one chunk (<= 64 B) is contiguous. */
static void test_interleaved(void)
{
	char b[64];
	uint32_t c;
	int na = 0, nb = 0;
	logring_reset();
	c = logring_replay_cursor();
	for (int i = 0; i < 50; i++) {
		snprintf(b, sizeof(b), "[task] t%d payload payload payload\n", i);
		logring_puts(b);
		snprintf(b, sizeof(b), "[isr] i%d\n", i);
		logring_puts(b);
		while (rd(&c, 0) > 0) {
			int v;
			if (sscanf(L, "[task] t%d payload payload payload", &v) == 1)
				assert(v == na++);
			else if (sscanf(L, "[isr] i%d", &v) == 1)
				assert(v == nb++);
			else
				assert(0);
		}
	}
	assert(na == 50 && nb == 50);
	printf("  ok  interleaved writers: each write intact and in order\n");
}

/* Real threads: 4 writers + 1 reader, the host lock hooks around every
 * ring access.  Every line read must be intact, per writer in order;
 * lines missing for a writer must be covered by reported loss. */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void lk(void) { pthread_mutex_lock(&mu); }
static void ulk(void) { pthread_mutex_unlock(&mu); }
#define NW 4
#define NL 20000
static int writers_done;           /* under mu */

static void *writer(void *arg)
{
	int id = (int)(long)arg;
	char b[64];
	for (int i = 0; i < NL; i++) {
		int n = snprintf(b, sizeof(b), "w%d %06d %.*s\n", id, i, (i % 23) + 1,
				 "abcdefghijklmnopqrstuvwxyz");
		logring_write(b, (size_t)n);
	}
	lk();
	writers_done++;
	ulk();
	return NULL;
}

static void test_threads(void)
{
	pthread_t t[NW];
	uint32_t c, lost_total = 0, lost;
	int next[NW] = { 0 }, got = 0, gaps = 0, done;
	logring_reset();
	logring_set_lock(lk, ulk);
	c = logring_replay_cursor();
	for (long i = 0; i < NW; i++)
		assert(pthread_create(&t[i], NULL, writer, (void *)i) == 0);
	for (;;) {
		int id, seq, len;
		char tail[32];
		lk();
		done = writers_done;    /* read before the ring: then empty = finished */
		ulk();
		int r = logring_read_line(&c, L, sizeof(L), 0, &lost);
		if (r == LOGRING_LOST) {
			lost_total += lost;
			for (int i = 0; i < NW; i++)
				next[i] = -1;   /* any line may have been skipped */
			continue;
		}
		if (r == 0) {
			if (done == NW)
				break;
			continue;
		}
		assert(sscanf(L, "w%d %d %31s", &id, &seq, tail) == 3);
		assert(id >= 0 && id < NW);
		len = (seq % 23) + 1;
		assert((int)strlen(tail) == len && !strncmp(tail, "abcdefghijklmnopqrstuvwxyz", (size_t)len));
		if (next[id] >= 0) {
			gaps += seq != next[id];
			assert(seq >= next[id]);
		}
		next[id] = seq + 1;
		got++;
	}
	for (int i = 0; i < NW; i++)
		pthread_join(t[i], NULL);
	logring_set_lock(NULL, NULL);
	assert(gaps == 0);   /* without a reported loss, no line goes missing */
	if (!lost_total)
		assert(got == NW * NL);
	printf("  ok  %d writer threads x %d lines + reader: %d lines intact, %u bytes reported lost\n",
	       NW, NL, got, (unsigned)lost_total);
}

static void test_levels(void)
{
	assert(logring_line_level("[E][api]: x") == LOGRING_LVL_ERROR);
	assert(logring_line_level("[W][api]: x") == LOGRING_LVL_WARN);
	assert(logring_line_level("[C][api]: x") == LOGRING_LVL_CONFIG);
	assert(logring_line_level("[D][x]: fail") == LOGRING_LVL_DEBUG);
	assert(logring_line_level("*** ASSERT tasks.c:12") == LOGRING_LVL_ERROR);
	assert(logring_line_level("[net] wlan_connect FAILED -5") == LOGRING_LVL_WARN);
	assert(logring_line_level("[ota] http: bad password") == LOGRING_LVL_WARN);
	assert(logring_line_level("[app] up 60s, heap free 1234") == LOGRING_LVL_INFO);
	assert(logring_line_level("[net] IP up: 1.2.3.4") == LOGRING_LVL_INFO);
	printf("  ok  levels: [L] prefix, *** = error, failure words = warning, else info\n");
}

/* ---- retention: the area survives a simulated reset ---- */
static logring_retain area;

static void reboot(void)
{
	logring_reset();                /* RAM ring lost */
	logring_retain_attach(&area);   /* AON RAM kept */
}

static void test_retention(void)
{
	char b[64];
	unsigned pos = 0;
	int n = 0;

	/* power-on: garbage */
	memset(&area, 0xA5, sizeof(area));
	reboot();
	assert(!logring_prev_valid() && !logring_prev_reason()[0]);

	/* short log + reason */
	logring_puts("[boot] one\r\n[app] two\r\n");
	logring_puts("\r\n*** ASSERT tasks.c:42 at 17s\r\n");
	logring_set_reason("ASSERT tasks.c:42 at 17s\r\n");
	reboot();
	assert(logring_prev_valid());
	assert(!strcmp(logring_prev_reason(), "ASSERT tasks.c:42 at 17s"));
	assert(logring_prev_line(&pos, L, sizeof(L)) && !strcmp(L, "[boot] one"));
	assert(logring_prev_line(&pos, L, sizeof(L)) && !strcmp(L, "[app] two"));
	assert(logring_prev_line(&pos, L, sizeof(L)) && !strcmp(L, "*** ASSERT tasks.c:42 at 17s"));
	assert(logring_prev_line(&pos, L, sizeof(L)) == 0);
	printf("  ok  retention: previous boot's lines + reason after a reset\n");

	/* nothing written since: valid, empty, no reason */
	reboot();
	pos = 0;
	assert(logring_prev_valid() && !logring_prev_reason()[0]);
	assert(logring_prev_line(&pos, L, sizeof(L)) == 0);

	/* lapped mirror: last 512 bytes, first fragment dropped, lines intact */
	for (int i = 0; i < 300; i++) {
		snprintf(b, sizeof(b), "r%03d xyz\n", i);
		logring_puts(b);
	}
	logring_set_reason("first");
	logring_set_reason("hardfault pc 1f0012ab");     /* last one wins */
	reboot();
	pos = 0;
	{
		int v, last = -1;
		while (logring_prev_line(&pos, L, sizeof(L)) > 0) {
			assert(sscanf(L, "r%d xyz", &v) == 1 && strlen(L) == 8);
			assert(last < 0 || v == last + 1);
			last = v;
			n++;
		}
		assert(last == 299 && n == 512 / 9);
	}
	assert(!strcmp(logring_prev_reason(), "hardfault pc 1f0012ab"));
	printf("  ok  retention: lapped 512-byte tail, %d whole lines, newest reason\n", n);

	/* corrupt reason CRC: log kept, reason dropped */
	logring_puts("x\n");
	logring_set_reason("stack overflow in task app");
	area.reason[0] ^= 1;
	reboot();
	assert(logring_prev_valid() && !logring_prev_reason()[0]);
	/* corrupt magic: nothing */
	area.magic_inv ^= 0x100;
	reboot();
	assert(!logring_prev_valid());
	/* garbage bytes in a valid record are sanitized */
	logring_puts("ok\n");
	area.log[0] = 0x0a01fe7fu;      /* 7f fe 01 \n */
	reboot();
	pos = 0;
	assert(logring_prev_line(&pos, L, sizeof(L)) == 3 && !strcmp(L, "???"));
	printf("  ok  retention: bad reason CRC / bad magic rejected, bytes sanitized\n");
	logring_reset();
}

int main(void)
{
	printf("test_logring:\n");
	test_lines();
	test_long_line();
	test_wrap_and_replay();
	test_lag_drop();
	test_interleaved();
	test_threads();
	test_levels();
	test_retention();
	printf("test_logring: OK\n");
	return 0;
}
