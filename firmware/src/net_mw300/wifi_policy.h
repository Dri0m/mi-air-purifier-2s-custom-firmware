/* wifi_policy.h - reconnect / rescue-network decisions for wifi_mw300.c.
 *
 * Pure (no SDK, no RTOS): wifi_mw300.c samples the WLAN manager state every
 * net_mw300_service() call, feeds it in here, and performs the returned
 * action (wlan_connect() on the named profile).  Host-tested in tests/net/.
 *
 * Rules, checked in this order on every step:
 *   1. probe     on the backup with an IP for WP_PRIMARY_RETRY_MS
 *                -> try the primary again (drops the backup link).
 *   2. failover  no IP for WP_FAILOVER_MS on the current network
 *                (WP_PROBE_DWELL_MS while probing the primary)
 *                -> switch to the other network.
 *   3. stuck     (R7) continuously mid-connect (SCANNING / ASSOCIATING /
 *                ASSOCIATED / CONNECTING) for WP_STUCK_MS -> re-kick.
 *   4. down      DISCONNECTED for more than WP_RECONNECT_MS -> re-kick
 *                (the pre-R2 rule, unchanged).
 * Rules 1 and 2 need a backup network; without one only 3 and 4 exist.
 *
 * Rules 2 and 3 are not applied while mid-connect if WP_MAX_UNANSWERED
 * earlier kicks issued mid-connect got no WLAN event at all: the SDK's DHCP
 * busy-wait (net.c net_configure_address) blocks the manager thread, and
 * every further wlan_connect() would only pile up in its queue and be
 * replayed (dropping the link) once DHCP finally succeeds.  Any WLAN event
 * re-enables them.
 */
#ifndef WIFI_POLICY_H
#define WIFI_POLICY_H
#include <stdint.h>

#define WP_RECONNECT_MS     15000u        /* DISCONNECTED this long -> kick */
#define WP_STUCK_MS         120000u       /* mid-connect this long -> kick */
#define WP_FAILOVER_MS      120000u       /* no IP this long -> other network */
#define WP_PROBE_DWELL_MS   60000u        /* probe of the primary: give up after */
#define WP_PRIMARY_RETRY_MS (30u * 60000u)/* on the backup this long -> probe */
#define WP_MAX_UNANSWERED   2

enum wp_link {
	WP_LINK_DOWN,      /* WLAN_DISCONNECTED */
	WP_LINK_BUSY,      /* scanning / associating / associated / connecting */
	WP_LINK_UP,        /* WLAN_CONNECTED, or any other state */
};

enum wp_net { WP_PRIMARY = 0, WP_BACKUP = 1 };

enum wp_act {
	WP_NONE,
	WP_KICK_DOWN,      /* rule 4: wlan_connect(net) */
	WP_KICK_STUCK,     /* rule 3: wlan_connect(net) */
	WP_SWITCH_FAIL,    /* rule 2: wlan_connect(net), net is the new one */
	WP_SWITCH_PROBE,   /* rule 1: wlan_connect(net), net == WP_PRIMARY */
	WP_HOLD,           /* rule 2/3 due but suppressed (manager silent);
	                      returned once per silent spell, for a log line */
};

struct wp_action {
	enum wp_act act;
	enum wp_net net;   /* network to wlan_connect() (valid unless NONE/HOLD) */
};

struct wifi_policy {
	int has_backup;
	enum wp_net cur;
	int probing;               /* on the primary because of rule 1 */
	uint32_t down_since;       /* last time not DOWN, or last kick */
	uint32_t busy_since;       /* last time not BUSY, or last kick */
	uint32_t noip_since;       /* last time with an IP, or last switch */
	uint32_t backup_since;     /* when we switched to the backup */
	uint32_t last_event_seq;
	int unanswered;            /* kicks issued mid-connect, no event since */
	int last_counted;          /* the last kick was counted in unanswered */
	int hold_logged;
};

/* now_ms: a free-running millisecond clock (wrap-safe). The first connect
 * (to the primary) is issued by the caller right after this. */
void wifi_policy_init(struct wifi_policy *p, int has_backup, uint32_t now_ms);

/* event_seq: counter of WLAN manager events (any reason), bumped by the
 * WLAN callback.  ip_up: the station has an IPv4 address.  The caller must
 * act on the returned network whenever act is a KICK or SWITCH; the policy
 * already counts it as done. */
struct wp_action wifi_policy_step(struct wifi_policy *p, uint32_t now_ms,
				  enum wp_link link, int ip_up, uint32_t event_seq);

/* Report a synchronous wlan_connect() failure (no request was queued), so
 * it is not counted as an unanswered kick. */
void wifi_policy_kick_failed(struct wifi_policy *p);

const char *wifi_policy_act_str(enum wp_act a);
#endif
