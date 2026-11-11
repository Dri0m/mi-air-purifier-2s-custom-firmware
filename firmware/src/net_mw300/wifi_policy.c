/* wifi_policy.c - see wifi_policy.h.  Pure C, no SDK/RTOS dependencies. */
#include "wifi_policy.h"

#define ELAPSED(now, t) ((uint32_t)((now) - (t)))

void wifi_policy_init(struct wifi_policy *p, int has_backup, uint32_t now_ms)
{
	p->has_backup = has_backup ? 1 : 0;
	p->cur = WP_PRIMARY;
	p->probing = 0;
	p->down_since = now_ms;
	p->busy_since = now_ms;
	p->noip_since = now_ms;
	p->backup_since = now_ms;
	p->last_event_seq = 0;
	p->unanswered = 0;
	p->last_counted = 0;
	p->hold_logged = 0;
}

static struct wp_action kick(struct wifi_policy *p, enum wp_act a,
			     uint32_t now, enum wp_link link)
{
	struct wp_action r = { a, p->cur };

	p->down_since = now;
	p->busy_since = now;
	p->last_counted = (link == WP_LINK_BUSY);
	p->unanswered += p->last_counted;
	return r;
}

static struct wp_action switch_to(struct wifi_policy *p, enum wp_net n,
				  enum wp_act a, uint32_t now, enum wp_link link)
{
	p->cur = n;
	p->noip_since = now;
	p->probing = (a == WP_SWITCH_PROBE);
	if (n == WP_BACKUP)
		p->backup_since = now;
	return kick(p, a, now, link);
}

struct wp_action wifi_policy_step(struct wifi_policy *p, uint32_t now,
				  enum wp_link link, int ip_up, uint32_t event_seq)
{
	struct wp_action none = { WP_NONE, p->cur };
	int held;

	if (event_seq != p->last_event_seq) {
		p->last_event_seq = event_seq;
		p->unanswered = 0;
		p->hold_logged = 0;
	}
	if (link != WP_LINK_DOWN)
		p->down_since = now;
	/* An IP means the data path works: never "stuck", whatever wlcmgr
	 * reports.  Seen on hardware (2026-11-04): wlcmgr stayed CONNECTING
	 * for minutes after DHCP succeeded, so the R7 re-kick deauthenticated
	 * a working link ~120 s after every boot. */
	if (link != WP_LINK_BUSY || ip_up)
		p->busy_since = now;
	if (ip_up) {
		p->noip_since = now;
		if (p->cur == WP_PRIMARY)
			p->probing = 0;      /* primary works again: stay */
	}

	/* 1. probe the primary from a working backup. */
	if (p->has_backup && p->cur == WP_BACKUP && ip_up &&
	    ELAPSED(now, p->backup_since) >= WP_PRIMARY_RETRY_MS)
		return switch_to(p, WP_PRIMARY, WP_SWITCH_PROBE, now, link);

	held = (link == WP_LINK_BUSY && p->unanswered >= WP_MAX_UNANSWERED);

	/* 2. failover after a while without an IP. */
	if (p->has_backup && !ip_up &&
	    ELAPSED(now, p->noip_since) >= (p->probing ? WP_PROBE_DWELL_MS : WP_FAILOVER_MS)) {
		if (!held)
			return switch_to(p, p->cur == WP_PRIMARY ? WP_BACKUP : WP_PRIMARY,
					 WP_SWITCH_FAIL, now, link);
		goto hold;
	}

	/* 3. (R7) stuck mid-connect. */
	if (link == WP_LINK_BUSY && ELAPSED(now, p->busy_since) >= WP_STUCK_MS) {
		if (!held)
			return kick(p, WP_KICK_STUCK, now, link);
		goto hold;
	}

	/* 4. disconnected and idle (the original rule, same '>' comparison). */
	if (link == WP_LINK_DOWN && ELAPSED(now, p->down_since) > WP_RECONNECT_MS)
		return kick(p, WP_KICK_DOWN, now, link);

	return none;

hold:
	if (!p->hold_logged) {
		p->hold_logged = 1;
		none.act = WP_HOLD;
	}
	return none;
}

void wifi_policy_kick_failed(struct wifi_policy *p)
{
	if (p->last_counted && p->unanswered > 0)
		p->unanswered--;
	p->last_counted = 0;
}

const char *wifi_policy_act_str(enum wp_act a)
{
	switch (a) {
	case WP_KICK_DOWN:    return "reconnecting";
	case WP_KICK_STUCK:   return "connect stuck, re-kicking";
	case WP_SWITCH_FAIL:  return "no IP, switching network";
	case WP_SWITCH_PROBE: return "retrying the primary network";
	case WP_HOLD:         return "wlan manager silent, holding re-kicks";
	default:              return "none";
	}
}
