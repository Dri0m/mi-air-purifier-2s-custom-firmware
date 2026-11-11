/* net_mw300.h - Wi-Fi station bring-up for the 88MW300 (used by src/app).
 *
 * Network task ("net"):
 *   net_mw300_start(ssid, pass, hostname)   once; blocks until the WLAN
 *                                            radio firmware is loaded
 *   loop: net_mw300_service()               every ~100 ms
 *
 * netio.h socket calls (API, mDNS, OTA servers) may run in a different
 * task, but all of them in the same one.  net_mw300_is_up() and
 * netio_local_ip_be()/netio_mac() are safe from any task; netio_rssi()
 * keeps unlocked statics, so call it from the API/app task only.
 */
#ifndef NET_MW300_H
#define NET_MW300_H
#include <stdint.h>

/* Loads the Wi-Fi firmware from the flash "wififw" partition (via the
 * partition table, like stock), starts the WLAN connection manager and
 * FreeRTOS+TCP, and starts connecting.  Returns 0 or a negative WMSDK
 * error (radio dead / wififw partition missing). */
int net_mw300_start(const char *ssid, const char *password, const char *hostname);

/* Housekeeping: reconnect after persistent disconnects or a connect attempt
 * stuck in progress, fail over to / back from the optional rescue network
 * (secrets.h WIFI_SSID_2/WIFI_PASSWORD_2; rules in wifi_policy.h), re-apply
 * multicast filters after (re)association.
 * Returns 1 while an IPv4 address is configured, else 0. */
int net_mw300_service(void);

/* 1 if the station has an IP address. */
int net_mw300_is_up(void);
#endif
