/* test_wifi_policy.c - host tests of the Wi-Fi reconnect / rescue policy.
 * Steps are 100 ms apart, like net_mw300_service() on the device. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wifi_policy.h"

#define STEP 100u
static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* A tiny world model: per network, whether it works.  A kick/switch makes
 * the link BUSY for connect_ms, then UP+IP if the network works, else DOWN
 * (wlcmgr reports a failure event).  Every outcome bumps the event counter
 * unless the manager is "wedged". */
struct world {
	uint32_t now;
	int works[2];
	int wedged;              /* no events, link stays BUSY */
	uint32_t connect_ms;
	enum wp_link link;
	int ip;
	uint32_t busy_until;
	enum wp_net on;
	uint32_t events;
	/* recorded */
	int n_act[WP_HOLD + 1];
	uint32_t last_t[WP_HOLD + 1];
	enum wp_net last_net;
};

static void world_connect(struct world *w, enum wp_net n)
{
	w->on = n;
	w->link = WP_LINK_BUSY;
	w->ip = 0;
	w->busy_until = w->now + w->connect_ms;
	if (!w->wedged)
		w->events++;     /* USER_DISCONNECT / cancel */
}

static void world_tick(struct world *w)
{
	if (w->link == WP_LINK_BUSY && !w->wedged &&
	    (int32_t)(w->now - w->busy_until) >= 0) {
		w->events++;
		if (w->works[w->on]) {
			w->link = WP_LINK_UP;
			w->ip = 1;
		} else {
			w->link = WP_LINK_DOWN;
		}
	}
	if (w->link == WP_LINK_UP && !w->works[w->on]) {   /* AP vanished */
		w->events++;
		w->link = WP_LINK_DOWN;
		w->ip = 0;
	}
}

static void run(struct wifi_policy *p, struct world *w, uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += STEP) {
		struct wp_action a;

		w->now += STEP;
		world_tick(w);
		a = wifi_policy_step(p, w->now, w->link, w->ip, w->events);
		w->n_act[a.act]++;
		w->last_t[a.act] = w->now;
		if (a.act != WP_NONE && a.act != WP_HOLD) {
			w->last_net = a.net;
			world_connect(w, a.net);
		}
	}
}

static void world_init(struct world *w, struct wifi_policy *p, int backup,
		       int w0, int w1, uint32_t t0)
{
	memset(w, 0, sizeof(*w));
	w->now = t0;
	w->works[0] = w0;
	w->works[1] = w1;
	w->connect_ms = 5000;
	wifi_policy_init(p, backup, t0);
	world_connect(w, WP_PRIMARY);      /* net_mw300_start's first connect */
}

static void clear_counts(struct world *w)
{
	memset(w->n_act, 0, sizeof(w->n_act));
}

/* ---- the pre-R2 rule, verbatim in effect ---- */
struct legacy { uint32_t since; };
static int legacy_step(struct legacy *l, uint32_t now, enum wp_link link)
{
	if (link != WP_LINK_DOWN) {
		l->since = now;
	} else if (now - l->since > WP_RECONNECT_MS) {
		l->since = now;
		return 1;
	}
	return 0;
}

/* No backup: identical decisions to the old code for any sequence of
 * states in which no mid-connect spell reaches WP_STUCK_MS (the R7 rule is
 * the only intended difference), whatever the IP flag says. */
static void test_no_backup_equivalence(void)
{
	for (int seed = 1; seed <= 200; seed++) {
		struct wifi_policy p;
		struct legacy l;
		uint32_t t0 = (seed & 1) ? 0xFFFF0000u : 1000u * (uint32_t)seed;
		uint32_t now = t0, busy_run = 0, ev = 0;
		enum wp_link link = WP_LINK_DOWN;

		srand((unsigned)seed);
		wifi_policy_init(&p, 0, t0);
		l.since = t0;
		for (int i = 0; i < 200000; i++) {
			int r = rand() % 1000;
			struct wp_action a;
			int ip;

			now += STEP;
			if (r < 3)
				link = WP_LINK_DOWN;
			else if (r < 6)
				link = WP_LINK_BUSY;
			else if (r < 8)
				link = WP_LINK_UP;
			if (link == WP_LINK_BUSY && busy_run >= WP_STUCK_MS - 10 * STEP)
				link = WP_LINK_UP;
			busy_run = link == WP_LINK_BUSY ? busy_run + STEP : 0;
			if (r % 7 == 0)
				ev++;
			ip = (r % 3 == 0);
			a = wifi_policy_step(&p, now, link, ip, ev);
			int old = legacy_step(&l, now, link);
			CHECK((a.act == WP_KICK_DOWN) == old);
			CHECK(a.act == WP_NONE || a.act == WP_KICK_DOWN);
			CHECK(a.net == WP_PRIMARY);
			if (fails > 20)
				return;
		}
	}
}

static void test_no_backup_dead_primary(void)
{
	struct wifi_policy p;
	struct world w;

	world_init(&w, &p, 0, 0, 1, 0);
	run(&p, &w, 3600000u);
	CHECK(w.n_act[WP_SWITCH_FAIL] == 0);
	CHECK(w.n_act[WP_SWITCH_PROBE] == 0);
	CHECK(w.n_act[WP_KICK_STUCK] == 0);
	CHECK(w.n_act[WP_KICK_DOWN] > 100);    /* every 5 s busy + 15 s idle */
	CHECK(w.on == WP_PRIMARY);
}

static void test_backup_rescue(void)
{
	struct wifi_policy p;
	struct world w;

	/* Primary dead, backup works: switch at 120 s, then stay. */
	world_init(&w, &p, 1, 0, 1, 0);
	run(&p, &w, WP_FAILOVER_MS - STEP);
	CHECK(w.n_act[WP_SWITCH_FAIL] == 0);
	run(&p, &w, STEP);
	CHECK(w.n_act[WP_SWITCH_FAIL] == 1);
	CHECK(w.last_net == WP_BACKUP);
	run(&p, &w, 10000);
	CHECK(w.ip == 1 && w.on == WP_BACKUP);

	/* Probe the primary after 30 min on the backup; it fails, back to
	 * the backup after WP_PROBE_DWELL_MS. */
	clear_counts(&w);
	run(&p, &w, WP_PRIMARY_RETRY_MS - 10000 - STEP);
	CHECK(w.n_act[WP_SWITCH_PROBE] == 0);
	run(&p, &w, STEP);
	CHECK(w.n_act[WP_SWITCH_PROBE] == 1 && w.on == WP_PRIMARY);
	uint32_t probe_t = w.now;
	run(&p, &w, WP_PROBE_DWELL_MS);
	CHECK(w.n_act[WP_SWITCH_FAIL] == 1 && w.on == WP_BACKUP);
	CHECK(w.last_t[WP_SWITCH_FAIL] - probe_t == WP_PROBE_DWELL_MS);
	run(&p, &w, 10000);
	CHECK(w.ip == 1);

	/* Primary fixed: the next probe sticks. */
	w.works[0] = 1;
	clear_counts(&w);
	run(&p, &w, WP_PRIMARY_RETRY_MS);
	CHECK(w.n_act[WP_SWITCH_PROBE] == 1 && w.on == WP_PRIMARY && w.ip == 1);
	clear_counts(&w);
	run(&p, &w, 4 * 3600000u);
	CHECK(w.n_act[WP_SWITCH_PROBE] == 0 && w.n_act[WP_SWITCH_FAIL] == 0);
	CHECK(w.n_act[WP_KICK_DOWN] == 0 && w.n_act[WP_KICK_STUCK] == 0);
	CHECK(w.on == WP_PRIMARY);

	/* A later primary outage gets the normal failover time, not the
	 * probe dwell. */
	w.works[0] = 0;
	uint32_t lost = w.now;         /* last step with an IP */
	clear_counts(&w);
	run(&p, &w, WP_FAILOVER_MS + 10000);
	CHECK(w.n_act[WP_SWITCH_FAIL] == 1);
	CHECK(w.last_t[WP_SWITCH_FAIL] - lost == WP_FAILOVER_MS);
}

static void test_both_dead_alternate(void)
{
	struct wifi_policy p;
	struct world w;

	world_init(&w, &p, 1, 0, 0, 0);
	run(&p, &w, 10 * WP_FAILOVER_MS);
	CHECK(w.n_act[WP_SWITCH_FAIL] == 10);
	CHECK(w.n_act[WP_SWITCH_PROBE] == 0);
	CHECK(w.on == WP_PRIMARY);              /* even number of switches */
	/* Primary returns: found within one failover period. */
	w.works[0] = 1;
	clear_counts(&w);
	run(&p, &w, WP_FAILOVER_MS + 20000);
	CHECK(w.on == WP_PRIMARY && w.ip == 1);
}

static void test_primary_never_left_when_working(void)
{
	struct wifi_policy p;
	struct world w;

	world_init(&w, &p, 1, 1, 1, 0);
	run(&p, &w, 24 * 3600000u);
	CHECK(w.on == WP_PRIMARY && w.ip == 1);
	CHECK(w.n_act[WP_SWITCH_FAIL] == 0 && w.n_act[WP_SWITCH_PROBE] == 0);
}

static void test_short_primary_outage(void)
{
	struct wifi_policy p;
	struct world w;

	/* Router reboot of 60 s: no failover (backup exists, but works). */
	world_init(&w, &p, 1, 1, 1, 0);
	run(&p, &w, 60000);
	w.works[0] = 0;
	run(&p, &w, 60000);
	w.works[0] = 1;
	run(&p, &w, 60000);
	CHECK(w.n_act[WP_SWITCH_FAIL] == 0 && w.on == WP_PRIMARY && w.ip == 1);
}

static void test_backup_drops_back_to_primary(void)
{
	struct wifi_policy p;
	struct world w;

	world_init(&w, &p, 1, 0, 1, 0);
	run(&p, &w, WP_FAILOVER_MS + 10000);
	CHECK(w.on == WP_BACKUP && w.ip == 1);
	/* User fixes the primary (OTA) and turns the hotspot off. */
	w.works[0] = 1;
	w.works[1] = 0;
	clear_counts(&w);
	run(&p, &w, WP_FAILOVER_MS + 10000);
	CHECK(w.n_act[WP_SWITCH_FAIL] == 1 && w.on == WP_PRIMARY && w.ip == 1);
}

/* R7: stuck mid-connect gets re-kicked; a silent manager gets at most
 * WP_MAX_UNANSWERED kicks, then one HOLD, until an event arrives. */
static void test_stuck_and_hold(void)
{
	struct wifi_policy p;
	uint32_t now = 0, ev = 5;
	struct wp_action a;
	int kicks = 0, holds = 0;
	uint32_t first = 0;

	wifi_policy_init(&p, 0, now);
	for (int i = 0; i < 20000; i++) {          /* 2000 s BUSY, no events */
		now += STEP;
		a = wifi_policy_step(&p, now, WP_LINK_BUSY, 0, ev);
		if (a.act == WP_KICK_STUCK && !kicks++)
			first = now;
		holds += a.act == WP_HOLD;
		CHECK(a.act == WP_NONE || a.act == WP_KICK_STUCK || a.act == WP_HOLD);
	}
	CHECK(first == WP_STUCK_MS);
	CHECK(kicks == WP_MAX_UNANSWERED);
	CHECK(holds == 1);
	/* An event re-enables it immediately. */
	ev++;
	now += STEP;
	a = wifi_policy_step(&p, now, WP_LINK_BUSY, 0, ev);
	CHECK(a.act == WP_KICK_STUCK);

	/* A synchronous wlan_connect() failure is not counted. */
	wifi_policy_init(&p, 0, 0);
	now = 0;
	kicks = 0;
	for (int i = 0; i < 20000; i++) {
		now += STEP;
		a = wifi_policy_step(&p, now, WP_LINK_BUSY, 0, 0);
		if (a.act == WP_KICK_STUCK) {
			kicks++;
			wifi_policy_kick_failed(&p);
		}
	}
	CHECK(kicks == (int)(2000000u / WP_STUCK_MS));

	/* DISCONNECTED kicks are never held (pre-R2 behaviour). */
	wifi_policy_init(&p, 0, 0);
	now = 0;
	kicks = 0;
	for (int i = 0; i < 20000; i++) {
		now += STEP;
		a = wifi_policy_step(&p, now, WP_LINK_DOWN, 0, 0);
		kicks += a.act == WP_KICK_DOWN;
		CHECK(a.act != WP_HOLD);
	}
	CHECK(kicks >= 100);
}

/* Wedged manager with a backup: failover kicks are capped too. */
/* Hardware 2026-11-04: wlcmgr reported CONNECTING for minutes with an IP
 * up; the re-kick must never fire then (with or without a backup), and a
 * BUSY spell right after the IP drops starts its 120 s from that moment. */
static void test_busy_with_ip_never_kicked(void)
{
	for (int backup = 0; backup <= 1; backup++) {
		struct wifi_policy p;
		uint32_t now = 0;
		struct wp_action a;
		int bad = 0;

		wifi_policy_init(&p, backup, now);
		for (int i = 0; i < 20000; i++) {  /* 2000 s BUSY + IP */
			now += STEP;
			a = wifi_policy_step(&p, now, WP_LINK_BUSY, 1, 7);
			bad += a.act != WP_NONE;
		}
		CHECK(bad == 0);
		uint32_t lost = now, first = 0;
		for (int i = 0; i < 1300 && !first; i++) {
			now += STEP;
			a = wifi_policy_step(&p, now, WP_LINK_BUSY, 0, 7);
			if (a.act == WP_KICK_STUCK)
				first = now;
		}
		CHECK(first == lost + WP_STUCK_MS || (backup && first == 0));
	}
}

static void test_wedged_with_backup(void)
{
	struct wifi_policy p;
	struct world w;

	world_init(&w, &p, 1, 0, 1, 0);
	w.wedged = 1;
	run(&p, &w, 3600000u);
	CHECK(w.n_act[WP_SWITCH_FAIL] + w.n_act[WP_KICK_STUCK] == WP_MAX_UNANSWERED);
	CHECK(w.n_act[WP_HOLD] == 1);
}

static void test_clock_wrap(void)
{
	struct wifi_policy p;
	struct world w;

	world_init(&w, &p, 1, 0, 1, 0xFFFFFFFFu - 60000u);
	run(&p, &w, WP_FAILOVER_MS);
	CHECK(w.n_act[WP_SWITCH_FAIL] == 1 && w.last_t[WP_SWITCH_FAIL] == 0xFFFFFFFFu - 60000u + WP_FAILOVER_MS);
	run(&p, &w, 10000);
	CHECK(w.on == WP_BACKUP && w.ip == 1);
}

int main(void)
{
	test_no_backup_equivalence();
	test_no_backup_dead_primary();
	test_backup_rescue();
	test_both_dead_alternate();
	test_primary_never_left_when_working();
	test_short_primary_outage();
	test_backup_drops_back_to_primary();
	test_stuck_and_hold();
	test_wedged_with_backup();
	test_clock_wrap();
	test_busy_with_ip_never_kicked();
	if (fails) {
		fprintf(stderr, "test_wifi_policy: %d FAILED\n", fails);
		return 1;
	}
	printf("test_wifi_policy: PASS\n");
	return 0;
}
